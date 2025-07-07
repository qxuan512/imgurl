const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { PassThrough } = require('stream');

// Configuration from environment variables
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const DEVICE_RTSP_PORT = process.env.DEVICE_RTSP_PORT || '554';
const DEVICE_RTSP_USER = process.env.DEVICE_RTSP_USER || '';
const DEVICE_RTSP_PASS = process.env.DEVICE_RTSP_PASS || '';
const DEVICE_RTSP_PATH = process.env.DEVICE_RTSP_PATH || 'stream1';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT) || 8080;

// Helper for RTSP URL construction
function buildRtspUrl() {
  let auth = '';
  if (DEVICE_RTSP_USER && DEVICE_RTSP_PASS) {
    auth = `${encodeURIComponent(DEVICE_RTSP_USER)}:${encodeURIComponent(DEVICE_RTSP_PASS)}@`;
  } else if (DEVICE_RTSP_USER) {
    auth = `${encodeURIComponent(DEVICE_RTSP_USER)}@`;
  }
  return `rtsp://${auth}${DEVICE_IP}:${DEVICE_RTSP_PORT}/${DEVICE_RTSP_PATH}`;
}

// Streaming session state
let activeSession = null;
let streamClients = [];
let lastStreamUrl = '';
let streamStatus = 'stopped';

function startStreaming() {
  if (activeSession) {
    return { status: 'already_running', url: lastStreamUrl };
  }
  const rtspUrl = buildRtspUrl();
  // Use ffmpeg to proxy RTSP to HTTP MJPEG stream (no third-party commands via shell, only spawn)
  // ffmpeg input: RTSP, output: MJPEG over pipe
  // ffmpeg must be in PATH
  const ffmpegArgs = [
    '-rtsp_transport', 'tcp',
    '-i', rtspUrl,
    '-f', 'mjpeg',
    '-q:v', '5',
    '-r', '15',
    '-an',
    '-'
  ];
  const ffmpeg = spawn('ffmpeg', ffmpegArgs, { stdio: ['ignore', 'pipe', 'ignore'] });
  const pass = new PassThrough();

  ffmpeg.stdout.pipe(pass);

  ffmpeg.on('exit', () => {
    stopStreaming();
  });

  ffmpeg.on('error', () => {
    stopStreaming();
  });

  activeSession = { ffmpeg, pass };
  lastStreamUrl = `http://${SERVER_HOST}:${SERVER_PORT}/stream/mjpeg`;
  streamStatus = 'running';
  return { status: 'started', url: lastStreamUrl };
}

function stopStreaming() {
  if (activeSession) {
    try {
      activeSession.ffmpeg.kill('SIGTERM');
    } catch (e) {}
    activeSession = null;
    streamStatus = 'stopped';
    lastStreamUrl = '';
    // Disconnect all clients
    streamClients.forEach(res => {
      try { res.end(); } catch (e) {}
    });
    streamClients = [];
  }
}

// HTTP Server
const server = http.createServer((req, res) => {
  const parsed = url.parse(req.url, true);

  // POST /cmd/start
  if (req.method === 'POST' && parsed.pathname === '/cmd/start') {
    const result = startStreaming();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      status: result.status,
      stream_url: result.url,
    }));
    return;
  }

  // POST /cmd/stop
  if (req.method === 'POST' && parsed.pathname === '/cmd/stop') {
    stopStreaming();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'stopped' }));
    return;
  }

  // GET /stream
  if (req.method === 'GET' && parsed.pathname === '/stream') {
    const url = lastStreamUrl || `http://${SERVER_HOST}:${SERVER_PORT}/stream/mjpeg`;
    const html5 = `<video src="${url}" controls autoplay></video>`;
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      stream_url: url,
      embed_html: html5,
      status: streamStatus,
    }));
    return;
  }

  // GET /stream/mjpeg (for browser/video tag)
  if (req.method === 'GET' && parsed.pathname === '/stream/mjpeg') {
    if (!activeSession) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('Stream not started');
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=ffserver',
      'Cache-Control': 'no-cache',
      'Connection': 'close',
      'Pragma': 'no-cache'
    });
    streamClients.push(res);
    const pipe = activeSession.pass.pipe(new PassThrough());
    pipe.on('data', chunk => {
      res.write(`--ffserver\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
      res.write(chunk);
      res.write('\r\n');
    });
    pipe.on('end', () => {
      try { res.end(); } catch (e) {}
    });
    res.on('close', () => {
      pipe.destroy();
      streamClients = streamClients.filter(r => r !== res);
    });
    return;
  }

  // 404
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'Not Found' }));
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
  // Ready
});