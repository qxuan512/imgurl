const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { parse } = require('querystring');

// ==== ENVIRONMENT VARIABLES ====
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const DEVICE_RTSP_PORT = process.env.DEVICE_RTSP_PORT || '554';
const DEVICE_RTSP_PATH = process.env.DEVICE_RTSP_PATH || 'stream1';
const DEVICE_USERNAME = process.env.DEVICE_USERNAME || '';
const DEVICE_PASSWORD = process.env.DEVICE_PASSWORD || '';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT || '8080', 10);

// ==== INTERNAL STATE ====
let streamProcess = null;
let clients = [];
let streamActive = false;

// ==== HELPER FUNCTIONS ====

// MJPEG boundary
const BOUNDARY = "--myboundary";

// Compose RTSP URL
function getRtspUrl() {
    let auth = '';
    if (DEVICE_USERNAME && DEVICE_PASSWORD) {
        auth = `${DEVICE_USERNAME}:${DEVICE_PASSWORD}@`;
    } else if (DEVICE_USERNAME) {
        auth = `${DEVICE_USERNAME}@`;
    }
    return `rtsp://${auth}${DEVICE_IP}:${DEVICE_RTSP_PORT}/${DEVICE_RTSP_PATH}`;
}

// Send multipart header
function sendMultipartHeader(res) {
    res.writeHead(200, {
        'Content-Type': `multipart/x-mixed-replace; boundary=${BOUNDARY.slice(2)}`,
        'Connection': 'close',
        'Pragma': 'no-cache',
        'Cache-Control': 'no-cache',
    });
}

// Start ffmpeg process to convert RTSP to MJPEG
function startStream() {
    if (streamProcess) return;
    const rtspUrl = getRtspUrl();
    // ffmpeg command to convert RTSP to MJPEG
    streamProcess = spawn('ffmpeg', [
        '-rtsp_transport', 'tcp',
        '-i', rtspUrl,
        '-f', 'mjpeg',
        '-q:v', '5',
        '-r', '10',
        '-'
    ]);
    streamActive = true;

    streamProcess.stderr.on('data', () => {});

    streamProcess.stdout.on('data', (data) => {
        // Send MJPEG frames to all connected clients
        for (let i = clients.length - 1; i >= 0; i--) {
            const client = clients[i];
            try {
                client.write(`${BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: ${data.length}\r\n\r\n`);
                client.write(data);
                client.write('\r\n');
            } catch (e) {
                try { client.end(); } catch (err) {}
                clients.splice(i, 1);
            }
        }
    });

    streamProcess.on('close', () => {
        streamProcess = null;
        streamActive = false;
        // Close all clients
        for (let c of clients) {
            try { c.end(); } catch (e) {}
        }
        clients = [];
    });
}

// Stop ffmpeg process
function stopStream() {
    if (streamProcess) {
        streamProcess.kill('SIGKILL');
        streamProcess = null;
        streamActive = false;
    }
}

// ==== HTTP SERVER ====

const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);

    // Stream endpoint for browser-friendly MJPEG
    if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
        if (!streamActive) startStream();
        sendMultipartHeader(res);
        clients.push(res);

        req.on('close', () => {
            const idx = clients.indexOf(res);
            if (idx >= 0) clients.splice(idx, 1);
            if (clients.length === 0) stopStream();
        });
        return;
    }

    // /info: Device metadata and status
    if (req.method === 'GET' && parsedUrl.pathname === '/info') {
        const info = {
            device_name: "RTSP Camera",
            device_model: "RTSP Camera",
            manufacturer: "Various",
            device_type: "IP Camera",
            streaming: !!streamActive,
            clients: clients.length,
            rtsp_url: getRtspUrl()
        };
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(info));
        return;
    }

    // /stream/start: Start the stream
    if (req.method === 'POST' && parsedUrl.pathname === '/stream/start') {
        if (!streamActive) startStream();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'streaming', stream_url: '/stream' }));
        return;
    }

    // /stream/stop: Stop the stream
    if (req.method === 'POST' && parsedUrl.pathname === '/stream/stop') {
        stopStream();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'stopped' }));
        return;
    }

    // /ptz: PTZ control (mock, as most RTSP don't support PTZ directly via RTSP)
    if (req.method === 'POST' && parsedUrl.pathname === '/ptz') {
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', () => {
            try {
                const params = JSON.parse(body || '{}');
                // PTZ commands would go here, but as generic, just mock response
                res.writeHead(200, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({
                    status: 'ok',
                    received: params,
                    message: 'PTZ command issued (mocked)'
                }));
            } catch (e) {
                res.writeHead(400, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ status: 'error', message: 'Invalid JSON' }));
            }
        });
        return;
    }

    // Not found
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Not found' }));
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
    console.log(`RTSP Camera HTTP Proxy listening on http://${SERVER_HOST}:${SERVER_PORT}`);
    console.log(`MJPEG Stream available at http://${SERVER_HOST}:${SERVER_PORT}/stream`);
});