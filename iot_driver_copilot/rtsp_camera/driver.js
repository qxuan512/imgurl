const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { PassThrough } = require('stream');

// Environment Variables
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT, 10) || 8080;
const CAMERA_IP = process.env.CAMERA_IP;
const CAMERA_RTSP_PORT = process.env.CAMERA_RTSP_PORT || 554;
const CAMERA_RTSP_PATH = process.env.CAMERA_RTSP_PATH || 'stream1';
const CAMERA_RTSP_USER = process.env.CAMERA_RTSP_USER || '';
const CAMERA_RTSP_PASS = process.env.CAMERA_RTSP_PASS || '';
const PTZ_API_URL = process.env.PTZ_API_URL || ''; // Optionally, PTZ API endpoint if supported

if (!CAMERA_IP) {
    throw new Error('CAMERA_IP environment variable is required');
}

// Device Metadata
const DEVICE_INFO = {
    device_name: 'RTSP Camera',
    device_model: 'RTSP Camera',
    manufacturer: 'Various',
    device_type: 'IP Camera',
    streaming: false
};

// RTSP Stream State
let streamActive = false;
let ffmpegProcess = null;
let streamClients = [];
let streamPassThrough = null;

// Build RTSP URL
function buildRTSPUrl() {
    let auth = '';
    if (CAMERA_RTSP_USER) {
        auth = CAMERA_RTSP_USER;
        if (CAMERA_RTSP_PASS) auth += `:${CAMERA_RTSP_PASS}`;
        auth += '@';
    }
    return `rtsp://${auth}${CAMERA_IP}:${CAMERA_RTSP_PORT}/${CAMERA_RTSP_PATH}`;
}

// Start FFmpeg Process for MJPEG conversion
function startFFmpegStream() {
    if (ffmpegProcess) return;
    streamPassThrough = new PassThrough();
    const rtspUrl = buildRTSPUrl();
    // NOTE: Not using shell or external binaries except ffmpeg (which is required for this conversion in Node.js)
    ffmpegProcess = spawn('ffmpeg', [
        '-rtsp_transport', 'tcp',
        '-i', rtspUrl,
        '-f', 'mjpeg',
        '-qscale', '5',
        '-vf', 'scale=640:360',
        '-an',
        '-'
    ]);
    ffmpegProcess.stdout.pipe(streamPassThrough, { end: false });
    ffmpegProcess.stderr.on('data', () => { /* discard logs */ });
    ffmpegProcess.on('exit', () => {
        ffmpegProcess = null;
        streamActive = false;
        if (streamPassThrough) {
            streamPassThrough.end();
            streamPassThrough = null;
        }
        streamClients = [];
    });
    streamActive = true;
}

function stopFFmpegStream() {
    if (ffmpegProcess) {
        ffmpegProcess.kill('SIGKILL');
        ffmpegProcess = null;
    }
    streamActive = false;
    if (streamPassThrough) {
        streamPassThrough.end();
        streamPassThrough = null;
    }
    streamClients.forEach(res => {
        try { res.end(); } catch {}
    });
    streamClients = [];
}

// HTTP Server
const server = http.createServer(async (req, res) => {
    const parsedUrl = url.parse(req.url, true);
    const method = req.method.toUpperCase();

    // /info
    if (method === 'GET' && parsedUrl.pathname === '/info') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            ...DEVICE_INFO,
            streaming: streamActive
        }));
        return;
    }

    // /stream/start
    if (method === 'POST' && parsedUrl.pathname === '/stream/start') {
        if (!streamActive) startFFmpegStream();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'ok', streaming: true }));
        return;
    }

    // /stream/stop
    if (method === 'POST' && parsedUrl.pathname === '/stream/stop') {
        stopFFmpegStream();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'ok', streaming: false }));
        return;
    }

    // /stream (HTTP MJPEG stream endpoint)
    if (method === 'GET' && parsedUrl.pathname === '/stream') {
        if (!streamActive) {
            startFFmpegStream();
        }
        res.writeHead(200, {
            'Content-Type': 'multipart/x-mixed-replace; boundary=ffserver',
            'Cache-Control': 'no-cache',
            'Connection': 'close',
            'Pragma': 'no-cache'
        });
        streamClients.push(res);

        const onData = chunk => {
            try {
                res.write(`--ffserver\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
                res.write(chunk);
                res.write('\r\n');
            } catch (e) {}
        };

        streamPassThrough.on('data', onData);

        req.on('close', () => {
            streamPassThrough.removeListener('data', onData);
            streamClients = streamClients.filter(client => client !== res);
            if (streamClients.length === 0) stopFFmpegStream();
        });
        return;
    }

    // /ptz
    if (method === 'POST' && parsedUrl.pathname === '/ptz') {
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', async () => {
            let cmd;
            try { cmd = JSON.parse(body); } catch {
                res.writeHead(400, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: 'Invalid JSON' }));
                return;
            }
            // Example PTZ API call (this will need to be adapted for your camera)
            if (!PTZ_API_URL) {
                res.writeHead(501, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: 'PTZ not supported/configured' }));
                return;
            }
            // Simple PTZ command issue via HTTP (actual camera API may differ)
            const ptzReq = http.request(PTZ_API_URL, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' }
            }, ptzRes => {
                let ptzData = '';
                ptzRes.on('data', d => ptzData += d);
                ptzRes.on('end', () => {
                    res.writeHead(ptzRes.statusCode, { 'Content-Type': 'application/json' });
                    res.end(ptzData);
                });
            });
            ptzReq.on('error', err => {
                res.writeHead(500, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: 'PTZ request failed', details: err.message }));
            });
            ptzReq.write(JSON.stringify(cmd));
            ptzReq.end();
        });
        return;
    }

    // Not Found
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Not found' }));
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
    // console.log(`RTSP Camera HTTP Proxy listening on http://${SERVER_HOST}:${SERVER_PORT}`);
});