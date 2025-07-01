// device-shifu-driver-rtsp-camera.js

const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { PassThrough } = require('stream');

// Configuration from environment variables
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const RTSP_PORT = process.env.RTSP_PORT || '554';
const RTSP_PATH = process.env.RTSP_PATH || '/stream';
const RTSP_USERNAME = process.env.RTSP_USERNAME || '';
const RTSP_PASSWORD = process.env.RTSP_PASSWORD || '';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT || '8080', 10);

const VIDEO_CODEC = process.env.VIDEO_CODEC || 'h264'; // or h265 or mjpeg
const AUDIO_CODEC = process.env.AUDIO_CODEC || 'aac'; // or g711

const STREAM_TIMEOUT = parseInt(process.env.STREAM_TIMEOUT || '600', 10); // seconds

// Internal state
let videoStreamProcess = null;
let audioStreamProcess = null;
let videoStreamActive = false;
let audioStreamActive = false;
let videoStreamClients = [];
let audioStreamClients = [];
let videoStreamPT = null;
let audioStreamPT = null;

function getRtspUrl() {
    let auth = '';
    if (RTSP_USERNAME && RTSP_PASSWORD) {
        auth = `${encodeURIComponent(RTSP_USERNAME)}:${encodeURIComponent(RTSP_PASSWORD)}@`;
    }
    return `rtsp://${auth}${DEVICE_IP}:${RTSP_PORT}${RTSP_PATH}`;
}

function startVideoStream() {
    if (videoStreamActive) return;
    const inputUrl = getRtspUrl();
    let args = [
        '-rtsp_transport', 'tcp',
        '-i', inputUrl,
        '-an',
        '-c:v', 'copy',
        '-f', 'mp4',
        '-movflags', 'frag_keyframe+empty_moov+default_base_moof',
        'pipe:1'
    ];
    videoStreamPT = new PassThrough();
    videoStreamProcess = spawn('ffmpeg', args, { stdio: ['ignore', 'pipe', 'ignore'] });
    videoStreamProcess.stdout.pipe(videoStreamPT);
    videoStreamActive = true;

    videoStreamProcess.on('close', () => {
        videoStreamActive = false;
        videoStreamPT && videoStreamPT.end();
        videoStreamPT = null;
        videoStreamProcess = null;
        videoStreamClients.forEach(res => res.end());
        videoStreamClients = [];
    });
}

function stopVideoStream() {
    if (videoStreamProcess) {
        videoStreamProcess.kill('SIGTERM');
    }
    videoStreamActive = false;
}

function startAudioStream() {
    if (audioStreamActive) return;
    const inputUrl = getRtspUrl();
    let args = [
        '-rtsp_transport', 'tcp',
        '-i', inputUrl,
        '-vn',
        '-c:a', AUDIO_CODEC === 'g711' ? 'pcm_alaw' : 'aac',
        '-f', AUDIO_CODEC === 'g711' ? 'wav' : 'adts',
        'pipe:1'
    ];
    audioStreamPT = new PassThrough();
    audioStreamProcess = spawn('ffmpeg', args, { stdio: ['ignore', 'pipe', 'ignore'] });
    audioStreamProcess.stdout.pipe(audioStreamPT);
    audioStreamActive = true;

    audioStreamProcess.on('close', () => {
        audioStreamActive = false;
        audioStreamPT && audioStreamPT.end();
        audioStreamPT = null;
        audioStreamProcess = null;
        audioStreamClients.forEach(res => res.end());
        audioStreamClients = [];
    });
}

function stopAudioStream() {
    if (audioStreamProcess) {
        audioStreamProcess.kill('SIGTERM');
    }
    audioStreamActive = false;
}

function handleStart(req, res) {
    startVideoStream();
    startAudioStream();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        status: 'started',
        video: videoStreamActive,
        audio: audioStreamActive
    }));
}

function handleStop(req, res) {
    stopVideoStream();
    stopAudioStream();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        status: 'stopped',
        video: videoStreamActive,
        audio: audioStreamActive
    }));
}

function handleGetStream(req, res) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        active: videoStreamActive || audioStreamActive,
        video: {
            active: videoStreamActive,
            codec: VIDEO_CODEC,
            url: `/streams/video`
        },
        audio: {
            active: audioStreamActive,
            codec: AUDIO_CODEC,
            url: `/streams/audio`
        },
        rtsp: {
            url: getRtspUrl()
        }
    }));
}

function handleStreamsVideo(req, res) {
    if (!videoStreamActive) startVideoStream();

    res.writeHead(200, {
        'Content-Type': 'video/mp4',
        'Transfer-Encoding': 'chunked',
        'Cache-Control': 'no-cache'
    });
    videoStreamClients.push(res);

    if (!videoStreamPT) {
        res.end();
        return;
    }

    const clientStream = videoStreamPT.pipe(new PassThrough());
    clientStream.pipe(res);

    req.on('close', () => {
        clientStream.unpipe(res);
        const idx = videoStreamClients.indexOf(res);
        if (idx >= 0) videoStreamClients.splice(idx, 1);
        if (videoStreamClients.length === 0) stopVideoStream();
    });
}

function handleStreamsAudio(req, res) {
    if (!audioStreamActive) startAudioStream();

    let contentType = AUDIO_CODEC === 'g711' ? 'audio/wav' : 'audio/aac';
    res.writeHead(200, {
        'Content-Type': contentType,
        'Transfer-Encoding': 'chunked',
        'Cache-Control': 'no-cache'
    });
    audioStreamClients.push(res);

    if (!audioStreamPT) {
        res.end();
        return;
    }

    const clientStream = audioStreamPT.pipe(new PassThrough());
    clientStream.pipe(res);

    req.on('close', () => {
        clientStream.unpipe(res);
        const idx = audioStreamClients.indexOf(res);
        if (idx >= 0) audioStreamClients.splice(idx, 1);
        if (audioStreamClients.length === 0) stopAudioStream();
    });
}

function parseBody(req, callback) {
    let body = '';
    req.on('data', chunk => { body += chunk; });
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

    // POST /commands/start or /cmd/start
    if ((req.method === 'POST') &&
        (parsedUrl.pathname === '/commands/start' || parsedUrl.pathname === '/cmd/start')) {
        parseBody(req, () => handleStart(req, res));
        return;
    }

    // POST /commands/stop or /cmd/stop
    if ((req.method === 'POST') &&
        (parsedUrl.pathname === '/commands/stop' || parsedUrl.pathname === '/cmd/stop')) {
        parseBody(req, () => handleStop(req, res));
        return;
    }

    // GET /stream
    if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
        handleGetStream(req, res);
        return;
    }

    // GET /streams/video
    if (req.method === 'GET' && parsedUrl.pathname === '/streams/video') {
        handleStreamsVideo(req, res);
        return;
    }

    // GET /streams/audio
    if (req.method === 'GET' && parsedUrl.pathname === '/streams/audio') {
        handleStreamsAudio(req, res);
        return;
    }

    // 404
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Not found' }));
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
    console.log(`RTSP Camera HTTP Driver listening on http://${SERVER_HOST}:${SERVER_PORT}/`);
});