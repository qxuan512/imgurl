const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { Readable, PassThrough } = require('stream');

// Environment Variables
const CAMERA_HOST = process.env.CAMERA_HOST || '127.0.0.1';
const CAMERA_PORT = process.env.CAMERA_PORT || '554';
const CAMERA_RTSP_PATH = process.env.CAMERA_RTSP_PATH || '/stream';
const CAMERA_USER = process.env.CAMERA_USER || '';
const CAMERA_PASS = process.env.CAMERA_PASS || '';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT || '8080', 10);
const HTTP_VIDEO_STREAM_PORT = parseInt(process.env.HTTP_VIDEO_STREAM_PORT || SERVER_PORT, 10);
const HTTP_AUDIO_STREAM_PORT = parseInt(process.env.HTTP_AUDIO_STREAM_PORT || SERVER_PORT, 10);

let streamActive = false;
let ffmpegVideoProcess = null;
let ffmpegAudioProcess = null;
let videoClients = [];
let audioClients = [];

function buildRTSPUrl() {
    if (CAMERA_USER && CAMERA_PASS) {
        return `rtsp://${CAMERA_USER}:${CAMERA_PASS}@${CAMERA_HOST}:${CAMERA_PORT}${CAMERA_RTSP_PATH}`;
    } else if (CAMERA_USER) {
        return `rtsp://${CAMERA_USER}@${CAMERA_HOST}:${CAMERA_PORT}${CAMERA_RTSP_PATH}`;
    } else {
        return `rtsp://${CAMERA_HOST}:${CAMERA_PORT}${CAMERA_RTSP_PATH}`;
    }
}

// Start Video Streaming
function startVideoStream() {
    if (ffmpegVideoProcess) return;
    const rtspUrl = buildRTSPUrl();
    // MJPEG over HTTP for browser compatibility
    ffmpegVideoProcess = spawn('ffmpeg', [
        '-rtsp_transport', 'tcp',
        '-i', rtspUrl,
        '-an',
        '-c:v', 'mjpeg',
        '-q:v', '5',
        '-f', 'mjpeg',
        'pipe:1'
    ]);
    ffmpegVideoProcess.stdout.on('data', (chunk) => {
        videoClients.forEach((res) => {
            res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
            res.write(chunk);
            res.write('\r\n');
        });
    });
    ffmpegVideoProcess.stderr.on('data', () => { /* Ignore ffmpeg logs */ });
    ffmpegVideoProcess.on('close', () => {
        ffmpegVideoProcess = null;
        videoClients.forEach((res) => {
            try { res.end(); } catch {}
        });
        videoClients = [];
    });
}

// Stop Video Streaming
function stopVideoStream() {
    if (ffmpegVideoProcess) {
        ffmpegVideoProcess.kill('SIGTERM');
        ffmpegVideoProcess = null;
    }
}

// Start Audio Streaming
function startAudioStream() {
    if (ffmpegAudioProcess) return;
    const rtspUrl = buildRTSPUrl();
    // Raw PCM audio for browser/command line access (WAV header)
    ffmpegAudioProcess = spawn('ffmpeg', [
        '-rtsp_transport', 'tcp',
        '-i', rtspUrl,
        '-vn',
        '-acodec', 'pcm_s16le',
        '-ar', '44100',
        '-ac', '2',
        '-f', 'wav',
        'pipe:1'
    ]);
    ffmpegAudioProcess.stdout.on('data', (chunk) => {
        audioClients.forEach((res) => {
            res.write(chunk);
        });
    });
    ffmpegAudioProcess.stderr.on('data', () => { /* Ignore ffmpeg logs */ });
    ffmpegAudioProcess.on('close', () => {
        ffmpegAudioProcess = null;
        audioClients.forEach((res) => {
            try { res.end(); } catch {}
        });
        audioClients = [];
    });
}

// Stop Audio Streaming
function stopAudioStream() {
    if (ffmpegAudioProcess) {
        ffmpegAudioProcess.kill('SIGTERM');
        ffmpegAudioProcess = null;
    }
}

// HTTP Server
const server = http.createServer(async (req, res) => {
    const parsedUrl = url.parse(req.url, true);
    const { pathname } = parsedUrl;

    // Helper: JSON reply
    function replyJson(obj, status = 200) {
        res.writeHead(status, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(obj));
    }

    // POST /commands/start and /cmd/start
    if (
        (req.method === 'POST' && pathname === '/commands/start') ||
        (req.method === 'POST' && pathname === '/cmd/start')
    ) {
        streamActive = true;
        startVideoStream();
        startAudioStream();
        replyJson({ status: 'ok', message: 'Streams started' });
        return;
    }

    // POST /commands/stop and /cmd/stop
    if (
        (req.method === 'POST' && pathname === '/commands/stop') ||
        (req.method === 'POST' && pathname === '/cmd/stop')
    ) {
        streamActive = false;
        stopVideoStream();
        stopAudioStream();
        replyJson({ status: 'ok', message: 'Streams stopped' });
        return;
    }

    // GET /stream -- status
    if (req.method === 'GET' && pathname === '/stream') {
        replyJson({
            active: streamActive,
            video: {
                format: 'MJPEG',
                http_url: `http://${SERVER_HOST}:${HTTP_VIDEO_STREAM_PORT}/streams/video`,
            },
            audio: {
                format: 'WAV (PCM 44.1kHz 2ch)',
                http_url: `http://${SERVER_HOST}:${HTTP_AUDIO_STREAM_PORT}/streams/audio`,
            },
            rtsp_url: buildRTSPUrl()
        });
        return;
    }

    // GET /streams/video -- MJPEG HTTP streaming
    if (req.method === 'GET' && pathname === '/streams/video') {
        res.writeHead(200, {
            'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
            'Connection': 'close',
            'Cache-Control': 'no-cache, no-store, must-revalidate',
            'Pragma': 'no-cache',
            'Expires': '0'
        });
        videoClients.push(res);
        if (streamActive) {
            startVideoStream();
        }

        req.on('close', () => {
            videoClients = videoClients.filter((client) => client !== res);
            if (videoClients.length === 0 && ffmpegVideoProcess) {
                stopVideoStream();
            }
        });
        return;
    }

    // GET /streams/audio -- WAV HTTP streaming
    if (req.method === 'GET' && pathname === '/streams/audio') {
        res.writeHead(200, {
            'Content-Type': 'audio/wav',
            'Connection': 'close',
            'Cache-Control': 'no-cache, no-store, must-revalidate',
            'Pragma': 'no-cache',
            'Expires': '0'
        });
        audioClients.push(res);
        if (streamActive) {
            startAudioStream();
        }
        req.on('close', () => {
            audioClients = audioClients.filter((client) => client !== res);
            if (audioClients.length === 0 && ffmpegAudioProcess) {
                stopAudioStream();
            }
        });
        return;
    }

    // Default: 404
    replyJson({ error: 'Not found' }, 404);
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
    // Ready
});