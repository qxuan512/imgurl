```javascript
const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { Readable } = require('stream');

// ==== ENVIRONMENT VARIABLE CONFIGURATION ====
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const DEVICE_RTSP_PORT = parseInt(process.env.DEVICE_RTSP_PORT, 10) || 554;
const DEVICE_RTSP_USER = process.env.DEVICE_RTSP_USER || '';
const DEVICE_RTSP_PASS = process.env.DEVICE_RTSP_PASS || '';
const DEVICE_RTSP_PATH = process.env.DEVICE_RTSP_PATH || 'Streaming/Channels/101';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT, 10) || 8080;
const STREAM_MJPEG_PORT = parseInt(process.env.STREAM_MJPEG_PORT, 10) || SERVER_PORT; // Only HTTP is used

// ==== DEVICE METADATA ====
const deviceMetadata = {
  device_name: "RTSP Camera",
  device_model: "RTSP Camera",
  manufacturer: "Various",
  device_type: "IP Camera",
  streaming: false
};

// ==== STREAM MANAGEMENT ====
let streamActive = false;
let streamClients = [];
let ffmpegProc = null;

// ==== RTSP URL BUILDER ====
function buildRtspUrl() {
  let prefix = 'rtsp://';
  if (DEVICE_RTSP_USER && DEVICE_RTSP_PASS)
    prefix += `${encodeURIComponent(DEVICE_RTSP_USER)}:${encodeURIComponent(DEVICE_RTSP_PASS)}@`;
  prefix += `${DEVICE_IP}:${DEVICE_RTSP_PORT}/${DEVICE_RTSP_PATH}`;
  return prefix;
}

// ==== MJPEG STREAMING LOGIC ====
function startFfmpeg() {
  if (ffmpegProc) return; // Already running
  const rtspUrl = buildRtspUrl();
  // FFmpeg command: input RTSP, output MJPEG frames to stdout
  ffmpegProc = spawn('ffmpeg', [
    '-rtsp_transport', 'tcp',
    '-i', rtspUrl,
    '-f', 'mjpeg',
    '-q:v', '5',
    '-r', '15',
    '-'
  ], { stdio: ['ignore', 'pipe', 'ignore'] });

  ffmpegProc.on('exit', () => {
    ffmpegProc = null;
    streamClients.forEach(client => client.end());
    streamClients = [];
    streamActive = false;
    deviceMetadata.streaming = false;
  });

  ffmpegProc.stdout.on('data', chunk => {
    // Slice and send multipart MJPEG frames
    for (let client of streamClients) {
      client.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
      client.write(chunk);
      client.write('\r\n');
    }
  });

  streamActive = true;
  deviceMetadata.streaming = true;
}

function stopFfmpeg() {
  if (ffmpegProc) {
    ffmpegProc.kill('SIGTERM');
    ffmpegProc = null;
  }
  streamActive = false;
  deviceMetadata.streaming = false;
  streamClients.forEach(client => client.end());
  streamClients = [];
}

// ==== HTTP SERVER ====
const server = http.createServer(async (req, res) => {
  const parsedUrl = url.parse(req.url, true);

  // --- [GET] /info ---
  if (req.method === 'GET' && parsedUrl.pathname === '/info') {
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify(deviceMetadata));
    return;
  }

  // --- [POST] /ptz ---
  if (req.method === 'POST' && parsedUrl.pathname === '/ptz') {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      // PTZ is device-specific; here we just echo the request
      res.writeHead(200, {'Content-Type': 'application/json'});
      res.end(JSON.stringify({
        status: 'unsupported',
        message: 'PTZ control not implemented in this generic RTSP camera driver.'
      }));
    });
    return;
  }

  // --- [POST] /stream/start ---
  if (req.method === 'POST' && parsedUrl.pathname === '/stream/start') {
    if (!streamActive) startFfmpeg();
    res.writeHead(200, {'Content-Type': 'application/json'});
    res.end(JSON.stringify({ status: 'started', streaming: streamActive }));
    return;
  }

  // --- [POST] /stream/stop ---
  if (req.method === 'POST' && parsedUrl.pathname === '/stream/stop') {
    stopFfmpeg();
    res.writeHead(200, {'Content-Type': 'application/json'});
    res.end(JSON.stringify({ status: 'stopped', streaming: streamActive }));
    return;
  }

  // --- [GET] /stream/mjpeg ---
  if (req.method === 'GET' && parsedUrl.pathname === '/stream/mjpeg') {
    // Only allow if streamActive
    if (!streamActive) {
      res.writeHead(503, {'Content-Type': 'application/json'});
      res.end(JSON.stringify({ error: 'Stream not started. POST /stream/start first.' }));
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
      'Cache-Control': 'no-cache',
      'Connection': 'close',
      'Pragma': 'no-cache'
    });
    streamClients.push(res);
    req.on('close', () => {
      // Remove client on disconnect
      streamClients = streamClients.filter(c => c !== res);
      if (streamClients.length === 0) stopFfmpeg();
    });
    return;
  }

  // --- 404 Not Found ---
  res.writeHead(404, {'Content-Type': 'application/json'});
  res.end(JSON.stringify({ error: 'Not found' }));
});

// ==== START SERVER ====
server.listen(SERVER_PORT, SERVER_HOST, () => {
  console.log(`RTSP Camera HTTP Driver running at http://${SERVER_HOST}:${SERVER_PORT}/`);
  console.log(`- MJPEG stream available at /stream/mjpeg (after /stream/start)`);
});
```
