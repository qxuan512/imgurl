const express = require('express');
const http = require('http');
const net = require('net');
const url = require('url');
const { PassThrough } = require('stream');

// Environment Variables
const DEVICE_IP = process.env.DEVICE_IP;
const DEVICE_RTSP_PORT = parseInt(process.env.DEVICE_RTSP_PORT || '554', 10);
const DEVICE_RTSP_USER = process.env.DEVICE_RTSP_USER || '';
const DEVICE_RTSP_PASS = process.env.DEVICE_RTSP_PASS || '';
const DEVICE_RTSP_PATH = process.env.DEVICE_RTSP_PATH || '/Streaming/Channels/101/';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT || '8080', 10);
const STREAM_HTTP_PORT = parseInt(process.env.STREAM_HTTP_PORT || SERVER_PORT, 10);

const DEVICE_MODEL = process.env.DEVICE_MODEL || "RTSP Camera";
const DEVICE_MANUFACTURER = process.env.DEVICE_MANUFACTURER || "Various";

// Streaming state
let streaming = false;
let currentStream = null;
let streamClients = [];
let rtspSession = null;
let seq = 1;

// MJPEG Frame Handling
const BOUNDARY = 'MJPEGBOUNDARY';

function makeRtspUrl() {
    let auth = '';
    if (DEVICE_RTSP_USER) {
        auth = encodeURIComponent(DEVICE_RTSP_USER);
        if (DEVICE_RTSP_PASS) {
            auth += ':' + encodeURIComponent(DEVICE_RTSP_PASS);
        }
        auth += '@';
    }
    return `rtsp://${auth}${DEVICE_IP}:${DEVICE_RTSP_PORT}${DEVICE_RTSP_PATH}`;
}

// Minimal RTSP Client for MJPEG extraction
class RtspMjpegProxy {
    constructor(rtspUrl) {
        this.rtspUrl = rtspUrl;
        this.host = DEVICE_IP;
        this.port = DEVICE_RTSP_PORT;
        this.socket = null;
        this.session = null;
        this.urlpath = url.parse(rtspUrl).path;
        this.seq = 1;
        this.buffer = Buffer.alloc(0);
        this.reading = false;
        this.closed = false;
        this.clients = [];
        this.mjpegStream = new PassThrough();
    }

    async start() {
        return new Promise((resolve, reject) => {
            this.socket = net.connect(this.port, this.host, async () => {
                try {
                    await this.sendOptions();
                    await this.sendDescribe();
                    await this.sendSetup();
                    await this.sendPlay();
                    this.reading = true;
                    this.socket.on('data', (data) => this.onData(data));
                    resolve();
                } catch (err) {
                    this.close();
                    reject(err);
                }
            });
            this.socket.on('error', (err) => {
                this.closed = true;
                this.mjpegStream.end();
            });
            this.socket.on('close', () => {
                this.closed = true;
                this.mjpegStream.end();
            });
        });
    }

    stop() {
        this.close();
    }

    close() {
        if (this.socket && !this.closed) {
            this.closed = true;
            try {
                this.socket.end();
                this.socket.destroy();
            } catch (e) {}
        }
        this.mjpegStream.end();
    }

    sendRtsp(cmd, headers = {}, body = "") {
        let str = `${cmd} RTSP/1.0\r\nCSeq: ${this.seq++}\r\n`;
        for (const k in headers) {
            str += `${k}: ${headers[k]}\r\n`;
        }
        str += '\r\n';
        if (body) str += body;
        this.socket.write(str);
        return new Promise((resolve, reject) => {
            let bufs = [];
            const onData = (data) => {
                bufs.push(data);
                const all = Buffer.concat(bufs).toString('utf8');
                if (all.includes('\r\n\r\n')) {
                    this.socket.removeListener('data', onData);
                    resolve(all);
                }
            };
            this.socket.on('data', onData);
        });
    }

    async sendOptions() {
        await this.sendRtsp(`OPTIONS ${this.rtspUrl}`);
    }
    async sendDescribe() {
        // Accept SDP
        await this.sendRtsp(`DESCRIBE ${this.rtspUrl}`, {
            'Accept': 'application/sdp'
        });
    }
    async sendSetup() {
        // SETUP for video channel 0, use UDP or TCP interleaved
        // We use TCP interleaved for browser forwarding
        let track = this.urlpath;
        await this.sendRtsp(`SETUP ${this.rtspUrl}/trackID=1`, {
            'Transport': 'RTP/AVP/TCP;unicast;interleaved=0-1'
        });
    }
    async sendPlay() {
        const res = await this.sendRtsp(`PLAY ${this.rtspUrl}`, {
            'Session': this.session ? this.session : ''
        });
        // Extract session from response
        const m = res.match(/Session: ([^\r\n;]+)/);
        if (m) this.session = m[1];
    }

    onData(data) {
        // RTP over RTSP (RFC 2326). $ + channel + len + RTP packet
        // We want to extract JPEG frames from RTP packets, then push as MJPEG multipart
        this.buffer = Buffer.concat([this.buffer, data]);
        while (this.buffer.length > 4) {
            if (this.buffer[0] !== 0x24) {
                // Not RTP, skip bad byte
                this.buffer = this.buffer.slice(1);
                continue;
            }
            const channel = this.buffer[1];
            const len = this.buffer.readUInt16BE(2);
            if (this.buffer.length < 4 + len) break;
            const rtp = this.buffer.slice(4, 4 + len);
            this.handleRtp(rtp);
            this.buffer = this.buffer.slice(4 + len);
        }
    }

    handleRtp(rtp) {
        // RTP header: https://datatracker.ietf.org/doc/html/rfc3550
        // Payload type for JPEG: 26 (0x1A) or dynamic PT for H.264
        // We'll attempt simple JPEG extraction for demonstration, otherwise ignore
        if (rtp.length < 12) return;
        const payloadType = rtp[1] & 0x7F;
        const marker = !!(rtp[1] & 0x80);
        let payload = rtp.slice(12);
        // RTP/JPEG: https://datatracker.ietf.org/doc/html/rfc2435
        if (payloadType === 26) {
            // RTP/JPEG
            // 8-byte JPEG header, then JPEG data
            if (payload.length < 8) return;
            const jpegHeader = payload.slice(0, 8);
            const jpegData = payload.slice(8);
            // For simple demo, we just forward JPEG data as-is (incomplete, strictly should reassemble)
            // We'll buffer until marker bit set (end of frame)
            if (!this._jpegBuf) this._jpegBuf = [];
            this._jpegBuf.push(jpegData);
            if (marker) {
                const frame = Buffer.concat(this._jpegBuf);
                this.pushJpegFrame(frame);
                this._jpegBuf = [];
            }
        }
        // For H.264 or others, not implemented in this example
    }

    pushJpegFrame(jpegBuf) {
        // Write MJPEG multipart
        const header = `\r\n--${BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: ${jpegBuf.length}\r\n\r\n`;
        this.mjpegStream.write(header);
        this.mjpegStream.write(jpegBuf);
    }

    getMjpegStream() {
        return this.mjpegStream;
    }
}

// Express App Setup
const app = express();
app.use(express.json());

// Camera Info
const cameraInfo = {
    model: DEVICE_MODEL,
    manufacturer: DEVICE_MANUFACTURER,
    streaming: () => streaming,
    ip: DEVICE_IP,
    rtsp_port: DEVICE_RTSP_PORT,
    rtsp_path: DEVICE_RTSP_PATH
};

// ========== API ==========

// PTZ Control (Dummy - since generic RTSP camera, not always supported)
app.post('/ptz', (req, res) => {
    // Accepts { direction, angle, zoom }
    // This is a placeholder. Real cameras need ONVIF or proprietary HTTP API.
    res.status(501).json({ status: 'error', message: 'PTZ not supported in generic RTSP driver.' });
});

// Info
app.get('/info', (req, res) => {
    res.json({
        model: cameraInfo.model,
        manufacturer: cameraInfo.manufacturer,
        streaming: cameraInfo.streaming(),
        ip: cameraInfo.ip,
        rtsp_port: cameraInfo.rtsp_port,
        rtsp_path: cameraInfo.rtsp_path
    });
});

// Start Stream
app.post('/stream/start', async (req, res) => {
    if (streaming) {
        return res.json({ status: 'already streaming' });
    }
    try {
        const rtspUrl = makeRtspUrl();
        currentStream = new RtspMjpegProxy(rtspUrl);
        await currentStream.start();
        streaming = true;
        res.json({ status: 'started' });
    } catch (err) {
        streaming = false;
        currentStream = null;
        res.status(500).json({ status: 'error', message: err.message });
    }
});

// Stop Stream
app.post('/stream/stop', (req, res) => {
    if (!streaming) {
        return res.json({ status: 'not streaming' });
    }
    if (currentStream) {
        currentStream.stop();
        currentStream = null;
    }
    streaming = false;
    res.json({ status: 'stopped' });
});

// MJPEG HTTP Stream
app.get('/stream.mjpeg', (req, res) => {
    if (!streaming || !currentStream) {
        res.status(503).send('Stream not started');
        return;
    }
    res.writeHead(200, {
        'Content-Type': 'multipart/x-mixed-replace; boundary=' + BOUNDARY,
        'Connection': 'close',
        'Cache-Control': 'no-cache',
        'Pragma': 'no-cache'
    });

    const mjpegStream = currentStream.getMjpegStream();
    const clientStream = new PassThrough();
    mjpegStream.pipe(clientStream);
    clientStream.pipe(res);

    req.on('close', () => {
        clientStream.end();
    });
});

// ========== START SERVER ==========
const server = http.createServer(app);
server.listen(SERVER_PORT, SERVER_HOST, () => {
    console.log(`RTSP Camera HTTP Proxy server running at http://${SERVER_HOST}:${SERVER_PORT}/`);
    console.log(`MJPEG stream available at http://${SERVER_HOST}:${SERVER_PORT}/stream.mjpeg (after /stream/start)`);
});