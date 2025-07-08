const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { Readable } = require('stream');

// Environment variables
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT, 10) || 8080;
const RTSP_CAMERA_IP = process.env.RTSP_CAMERA_IP || '127.0.0.1';
const RTSP_CAMERA_PORT = process.env.RTSP_CAMERA_PORT || '554';
const RTSP_CAMERA_USERNAME = process.env.RTSP_CAMERA_USERNAME || '';
const RTSP_CAMERA_PASSWORD = process.env.RTSP_CAMERA_PASSWORD || '';
const RTSP_STREAM_PATH = process.env.RTSP_STREAM_PATH || '/stream1';
const DEFAULT_CODEC = process.env.DEFAULT_CODEC || 'mjpeg'; // 'mjpeg' is most browser-compatible
const DEFAULT_RESOLUTION = process.env.DEFAULT_RESOLUTION || '640x480';
const DEFAULT_BITRATE = process.env.DEFAULT_BITRATE || '1000k';

// Only HTTP port is configurable and used, as per requirements
// Stream State
let streamStatus = 'stopped';
let streamParams = {
    codec: DEFAULT_CODEC,
    resolution: DEFAULT_RESOLUTION,
    bitrate: DEFAULT_BITRATE
};

// RTSP URL generator
function getRtspUrl() {
    let userinfo = '';
    if (RTSP_CAMERA_USERNAME && RTSP_CAMERA_PASSWORD) {
        userinfo = `${encodeURIComponent(RTSP_CAMERA_USERNAME)}:${encodeURIComponent(RTSP_CAMERA_PASSWORD)}@`;
    } else if (RTSP_CAMERA_USERNAME) {
        userinfo = `${encodeURIComponent(RTSP_CAMERA_USERNAME)}@`;
    }
    return `rtsp://${userinfo}${RTSP_CAMERA_IP}:${RTSP_CAMERA_PORT}${RTSP_STREAM_PATH}`;
}

// A singleton object for managing a single streaming session/proxy
const streamProxy = {
    ffmpeg: null,
    clients: [],
    start(params) {
        if (this.ffmpeg) return;
        streamStatus = 'streaming';
        streamParams = { ...streamParams, ...params };

        const ffmpegArgs = [
            '-rtsp_transport', 'tcp',
            '-i', getRtspUrl(),
            '-an', // disable audio for broad compatibility
            '-f', 'mjpeg',
            '-vf', `scale=${streamParams.resolution}`,
            '-q:v', '5'
        ];

        this.ffmpeg = spawn('ffmpeg', ffmpegArgs.concat('-'));
        this.ffmpeg.stdout.on('data', (chunk) => {
            this.clients.forEach((res) => {
                res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
                res.write(chunk);
                res.write('\r\n');
            });
        });

        this.ffmpeg.stderr.on('data', () => {}); // discard logs

        this.ffmpeg.on('close', () => {
            this.ffmpeg = null;
            streamStatus = 'stopped';
            this.clients.forEach((res) => {
                try { res.end(); } catch (e) {}
            });
            this.clients = [];
        });
    },
    stop() {
        if (this.ffmpeg) {
            this.ffmpeg.kill('SIGKILL');
            this.ffmpeg = null;
        }
        streamStatus = 'stopped';
        this.clients.forEach((res) => {
            try { res.end(); } catch (e) {}
        });
        this.clients = [];
    },
    addClient(res) {
        this.clients.push(res);
        res.on('close', () => {
            this.clients = this.clients.filter((r) => r !== res);
            if (this.clients.length === 0) {
                this.stop();
            }
        });
    }
};

// HTTP Server
const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    // CORS
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

    if (req.method === 'OPTIONS') {
        res.writeHead(204);
        return res.end();
    }

    // Route: GET /stream (returns status)
    if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            status: streamStatus,
            rtsp_url: getRtspUrl(),
            params: streamParams
        }));
        return;
    }

    // Route: POST /stream/start
    if (req.method === 'POST' && parsedUrl.pathname === '/stream/start') {
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', () => {
            let params = {};
            try {
                if (body) params = JSON.parse(body);
            } catch {}
            streamProxy.start(params);
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ status: 'streaming', params: streamParams }));
        });
        return;
    }

    // Route: POST /stream/stop
    if (req.method === 'POST' && parsedUrl.pathname === '/stream/stop') {
        streamProxy.stop();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ status: 'stopped' }));
        return;
    }

    // Route: PUT /stream/params
    if (req.method === 'PUT' && parsedUrl.pathname === '/stream/params') {
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', () => {
            let params = {};
            try {
                if (body) params = JSON.parse(body);
            } catch {}
            streamParams = { ...streamParams, ...params };
            if (streamStatus === 'streaming') {
                streamProxy.stop();
                streamProxy.start(streamParams);
            }
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ status: streamStatus, params: streamParams }));
        });
        return;
    }

    // Route: GET /stream/live (HTTP MJPEG stream)
    if (req.method === 'GET' && parsedUrl.pathname === '/stream/live') {
        res.writeHead(200, {
            'Content-Type': 'multipart/x-mixed-replace; boundary=--frame',
            'Cache-Control': 'no-cache, private',
            'Pragma': 'no-cache',
            'Connection': 'close'
        });
        streamProxy.start(streamParams);
        streamProxy.addClient(res);
        return;
    }

    // Not found
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Endpoint not found' }));
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
    // No output as per requirements
});