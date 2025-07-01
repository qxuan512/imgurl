const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { PassThrough } = require('stream');

// ==== ENVIRONMENT VARIABLES ====
const DEVICE_IP = process.env.RTSP_CAMERA_IP || '192.168.1.64';
const DEVICE_PORT = process.env.RTSP_CAMERA_RTSP_PORT || '554';
const DEVICE_USER = process.env.RTSP_CAMERA_USER || '';
const DEVICE_PASS = process.env.RTSP_CAMERA_PASS || '';
const RTSP_PATH = process.env.RTSP_CAMERA_RTSP_PATH || 'stream1';
const SERVER_HOST = process.env.HTTP_SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.HTTP_SERVER_PORT || '8080', 10);

const RTSP_URL = `rtsp://${DEVICE_USER ? DEVICE_USER + (DEVICE_PASS ? `:${DEVICE_PASS}` : '') + '@' : ''}${DEVICE_IP}:${DEVICE_PORT}/${RTSP_PATH}`;

// ==== STREAM STATE ====
let isStreaming = false;
let videoClients = [];
let audioClients = [];
let ffmpegVideoProcess = null;
let ffmpegAudioProcess = null;

// ==== UTILITY: Spawn ffmpeg as a library process ====
function startFFmpegStream(type) {
    if (type === 'video' && !ffmpegVideoProcess) {
        // Video stream as MJPEG for browser compatibility
        ffmpegVideoProcess = spawn('ffmpeg', [
            '-rtsp_transport', 'tcp',
            '-i', RTSP_URL,
            '-an',
            '-c:v', 'mjpeg',
            '-f', 'mjpeg',
            '-q:v', '5',
            '-r', '15',
            'pipe:1'
        ], { stdio: ['ignore', 'pipe', 'ignore'] });

        ffmpegVideoProcess.on('close', () => {
            ffmpegVideoProcess = null;
            videoClients.forEach(res => {
                try { res.end(); } catch(e){}
            });
            videoClients = [];
        });
    }
    if (type === 'audio' && !ffmpegAudioProcess) {
        // Audio stream as WAV for browser/CLI compatibility
        ffmpegAudioProcess = spawn('ffmpeg', [
            '-rtsp_transport', 'tcp',
            '-i', RTSP_URL,
            '-vn',
            '-c:a', 'pcm_s16le',
            '-ar', '44100',
            '-ac', '1',
            '-f', 'wav',
            'pipe:1'
        ], { stdio: ['ignore', 'pipe', 'ignore'] });

        ffmpegAudioProcess.on('close', () => {
            ffmpegAudioProcess = null;
            audioClients.forEach(res => {
                try { res.end(); } catch(e){}
            });
            audioClients = [];
        });
    }
}

function stopFFmpegStream(type) {
    if (type === 'video' && ffmpegVideoProcess) {
        ffmpegVideoProcess.kill('SIGTERM');
        ffmpegVideoProcess = null;
    }
    if (type === 'audio' && ffmpegAudioProcess) {
        ffmpegAudioProcess.kill('SIGTERM');
        ffmpegAudioProcess = null;
    }
    if (type === 'all') {
        stopFFmpegStream('video');
        stopFFmpegStream('audio');
    }
}

// ==== HTTP SERVER ====
const server = http.createServer(async (req, res) => {
    const parsedUrl = url.parse(req.url, true);
    const method = req.method.toUpperCase();
    const path = parsedUrl.pathname;

    // === /commands/start and /cmd/start (POST) ===
    if ((path === '/commands/start' || path === '/cmd/start') && method === 'POST') {
        isStreaming = true;
        res.writeHead(200, {'Content-Type': 'application/json'});
        res.end(JSON.stringify({
            status: "started",
            details: "Media streams started.",
            video: true,
            audio: true
        }));
        return;
    }

    // === /commands/stop and /cmd/stop (POST) ===
    if ((path === '/commands/stop' || path === '/cmd/stop') && method === 'POST') {
        isStreaming = false;
        stopFFmpegStream('all');
        res.writeHead(200, {'Content-Type': 'application/json'});
        res.end(JSON.stringify({
            status: "stopped",
            details: "Media streams stopped.",
            video: false,
            audio: false
        }));
        return;
    }

    // === /stream (GET) ===
    if (path === '/stream' && method === 'GET') {
        res.writeHead(200, {'Content-Type': 'application/json'});
        res.end(JSON.stringify({
            active: isStreaming,
            video: isStreaming,
            audio: isStreaming,
            video_format: "MJPEG (proxied)",
            audio_format: "WAV (proxied)",
            rtsp_url: RTSP_URL
        }));
        return;
    }

    // === /streams/video (GET) ===
    if (path === '/streams/video' && method === 'GET') {
        if (!isStreaming) {
            res.writeHead(503, {'Content-Type': 'application/json'});
            res.end(JSON.stringify({ error: 'Stream not started. Use /commands/start.' }));
            return;
        }
        res.writeHead(200, {
            'Content-Type': 'multipart/x-mixed-replace; boundary=ffmpegvideo',
            'Cache-Control': 'no-cache',
            'Connection': 'close',
            'Pragma': 'no-cache'
        });
        videoClients.push(res);

        startFFmpegStream('video');

        const ffmpeg = ffmpegVideoProcess;
        if (ffmpeg && ffmpeg.stdout) {
            const writer = (data) => {
                try {
                    res.write(`--ffmpegvideo\r\nContent-Type: image/jpeg\r\nContent-Length: ${data.length}\r\n\r\n`);
                    res.write(data);
                    res.write('\r\n');
                } catch(e) {}
            };
            ffmpeg.stdout.on('data', writer);

            req.on('close', () => {
                ffmpeg.stdout.off('data', writer);
                videoClients = videoClients.filter(r => r !== res);
                if (videoClients.length === 0) stopFFmpegStream('video');
            });
        }
        return;
    }

    // === /streams/audio (GET) ===
    if (path === '/streams/audio' && method === 'GET') {
        if (!isStreaming) {
            res.writeHead(503, {'Content-Type': 'application/json'});
            res.end(JSON.stringify({ error: 'Stream not started. Use /commands/start.' }));
            return;
        }
        res.writeHead(200, {
            'Content-Type': 'audio/wav',
            'Cache-Control': 'no-cache',
            'Connection': 'close',
            'Pragma': 'no-cache'
        });
        audioClients.push(res);

        startFFmpegStream('audio');

        const ffmpeg = ffmpegAudioProcess;
        if (ffmpeg && ffmpeg.stdout) {
            const writer = (data) => {
                try {
                    res.write(data);
                } catch(e) {}
            };
            ffmpeg.stdout.on('data', writer);

            req.on('close', () => {
                ffmpeg.stdout.off('data', writer);
                audioClients = audioClients.filter(r => r !== res);
                if (audioClients.length === 0) stopFFmpegStream('audio');
            });
        }
        return;
    }

    // === Fallback: 404 ===
    res.writeHead(404, {'Content-Type': 'application/json'});
    res.end(JSON.stringify({ error: 'Not Found' }));
});

// ==== SERVER BOOT ====
server.listen(SERVER_PORT, SERVER_HOST, () => {
    console.log(`RTSP Camera HTTP Proxy listening on http://${SERVER_HOST}:${SERVER_PORT}`);
});