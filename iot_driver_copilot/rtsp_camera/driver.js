const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { Readable } = require('stream');
const { parse } = require('querystring');

// Environment variables
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT, 10) || 8080;
const RTSP_CAMERA_IP = process.env.RTSP_CAMERA_IP || '127.0.0.1';
const RTSP_CAMERA_PORT = process.env.RTSP_CAMERA_PORT || '554';
const RTSP_USERNAME = process.env.RTSP_USERNAME || '';
const RTSP_PASSWORD = process.env.RTSP_PASSWORD || '';
const RTSP_PATH = process.env.RTSP_PATH || '/stream';
const RTSP_VIDEO_CODEC = process.env.RTSP_VIDEO_CODEC || 'h264'; // can be h264, h265, mjpeg
const RTSP_AUDIO_CODEC = process.env.RTSP_AUDIO_CODEC || 'aac';  // can be aac, g711

// State
let isStreaming = false;
let currentVideoStream = null;
let currentAudioStream = null;
let videoClients = [];
let audioClients = [];

function buildRtspUrl() {
    let credentials = '';
    if (RTSP_USERNAME && RTSP_PASSWORD) {
        credentials = `${encodeURIComponent(RTSP_USERNAME)}:${encodeURIComponent(RTSP_PASSWORD)}@`;
    }
    return `rtsp://${credentials}${RTSP_CAMERA_IP}:${RTSP_CAMERA_PORT}${RTSP_PATH}`;
}

// Helper to start FFmpeg as a streaming proxy for video (returns ffmpeg process and readable stream)
function startVideoStream() {
    const rtspUrl = buildRtspUrl();
    let codecs = {
        'h264': 'copy',
        'h265': 'copy',
        'mjpeg': 'copy'
    };
    const videoCodec = RTSP_VIDEO_CODEC.toLowerCase();
    // We'll output to MP4 over HTTP for browser consumption (video/mp4)
    const ffmpegArgs = [
        '-rtsp_transport', 'tcp',
        '-i', rtspUrl,
        '-an', // no audio
        '-c:v', codecs[videoCodec] || 'copy',
        '-f', 'mp4',
        '-movflags', 'frag_keyframe+empty_moov+default_base_moof',
        '-reset_timestamps', '1',
        'pipe:1'
    ];
    const ffmpeg = spawn('ffmpeg', ffmpegArgs, { stdio: ['ignore', 'pipe', 'inherit'] });
    return ffmpeg;
}

// Helper to start FFmpeg as a streaming proxy for audio (returns ffmpeg process and readable stream)
function startAudioStream() {
    const rtspUrl = buildRtspUrl();
    let codecs = {
        'aac': 'aac',
        'g711': 'pcm_alaw'
    };
    const audioCodec = RTSP_AUDIO_CODEC.toLowerCase();
    // We'll output to ADTS AAC for browser/cli consumption (audio/aac)
    const ffmpegArgs = [
        '-rtsp_transport', 'tcp',
        '-i', rtspUrl,
        '-vn', // no video
        '-c:a', codecs[audioCodec] || 'aac',
        '-f', audioCodec === 'g711' ? 'alaw' : 'adts',
        'pipe:1'
    ];
    const ffmpeg = spawn('ffmpeg', ffmpegArgs, { stdio: ['ignore', 'pipe', 'inherit'] });
    return ffmpeg;
}

function stopVideoStream() {
    if (currentVideoStream) {
        currentVideoStream.kill('SIGTERM');
        currentVideoStream = null;
    }
    videoClients.forEach(res => {
        try { res.end(); } catch {}
    });
    videoClients = [];
}

function stopAudioStream() {
    if (currentAudioStream) {
        currentAudioStream.kill('SIGTERM');
        currentAudioStream = null;
    }
    audioClients.forEach(res => {
        try { res.end(); } catch {}
    });
    audioClients = [];
}

function handleStartStream(res) {
    if (!isStreaming) {
        // Start both video and audio streams
        currentVideoStream = startVideoStream();
        currentAudioStream = startAudioStream();
        isStreaming = true;
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'started', message: 'RTSP stream started.' }));
    } else {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'already_running', message: 'RTSP stream is already running.' }));
    }
}

function handleStopStream(res) {
    if (isStreaming) {
        stopVideoStream();
        stopAudioStream();
        isStreaming = false;
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'stopped', message: 'RTSP stream stopped.' }));
    } else {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'not_running', message: 'RTSP stream was not running.' }));
    }
}

function handleStreamStatus(res) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        active: isStreaming,
        video_codec: RTSP_VIDEO_CODEC,
        audio_codec: RTSP_AUDIO_CODEC,
        rtsp_url: buildRtspUrl()
    }));
}

function handleVideoStream(req, res) {
    if (!isStreaming) {
        res.writeHead(503, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'not_running', message: 'Stream not running.' }));
        return;
    }

    res.writeHead(200, {
        'Content-Type': 'video/mp4',
        'Connection': 'close',
        'Accept-Ranges': 'none',
        'Cache-Control': 'no-cache'
    });
    videoClients.push(res);

    if (currentVideoStream && currentVideoStream.stdout) {
        currentVideoStream.stdout.pipe(res);
        res.on('close', () => {
            try { currentVideoStream.stdout.unpipe(res); } catch {}
            videoClients = videoClients.filter(client => client !== res);
        });
    } else {
        res.end();
    }
}

function handleAudioStream(req, res) {
    if (!isStreaming) {
        res.writeHead(503, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'not_running', message: 'Stream not running.' }));
        return;
    }
    // For AAC: audio/aac, for G711: audio/basic (as close as possible)
    const audioContentType = RTSP_AUDIO_CODEC.toLowerCase() === 'g711' ? 'audio/basic' : 'audio/aac';

    res.writeHead(200, {
        'Content-Type': audioContentType,
        'Connection': 'close',
        'Accept-Ranges': 'none',
        'Cache-Control': 'no-cache'
    });
    audioClients.push(res);

    if (currentAudioStream && currentAudioStream.stdout) {
        currentAudioStream.stdout.pipe(res);
        res.on('close', () => {
            try { currentAudioStream.stdout.unpipe(res); } catch {}
            audioClients = audioClients.filter(client => client !== res);
        });
    } else {
        res.end();
    }
}

// Accept only POST and JSON for /cmd/start and /commands/start
function parseBody(req, callback) {
    let body = '';
    req.on('data', chunk => { body += chunk.toString(); });
    req.on('end', () => {
        try {
            callback(JSON.parse(body));
        } catch {
            callback({});
        }
    });
}

const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    if (req.method === 'POST' && (parsedUrl.pathname === '/commands/start' || parsedUrl.pathname === '/cmd/start')) {
        handleStartStream(res);
    } else if (req.method === 'POST' && (parsedUrl.pathname === '/commands/stop' || parsedUrl.pathname === '/cmd/stop')) {
        handleStopStream(res);
    } else if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
        handleStreamStatus(res);
    } else if (req.method === 'GET' && parsedUrl.pathname === '/streams/video') {
        // Return stream info as JSON, not the actual video stream (for browser/cli)
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            type: 'video',
            url: '/streams/video/feed',
            format: 'mp4',
            status: isStreaming ? 'active' : 'inactive'
        }));
    } else if (req.method === 'GET' && parsedUrl.pathname === '/streams/audio') {
        // Return stream info as JSON, not the actual audio stream (for browser/cli)
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            type: 'audio',
            url: '/streams/audio/feed',
            format: RTSP_AUDIO_CODEC.toLowerCase() === 'g711' ? 'alaw' : 'aac',
            status: isStreaming ? 'active' : 'inactive'
        }));
    } else if (req.method === 'GET' && parsedUrl.pathname === '/streams/video/feed') {
        handleVideoStream(req, res);
    } else if (req.method === 'GET' && parsedUrl.pathname === '/streams/audio/feed') {
        handleAudioStream(req, res);
    } else {
        res.writeHead(404, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'Not found' }));
    }
});

process.on('SIGINT', () => {
    stopVideoStream();
    stopAudioStream();
    process.exit(0);
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
    console.log(`RTSP Camera HTTP Proxy listening on http://${SERVER_HOST}:${SERVER_PORT}`);
});