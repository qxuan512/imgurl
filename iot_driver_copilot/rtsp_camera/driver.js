const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { Readable } = require('stream');

// ====== Environment Variables ======
const RTSP_URL = process.env.RTSP_URL; // e.g., rtsp://user:pass@ip:port/path
const HTTP_HOST = process.env.HTTP_HOST || '0.0.0.0';
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '8080', 10);
const SNAPSHOT_WIDTH = parseInt(process.env.SNAPSHOT_WIDTH || '1280', 10);
const SNAPSHOT_HEIGHT = parseInt(process.env.SNAPSHOT_HEIGHT || '720', 10);

// ====== Validations ======
if (!RTSP_URL) {
  throw new Error('RTSP_URL environment variable is required.');
}

// ====== State ======
let streamClients = [];
let ffmpegStreamProcess = null;
let isStreaming = false;

// ====== Helper: Start RTSP -> HTTP Proxy with ffmpeg ======
function startStreaming() {
  if (isStreaming) return true;
  // ffmpeg -rtsp_transport tcp -i <RTSP_URL> -f mpegts -codec:v mpeg1video -codec:a mp2 -
  ffmpegStreamProcess = spawn(
    'ffmpeg', [
      '-rtsp_transport', 'tcp',
      '-i', RTSP_URL,
      '-f', 'mpegts',
      '-codec:v', 'mpeg1video',
      '-codec:a', 'mp2',
      '-'
    ],
    { stdio: ['ignore', 'pipe', 'ignore'] }
  );

  ffmpegStreamProcess.on('exit', () => {
    isStreaming = false;
    ffmpegStreamProcess = null;
    streamClients.forEach(res => res.end());
    streamClients = [];
  });

  ffmpegStreamProcess.stdout.on('data', (chunk) => {
    streamClients.forEach(res => res.write(chunk));
  });

  isStreaming = true;
  return true;
}

function stopStreaming() {
  if (ffmpegStreamProcess) {
    ffmpegStreamProcess.kill('SIGTERM');
    ffmpegStreamProcess = null;
    isStreaming = false;
    streamClients.forEach(res => res.end());
    streamClients = [];
    return true;
  }
  return false;
}

// ====== Helper: Capture Snapshot ======
function captureSnapshot(callback) {
  // ffmpeg -rtsp_transport tcp -i <RTSP_URL> -vframes 1 -f image2 -vf scale=WIDTH:HEIGHT -q:v 2 pipe:1
  let bufs = [];
  const proc = spawn(
    'ffmpeg', [
      '-rtsp_transport', 'tcp',
      '-i', RTSP_URL,
      '-vframes', '1',
      '-f', 'image2',
      '-vf', `scale=${SNAPSHOT_WIDTH}:${SNAPSHOT_HEIGHT}`,
      '-q:v', '2',
      'pipe:1'
    ],
    { stdio: ['ignore', 'pipe', 'ignore'] }
  );
  proc.stdout.on('data', chunk => bufs.push(chunk));
  proc.on('close', code => {
    if (code === 0) {
      const imgBuf = Buffer.concat(bufs);
      callback(null, imgBuf);
    } else {
      callback(new Error('Snapshot failed'));
    }
  });
}

// ====== HTTP Server ======
const server = http.createServer((req, res) => {
  const parsed = url.parse(req.url, true);
  // ---- POST /stream/start ----
  if (req.method === 'POST' && parsed.pathname === '/stream/start') {
    if (!isStreaming) {
      startStreaming();
    }
    res.writeHead(200, {'Content-Type': 'application/json'});
    res.end(JSON.stringify({ status: 'streaming', message: 'RTSP stream started', path: '/stream/live' }));
    return;
  }

  // ---- POST /stream/stop ----
  if (req.method === 'POST' && parsed.pathname === '/stream/stop') {
    if (isStreaming) {
      stopStreaming();
      res.writeHead(200, {'Content-Type': 'application/json'});
      res.end(JSON.stringify({ status: 'stopped', message: 'RTSP stream stopped' }));
    } else {
      res.writeHead(400, {'Content-Type': 'application/json'});
      res.end(JSON.stringify({ status: 'not-active', message: 'RTSP stream was not active' }));
    }
    return;
  }

  // ---- GET /stream/live ----
  if (req.method === 'GET' && parsed.pathname === '/stream/live') {
    if (!isStreaming) {
      res.writeHead(503, {'Content-Type': 'application/json'});
      res.end(JSON.stringify({ error: 'stream not started. POST /stream/start first.' }));
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'video/mp2t',
      'Connection': 'keep-alive',
      'Cache-Control': 'no-cache'
    });
    streamClients.push(res);
    req.on('close', () => {
      streamClients = streamClients.filter(r => r !== res);
    });
    return;
  }

  // ---- GET /snapshot ----
  if (req.method === 'GET' && parsed.pathname === '/snapshot') {
    captureSnapshot((err, imgBuf) => {
      if (err) {
        res.writeHead(500, {'Content-Type': 'application/json'});
        res.end(JSON.stringify({ status: 'error', message: 'Snapshot failed' }));
      } else {
        res.writeHead(200, {'Content-Type': 'application/json'});
        res.end(JSON.stringify({
          status: 'ok',
          content_type: 'image/jpeg',
          data: imgBuf.toString('base64')
        }));
      }
    });
    return;
  }

  // 404
  res.writeHead(404, {'Content-Type': 'application/json'});
  res.end(JSON.stringify({ error: 'not found' }));
});

// ====== Start Server ======
server.listen(HTTP_PORT, HTTP_HOST, () => {
  // Server started
});