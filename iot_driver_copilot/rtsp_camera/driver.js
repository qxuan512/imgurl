const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { StringDecoder } = require('string_decoder');

// ==== Config from ENV ====
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const DEVICE_RTSP_PORT = process.env.DEVICE_RTSP_PORT || '554';
const DEVICE_RTSP_USER = process.env.DEVICE_RTSP_USER || '';
const DEVICE_RTSP_PASS = process.env.DEVICE_RTSP_PASS || '';
const DEVICE_RTSP_PATH = process.env.DEVICE_RTSP_PATH || 'stream';
const HTTP_SERVER_HOST = process.env.HTTP_SERVER_HOST || '0.0.0.0';
const HTTP_SERVER_PORT = parseInt(process.env.HTTP_SERVER_PORT, 10) || 8080;
const HTTP_STREAM_PATH = process.env.HTTP_STREAM_PATH || '/live.mjpeg';
const STREAM_CODEC = process.env.STREAM_CODEC || 'mjpeg'; // Only mjpeg will be proxied for browser

// ==== Internal State ====
let streamStatus = 'stopped';
let streamParams = {
  resolution: '1280x720',
  bitrate: '2048k',
  codec: STREAM_CODEC,
};
let ffmpegProcess = null;
let clients = [];

// ==== Helper: RTSP URL generator ====
function getRTSPUrl() {
  let auth = DEVICE_RTSP_USER && DEVICE_RTSP_PASS ? `${DEVICE_RTSP_USER}:${DEVICE_RTSP_PASS}@` : '';
  return `rtsp://${auth}${DEVICE_IP}:${DEVICE_RTSP_PORT}/${DEVICE_RTSP_PATH}`;
}

// ==== Helper: Start MJPEG Stream (via ffmpeg) ====
function startStream(customParams = {}) {
  if (ffmpegProcess) return;
  streamStatus = 'streaming';
  streamParams = { ...streamParams, ...customParams };

  // Build FFmpeg args
  // Inputs:
  // -i <RTSP-URL>
  // Outputs:
  // -f mjpeg  (for browser)
  // - (stdout)
  const args = [
    '-rtsp_transport', 'tcp',
    '-i', getRTSPUrl(),
    '-f', 'mjpeg',
    '-vf', `scale=${streamParams.resolution}`,
    '-q:v', '5',
    '-an',
    '-'
  ];

  ffmpegProcess = spawn('ffmpeg', args, { stdio: ['ignore', 'pipe', 'ignore'] });

  // When ffmpeg outputs data, push to clients
  ffmpegProcess.stdout.on('data', chunk => {
    clients.forEach(res => {
      res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
      res.write(chunk);
      res.write('\r\n');
    });
  });

  // On ffmpeg exit, cleanup
  ffmpegProcess.on('exit', () => {
    ffmpegProcess = null;
    streamStatus = 'stopped';
    clients.forEach(res => {
      try { res.end(); } catch {}
    });
    clients = [];
  });
}

function stopStream() {
  if (ffmpegProcess) {
    ffmpegProcess.kill('SIGTERM');
    ffmpegProcess = null;
    streamStatus = 'stopped';
    clients.forEach(res => {
      try { res.end(); } catch {}
    });
    clients = [];
  }
}

// ==== HTTP Server ====
const server = http.createServer((req, res) => {
  const parsedUrl = url.parse(req.url, true);
  const decoder = new StringDecoder('utf8');
  let buffer = '';

  // ==== MJPEG Proxy Endpoint ====
  if (req.method === 'GET' && parsedUrl.pathname === HTTP_STREAM_PATH) {
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
      'Cache-Control': 'no-cache',
      'Connection': 'close',
      'Pragma': 'no-cache'
    });
    clients.push(res);

    // Start stream if not running
    if (!ffmpegProcess) {
      startStream();
    }

    // Remove client on close
    req.on('close', () => {
      clients = clients.filter(c => c !== res);
      if (clients.length === 0) {
        stopStream();
      }
    });
    return;
  }

  // ==== API: GET /stream ====
  if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      status: streamStatus,
      rtsp_url: getRTSPUrl(),
      mjpeg_stream_url: `http://${HTTP_SERVER_HOST}:${HTTP_SERVER_PORT}${HTTP_STREAM_PATH}`,
      params: streamParams
    }));
    return;
  }

  // ==== API: POST /stream/start ====
  if (req.method === 'POST' && parsedUrl.pathname === '/stream/start') {
    req.on('data', d => buffer += decoder.write(d));
    req.on('end', () => {
      buffer += decoder.end();
      let payload = {};
      try { payload = JSON.parse(buffer); } catch {}
      startStream(payload);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ status: streamStatus, params: streamParams }));
    });
    return;
  }

  // ==== API: POST /stream/stop ====
  if (req.method === 'POST' && parsedUrl.pathname === '/stream/stop') {
    stopStream();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: streamStatus }));
    return;
  }

  // ==== API: PUT /stream/params ====
  if (req.method === 'PUT' && parsedUrl.pathname === '/stream/params') {
    req.on('data', d => buffer += decoder.write(d));
    req.on('end', () => {
      buffer += decoder.end();
      let payload = {};
      try { payload = JSON.parse(buffer); } catch {}
      // Merge new params
      streamParams = { ...streamParams, ...payload };
      // Restart stream if running
      if (streamStatus === 'streaming') {
        stopStream();
        startStream(streamParams);
      }
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ params: streamParams }));
    });
    return;
  }

  // ==== 404 ====
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'Not found' }));
});

// ==== Start Server ====
server.listen(HTTP_SERVER_PORT, HTTP_SERVER_HOST, () => {
  // No log as per instructions
});