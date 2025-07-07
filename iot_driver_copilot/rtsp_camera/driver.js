const http = require('http');
const net = require('net');
const url = require('url');
const { spawn } = require('child_process');

// ==== CONFIGURATION ====
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const DEVICE_RTSP_PORT = parseInt(process.env.DEVICE_RTSP_PORT, 10) || 554;
const DEVICE_RTSP_PATH = process.env.DEVICE_RTSP_PATH || '/stream';
const DEVICE_RTSP_USERNAME = process.env.DEVICE_RTSP_USERNAME || '';
const DEVICE_RTSP_PASSWORD = process.env.DEVICE_RTSP_PASSWORD || '';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT, 10) || 8080;

// ==== SIMPLE INTERNAL STATE ====
let streamActive = false;

// ==== STREAMING SESSION ====
let clients = [];
let ffmpegProcess = null;

// ==== UTILS ====
function getRtspUrl() {
  let auth = '';
  if (DEVICE_RTSP_USERNAME && DEVICE_RTSP_PASSWORD) {
    auth = `${DEVICE_RTSP_USERNAME}:${DEVICE_RTSP_PASSWORD}@`;
  }
  return `rtsp://${auth}${DEVICE_IP}:${DEVICE_RTSP_PORT}${DEVICE_RTSP_PATH}`;
}

function getEmbedSnippet(httpStreamUrl) {
  return `<video src="${httpStreamUrl}" controls autoplay></video>`;
}

// ==== FFMPEG STREAMING BRIDGE (NO COMMAND EXECUTION ALLOWED) ====
// Since we cannot run external commands, we need to implement a minimal RTSP client and RTP depacketizer for MJPEG or H264
// For simplicity, let's proxy the RTP directly as multipart/x-mixed-replace if the camera supports MJPEG over RTSP (most generic cams do).
// Otherwise, only provide the HTTP endpoint and HTML5 <video> snippet with the HTTP stream URL

// ---- Basic RTSP Proxy for MJPEG or H264 ----
const { EventEmitter } = require('events');
const { PassThrough } = require('stream');
const base64 = require('base-64');

// RTSP Client for MJPEG/H264 over RTP (minimal, not production-grade)
class RTSPtoMjpegProxy extends EventEmitter {
  constructor(rtspUrl) {
    super();
    this.rtspUrl = rtspUrl;
    this.rtspHost = url.parse(rtspUrl).hostname;
    this.rtspPort = parseInt(url.parse(rtspUrl).port, 10) || 554;
    this.rtspPath = url.parse(rtspUrl).pathname + (url.parse(rtspUrl).search || '');
    this.username = url.parse(rtspUrl).auth ? url.parse(rtspUrl).auth.split(':')[0] : '';
    this.password = url.parse(rtspUrl).auth ? url.parse(rtspUrl).auth.split(':')[1] : '';
    this.cseq = 1;
    this.session = null;
    this.client = null;
    this.rtpPort = null;
    this.rtpServer = null;
    this.frameStream = new PassThrough();
    this.running = false;
    this.isMjpeg = false;
  }

  async start() {
    this.running = true;
    await this.setupRtpServer();
    await this.connectRtsp();
    await this.sendDescribe();
    await this.sendSetup();
    await this.sendPlay();
  }

  stop() {
    this.running = false;
    if (this.client) {
      this.client.end();
    }
    if (this.rtpServer) {
      this.rtpServer.close();
    }
    this.frameStream.end();
  }

  async setupRtpServer() {
    return new Promise((resolve, reject) => {
      this.rtpServer = dgram.createSocket('udp4');
      this.rtpServer.on('message', (msg) => {
        // Parse RTP packets (here we only handle MJPEG simple case)
        // For MJPEG, each RTP packet usually contains a full JPEG frame
        // For H264, you'd need a NALU assembler (not feasible in a short script)
        if (this.isMjpeg) {
          this.frameStream.write(
            `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${msg.length}\r\n\r\n`
          );
          this.frameStream.write(msg);
          this.frameStream.write('\r\n');
        }
      });
      this.rtpServer.bind(0, () => {
        this.rtpPort = this.rtpServer.address().port;
        resolve();
      });
    });
  }

  async connectRtsp() {
    this.client = net.connect(this.rtspPort, this.rtspHost);
    this.client.setEncoding('utf8');
    this.client.on('data', (data) => this.onRtspData(data));
    this.rtspBuffer = '';
  }

  sendRtsp(cmd, headers = '') {
    let req =
      `${cmd} ${this.rtspUrl} RTSP/1.0\r\n` +
      `CSeq: ${this.cseq++}\r\n` +
      (this.session ? `Session: ${this.session}\r\n` : '') +
      (this.username && this.password
        ? `Authorization: Basic ${base64.encode(`${this.username}:${this.password}`)}\r\n`
        : '') +
      headers +
      `\r\n`;
    this.client.write(req);
  }

  async sendDescribe() {
    this.sendRtsp('DESCRIBE', 'Accept: application/sdp\r\n');
  }

  async sendSetup() {
    // Use UDP unicast for simplicity
    this.sendRtsp(
      'SETUP',
      `Transport: RTP/AVP;unicast;client_port=${this.rtpPort}-${this.rtpPort + 1}\r\n`
    );
  }

  async sendPlay() {
    this.sendRtsp('PLAY');
  }

  onRtspData(data) {
    this.rtspBuffer += data;
    // Parse RTSP responses (simplified)
    let lines = this.rtspBuffer.split('\r\n');
    if (lines[0].startsWith('RTSP/1.0')) {
      if (lines[0].includes('200')) {
        if (lines.some((l) => l.startsWith('Session:'))) {
          this.session = lines.find((l) => l.startsWith('Session:')).split(' ')[1].split(';')[0];
        }
        if (lines.some((l) => l.toLowerCase().includes('m=video'))) {
          if (lines.some((l) => l.toLowerCase().includes('mjpeg'))) {
            this.isMjpeg = true;
          }
        }
      }
      // Reset buffer after handling response
      this.rtspBuffer = '';
    }
  }

  getStream() {
    return this.frameStream;
  }
}

// ==== HTTP SERVER LOGIC ====
const dgram = require('dgram');

function startRtspProxy() {
  if (ffmpegProcess || streamActive) return;
  streamActive = true;

  // Start RTSP->HTTP proxy (MJPEG only)
  const rtspProxy = new RTSPtoMjpegProxy(getRtspUrl());
  rtspProxy.start().catch((err) => {
    streamActive = false;
  });
  ffmpegProcess = rtspProxy;
}

function stopRtspProxy() {
  if (ffmpegProcess) {
    ffmpegProcess.stop();
    ffmpegProcess = null;
  }
  streamActive = false;
  clients.forEach((res) => {
    try {
      res.end();
    } catch (_) {}
  });
  clients = [];
}

function handleStreamRequest(req, res) {
  startRtspProxy();
  res.writeHead(200, {
    'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
    'Cache-Control': 'no-cache',
    'Connection': 'close'
  });
  clients.push(res);

  if (ffmpegProcess && ffmpegProcess.getStream) {
    const pipe = ffmpegProcess.getStream().pipe(res);
    req.on('close', () => {
      pipe.unpipe(res);
      clients = clients.filter((c) => c !== res);
      if (clients.length === 0) {
        stopRtspProxy();
      }
    });
  } else {
    res.end();
  }
}

function sendStreamInfoJson(res) {
  const httpStreamUrl = `http://${SERVER_HOST === '0.0.0.0' ? 'localhost' : SERVER_HOST}:${SERVER_PORT}/stream`;
  const rtspUrl = getRtspUrl();
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(
    JSON.stringify({
      stream: {
        http: httpStreamUrl,
        rtsp: rtspUrl,
        embed: getEmbedSnippet(httpStreamUrl),
        active: streamActive
      }
    })
  );
}

function handleApi(req, res) {
  const parsedUrl = url.parse(req.url, true);
  if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
    // Return stream info (URL + embed snippet)
    return sendStreamInfoJson(res);
  }
  if (req.method === 'POST' && (parsedUrl.pathname === '/commands/start' || parsedUrl.pathname === '/cmd/start')) {
    if (!streamActive) startRtspProxy();
    sendStreamInfoJson(res);
    return;
  }
  if (req.method === 'POST' && (parsedUrl.pathname === '/commands/stop' || parsedUrl.pathname === '/cmd/stop')) {
    stopRtspProxy();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ stopped: true }));
    return;
  }
  if (req.method === 'GET' && parsedUrl.pathname === '/stream.mjpeg') {
    // Provide MJPEG over HTTP for video tag
    return handleStreamRequest(req, res);
  }
  // Fallback
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'Not found' }));
}

// ==== SERVER ====
const server = http.createServer((req, res) => {
  const parsedUrl = url.parse(req.url, true);
  if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
    // Serve MJPEG HTTP stream for browser
    handleStreamRequest(req, res);
    return;
  }
  handleApi(req, res);
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
  // Ready to accept connections
});