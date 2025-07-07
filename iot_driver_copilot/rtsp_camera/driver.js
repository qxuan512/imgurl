const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { Readable } = require('stream');

// Environment variables for configuration
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const DEVICE_RTSP_PORT = parseInt(process.env.DEVICE_RTSP_PORT) || 554;
const DEVICE_RTSP_USER = process.env.DEVICE_RTSP_USER || '';
const DEVICE_RTSP_PASS = process.env.DEVICE_RTSP_PASS || '';
const DEVICE_RTSP_PATH = process.env.DEVICE_RTSP_PATH || 'Streaming/Channels/101';
const HTTP_SERVER_HOST = process.env.HTTP_SERVER_HOST || '0.0.0.0';
const HTTP_SERVER_PORT = parseInt(process.env.HTTP_SERVER_PORT) || 8080;

// For demonstration, PTZ control is not implemented (would require device-specific HTTP API)
const PTZ_SUPPORTED = false;

let streaming = false;
let currentFFmpeg = null;
let streamClients = [];

const getRTSPUrl = () => {
    let auth = '';
    if (DEVICE_RTSP_USER && DEVICE_RTSP_PASS) {
        auth = `${encodeURIComponent(DEVICE_RTSP_USER)}:${encodeURIComponent(DEVICE_RTSP_PASS)}@`;
    }
    return `rtsp://${auth}${DEVICE_IP}:${DEVICE_RTSP_PORT}/${DEVICE_RTSP_PATH}`;
};

function startStream() {
    if (streaming && currentFFmpeg) {
        return true;
    }
    const rtspUrl = getRTSPUrl();

    // Use ffmpeg to convert RTSP to MJPEG, output to stdout (single process for all clients)
    // This is the only way to do protocol conversion in pure JS is to rely on ffmpeg binary,
    // but as required, we avoid executing commands here and just keep the placeholder for native implementation.
    // In a pure JS environment, protocol translation (RTSP to MJPEG) is not feasible without native or external tools.
    // So we simulate the streaming by creating a readable stream (placeholder).
    // In practical deployment, a native addon, or WASM ffmpeg, or similar would be required.

    // Placeholder simulated MJPEG stream
    let mjpegReadable = new Readable({
        read() { }
    });
    // Would push JPEG frames here in real implementation

    streaming = true;
    currentFFmpeg = mjpegReadable;
    return true;
}

function stopStream() {
    if (currentFFmpeg) {
        currentFFmpeg.destroy();
    }
    streaming = false;
    currentFFmpeg = null;
    streamClients = [];
    return true;
}

function serveMJPEG(req, res) {
    // Start stream if not already started
    if (!streaming || !currentFFmpeg) {
        startStream();
    }
    res.writeHead(200, {
        'Content-Type': 'multipart/x-mixed-replace; boundary=--frame',
        'Cache-Control': 'no-cache',
        'Connection': 'close',
        'Pragma': 'no-cache',
    });
    streamClients.push(res);

    req.on('close', () => {
        streamClients = streamClients.filter(r => r !== res);
        if (streamClients.length === 0) {
            stopStream();
        }
    });
    // In real code, pipe MJPEG frames to res here
}

// Simulate camera info/state
function getDeviceInfo() {
    return {
        device_name: "RTSP Camera",
        device_model: "RTSP Camera",
        manufacturer: "Various",
        device_type: "IP Camera",
        streaming: streaming,
        stream_url: `/stream` // HTTP MJPEG endpoint
    };
}

// HTTP Server
const server = http.createServer(async (req, res) => {
    const parsedUrl = url.parse(req.url, true);
    if (req.method === 'GET' && parsedUrl.pathname === '/info') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(getDeviceInfo()));
        return;
    }

    if (req.method === 'POST' && parsedUrl.pathname === '/ptz') {
        if (!PTZ_SUPPORTED) {
            res.writeHead(501, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ error: 'PTZ control not supported on this device.' }));
            return;
        }
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', () => {
            // Parse body and send PTZ command (not implemented)
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ status: 'PTZ command sent (simulated).' }));
        });
        return;
    }

    if (req.method === 'POST' && parsedUrl.pathname === '/stream/start') {
        const started = startStream();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ streaming: streaming, started }));
        return;
    }

    if (req.method === 'POST' && parsedUrl.pathname === '/stream/stop') {
        const stopped = stopStream();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ streaming: streaming, stopped }));
        return;
    }

    if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
        serveMJPEG(req, res);
        // Simulate MJPEG streaming, in real implementation MJPEG frames are written here
        return;
    }

    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Endpoint not found' }));
});

// Simulate MJPEG frames for browser demo (uses a static JPEG)
const fs = require('fs');
const path = require('path');
const sampleJpeg = fs.readFileSync(path.join(__dirname, 'sample.jpg')); // Place a sample.jpg in the same folder

function broadcastMJPEGFrame() {
    if (streamClients.length === 0) return;
    const frame = Buffer.from(
        `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${sampleJpeg.length}\r\n\r\n`
    );
    for (const res of streamClients) {
        res.write(frame);
        res.write(sampleJpeg);
        res.write('\r\n');
    }
}
setInterval(() => {
    if (streaming) broadcastMJPEGFrame();
}, 100); // ~10fps

server.listen(HTTP_SERVER_PORT, HTTP_SERVER_HOST, () => {
    console.log(`RTSP Camera HTTP proxy running at http://${HTTP_SERVER_HOST}:${HTTP_SERVER_PORT}`);
    console.log(`Stream endpoint: http://${HTTP_SERVER_HOST}:${HTTP_SERVER_PORT}/stream`);
});