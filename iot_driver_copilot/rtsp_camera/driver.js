const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const EventEmitter = require('events');

// Configuration from environment
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const DEVICE_RTSP_PORT = process.env.DEVICE_RTSP_PORT || '554';
const DEVICE_RTSP_PATH = process.env.DEVICE_RTSP_PATH || '/stream';
const DEVICE_RTSP_USER = process.env.DEVICE_RTSP_USER || '';
const DEVICE_RTSP_PASS = process.env.DEVICE_RTSP_PASS || '';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT || '8080', 10);
const DEFAULT_STREAM_FORMAT = process.env.DEFAULT_STREAM_FORMAT || 'mjpeg'; // mjpeg, h264, mp4

// Supported formats
const SUPPORTED_FORMATS = ['mjpeg', 'h264', 'mp4'];
const MIME_TYPES = {
    'mjpeg': 'multipart/x-mixed-replace; boundary=ffserver',
    'h264': 'video/h264',
    'mp4': 'video/mp4'
};

// RTSP URL builder
function getRtspUrl() {
    if (DEVICE_RTSP_USER && DEVICE_RTSP_PASS) {
        return `rtsp://${DEVICE_RTSP_USER}:${DEVICE_RTSP_PASS}@${DEVICE_IP}:${DEVICE_RTSP_PORT}${DEVICE_RTSP_PATH}`;
    }
    return `rtsp://${DEVICE_IP}:${DEVICE_RTSP_PORT}${DEVICE_RTSP_PATH}`;
}

// Camera Stream Manager
class CameraStream extends EventEmitter {
    constructor() {
        super();
        this.active = false;
        this.format = DEFAULT_STREAM_FORMAT;
        this.ffmpeg = null;
        this.clients = [];
    }

    start(format = DEFAULT_STREAM_FORMAT) {
        if (this.active) return;
        if (!SUPPORTED_FORMATS.includes(format)) format = DEFAULT_STREAM_FORMAT;
        this.format = format;
        this.active = true;

        // Build ffmpeg args
        let args = [
            '-rtsp_transport', 'tcp',
            '-i', getRtspUrl()
        ];

        if (format === 'mjpeg') {
            args.push('-f', 'mjpeg', '-q:v', '5', '-');
        } else if (format === 'h264') {
            args.push('-an', '-c:v', 'copy', '-f', 'h264', '-');
        } else if (format === 'mp4') {
            args.push('-an', '-c:v', 'copy', '-f', 'mp4', '-movflags', 'frag_keyframe+empty_moov', '-');
        }

        this.ffmpeg = spawn('ffmpeg', args);

        this.ffmpeg.stdout.on('data', (chunk) => {
            this.clients.forEach(res => {
                res.write(chunk);
            });
        });

        this.ffmpeg.stderr.on('data', () => {});

        this.ffmpeg.on('close', () => {
            this.active = false;
            this.ffmpeg = null;
            this.clients.forEach(res => {
                res.end();
            });
            this.clients = [];
            this.emit('stopped');
        });

        this.emit('started');
    }

    stop() {
        if (!this.active || !this.ffmpeg) return;
        this.ffmpeg.kill('SIGTERM');
        this.active = false;
        this.emit('stopped');
    }

    addClient(res) {
        this.clients.push(res);
        res.on('close', () => {
            this.clients = this.clients.filter(r => r !== res);
            if (this.clients.length === 0 && this.active) {
                this.stop();
            }
        });
    }

    getStatus() {
        return {
            active: this.active,
            format: this.format,
            stream_url: `/stream?format=${this.format}`,
            supported_formats: SUPPORTED_FORMATS
        };
    }
}

const cameraStream = new CameraStream();

function handleStart(req, res, body) {
    let format = DEFAULT_STREAM_FORMAT;
    try {
        const data = body ? JSON.parse(body) : {};
        if (typeof data.format === 'string' && SUPPORTED_FORMATS.includes(data.format)) {
            format = data.format;
        }
    } catch (_) {}

    cameraStream.start(format);

    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        status: 'started',
        format: format,
        stream_url: `/stream?format=${format}`,
        supported_formats: SUPPORTED_FORMATS
    }));
}

function handleStop(req, res) {
    cameraStream.stop();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'stopped' }));
}

function handleStream(req, res, query) {
    let format = DEFAULT_STREAM_FORMAT;
    if (typeof query.format === 'string' && SUPPORTED_FORMATS.includes(query.format.toLowerCase())) {
        format = query.format.toLowerCase();
    }
    if (!cameraStream.active || cameraStream.format !== format) {
        cameraStream.start(format);
    }

    res.writeHead(200, { 'Content-Type': MIME_TYPES[format] });
    cameraStream.addClient(res);
}

function handleStatus(req, res, query) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(cameraStream.getStatus()));
}

const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
        if (req.method === 'POST' && parsedUrl.pathname === '/commands/start') {
            handleStart(req, res, body);
        } else if (req.method === 'POST' && parsedUrl.pathname === '/commands/stop') {
            handleStop(req, res);
        } else if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
            handleStream(req, res, parsedUrl.query);
        } else if (req.method === 'GET' && parsedUrl.pathname === '/stream/status') {
            handleStatus(req, res, parsedUrl.query);
        } else {
            res.writeHead(404, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ error: 'Not found' }));
        }
    });
});

server.listen(SERVER_PORT, SERVER_HOST);