const express = require('express');
const { RTSPClient, H264Transport, AACTransport } = require('rtsp-client');
const { PassThrough } = require('stream');

// Environment Variables
const RTSP_URL = process.env.RTSP_URL || '';
const RTSP_USERNAME = process.env.RTSP_USERNAME || '';
const RTSP_PASSWORD = process.env.RTSP_PASSWORD || '';
const RTSP_IP = process.env.RTSP_IP || '';
const RTSP_PORT = process.env.RTSP_PORT || '554';
const HTTP_HOST = process.env.HTTP_HOST || '0.0.0.0';
const HTTP_PORT = process.env.HTTP_PORT || 8080;
const RTSP_PATH = process.env.RTSP_PATH || '/stream';

const VIDEO_FORMAT = process.env.VIDEO_FORMAT || 'H264'; // or H265, MJPEG
const AUDIO_FORMAT = process.env.AUDIO_FORMAT || 'AAC'; // or G711

// Compose RTSP URL if not directly given
const composeRtspUrl = () => {
    if (RTSP_URL) return RTSP_URL;
    let auth = '';
    if (RTSP_USERNAME && RTSP_PASSWORD) {
        auth = encodeURIComponent(RTSP_USERNAME) + ':' + encodeURIComponent(RTSP_PASSWORD) + '@';
    }
    return `rtsp://${auth}${RTSP_IP}:${RTSP_PORT}${RTSP_PATH}`;
};

let streamActive = false;
let videoStream = null;
let audioStream = null;
let client = null;

// Utility: Start RTSP client and stream
async function startRtspStreams() {
    if (streamActive) return;
    client = new RTSPClient();
    await client.connect(composeRtspUrl(), {
        connection: { username: RTSP_USERNAME, password: RTSP_PASSWORD }
    });

    // Setup video
    let videoPt = new PassThrough();
    let audioPt = new PassThrough();

    client.on('data', (channel, data, packet) => {
        if (packet && packet.type === 'video') {
            videoPt.write(packet.data);
        }
        if (packet && packet.type === 'audio') {
            audioPt.write(packet.data);
        }
    });

    // Setup transports
    let transports = [];
    if (VIDEO_FORMAT === 'H264' || VIDEO_FORMAT === 'H265' || VIDEO_FORMAT === 'MJPEG') {
        transports.push(new H264Transport());
    }
    if (AUDIO_FORMAT === 'AAC') {
        transports.push(new AACTransport());
    }
    await client.play(transports);

    videoStream = videoPt;
    audioStream = audioPt;
    streamActive = true;
}

// Utility: Stop RTSP client and streams
async function stopRtspStreams() {
    streamActive = false;
    if (client) {
        await client.close();
        client = null;
    }
    if (videoStream) {
        videoStream.end();
        videoStream = null;
    }
    if (audioStream) {
        audioStream.end();
        audioStream = null;
    }
}

// Express setup
const app = express();
app.use(express.json());

// Start stream command (POST /commands/start, /cmd/start)
app.post(['/commands/start', '/cmd/start'], async (req, res) => {
    try {
        await startRtspStreams();
        res.json({ status: 'started', active: streamActive });
    } catch (e) {
        res.status(500).json({ status: 'error', message: e.message });
    }
});

// Stop stream command (POST /commands/stop, /cmd/stop)
app.post(['/commands/stop', '/cmd/stop'], async (req, res) => {
    try {
        await stopRtspStreams();
        res.json({ status: 'stopped', active: streamActive });
    } catch (e) {
        res.status(500).json({ status: 'error', message: e.message });
    }
});

// Streaming status (GET /stream)
app.get('/stream', (req, res) => {
    res.json({
        active: streamActive,
        video: {
            format: VIDEO_FORMAT,
            endpoint: '/streams/video'
        },
        audio: {
            format: AUDIO_FORMAT,
            endpoint: '/streams/audio'
        },
        rtsp_url: composeRtspUrl()
    });
});

// Stream video (GET /streams/video)
app.get('/streams/video', async (req, res) => {
    try {
        if (!streamActive) await startRtspStreams();
        res.setHeader('Content-Type', 'video/mp4');
        // For browser: Use MP4 fragment, but here raw stream, adjust as needed for real browser support
        videoStream.pipe(res);
        req.on('close', () => {
            videoStream.unpipe(res);
        });
    } catch (e) {
        res.status(500).json({ status: 'error', message: e.message });
    }
});

// Stream audio (GET /streams/audio)
app.get('/streams/audio', async (req, res) => {
    try {
        if (!streamActive) await startRtspStreams();
        res.setHeader('Content-type', AUDIO_FORMAT === 'AAC' ? 'audio/aac' : 'audio/basic');
        audioStream.pipe(res);
        req.on('close', () => {
            audioStream.unpipe(res);
        });
    } catch (e) {
        res.status(500).json({ status: 'error', message: e.message });
    }
});

// Stream info (GET /streams/video - JSON endpoint)
app.get('/streams/video', (req, res) => {
    res.json({
        active: streamActive,
        format: VIDEO_FORMAT,
        endpoint: '/streams/video',
        rtsp_url: composeRtspUrl()
    });
});

// Stream info (GET /streams/audio - JSON endpoint)
app.get('/streams/audio', (req, res) => {
    res.json({
        active: streamActive,
        format: AUDIO_FORMAT,
        endpoint: '/streams/audio',
        rtsp_url: composeRtspUrl()
    });
});

app.listen(HTTP_PORT, HTTP_HOST, () => {
    console.log(`RTSP Camera Driver HTTP server running at http://${HTTP_HOST}:${HTTP_PORT}/`);
});