const express = require('express');
const http = require('http');
const { spawn } = require('child_process');
const { Readable } = require('stream');

// Environment Variables
const RTSP_HOST = process.env.RTSP_HOST || '127.0.0.1';
const RTSP_PORT = process.env.RTSP_PORT || '554';
const RTSP_PATH = process.env.RTSP_PATH || '/stream';
const RTSP_USER = process.env.RTSP_USER || '';
const RTSP_PASS = process.env.RTSP_PASS || '';
const HTTP_HOST = process.env.HTTP_HOST || '0.0.0.0';
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '8080', 10);

const RTSP_URL =
  (RTSP_USER && RTSP_PASS
    ? `rtsp://${encodeURIComponent(RTSP_USER)}:${encodeURIComponent(RTSP_PASS)}@${RTSP_HOST}:${RTSP_PORT}${RTSP_PATH}`
    : `rtsp://${RTSP_HOST}:${RTSP_PORT}${RTSP_PATH}`);

const app = express();
app.use(express.json());

// In-memory streaming process state
let ffmpegProcess = null;
let clients = [];
let isStreaming = false;

// Utility: Start FFmpeg for MJPEG over HTTP
function startStream() {
  if (isStreaming) return;
  ffmpegProcess = spawn('ffmpeg', [
    '-rtsp_transport', 'tcp',
    '-i', RTSP_URL,
    '-f', 'mjpeg',
    '-q:v', '5',
    '-update', '1',
    '-r', '15',
    '-an',
    '-'
  ]);
  ffmpegProcess.stdout.on('data', (chunk) => {
    clients.forEach(res => {
      res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
      res.write(chunk);
      res.write('\r\n');
    });
  });
  ffmpegProcess.stderr.on('data', () => {});
  ffmpegProcess.on('exit', () => {
    isStreaming = false;
    ffmpegProcess = null;
    clients.forEach(res => res.end());
    clients = [];
  });
  isStreaming = true;
}

function stopStream() {
  if (ffmpegProcess) {
    ffmpegProcess.kill('SIGKILL');
    ffmpegProcess = null;
    isStreaming = false;
    clients.forEach(res => res.end());
    clients = [];
  }
}

// POST /stream/start
app.post(['/stream/start', '/commands/start'], (req, res) => {
  if (isStreaming) {
    return res.json({ status: 'already streaming', stream: true });
  }
  startStream();
  setTimeout(() => {
    if (isStreaming) {
      res.json({ status: 'stream started', stream: true });
    } else {
      res.status(500).json({ status: 'failed to start stream', stream: false });
    }
  }, 800); // give ffmpeg some time to initialize
});

// POST /stream/stop
app.post(['/stream/stop', '/commands/stop'], (req, res) => {
  if (!isStreaming) {
    return res.json({ status: 'not streaming', stream: false });
  }
  stopStream();
  res.json({ status: 'stream stopped', stream: false });
});

// GET /stream - HTTP MJPEG stream for browser
app.get('/stream', (req, res) => {
  res.writeHead(200, {
    'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
    'Cache-Control': 'no-cache',
    'Connection': 'close',
    'Pragma': 'no-cache'
  });
  clients.push(res);
  if (!isStreaming) startStream();
  req.on('close', () => {
    clients = clients.filter(r => r !== res);
    if (clients.length === 0) stopStream();
  });
});

// GET /snapshot - JPEG snapshot (base64 in JSON)
app.get('/snapshot', async (req, res) => {
  try {
    const chunks = [];
    const snap = spawn('ffmpeg', [
      '-rtsp_transport', 'tcp',
      '-i', RTSP_URL,
      '-frames:v', '1',
      '-f', 'image2',
      '-q:v', '2',
      '-'
    ]);
    snap.stdout.on('data', chunk => chunks.push(chunk));
    snap.stderr.on('data', () => {});
    snap.on('error', () => res.status(500).json({ error: 'Snapshot failed' }));
    snap.on('close', code => {
      if (code === 0) {
        const img = Buffer.concat(chunks);
        res.json({ image: img.toString('base64') });
      } else {
        res.status(500).json({ error: 'Snapshot failed' });
      }
    });
  } catch {
    res.status(500).json({ error: 'Snapshot failed' });
  }
});
// POST /commands/snap - JPEG snapshot (base64 in JSON)
app.post('/commands/snap', async (req, res) => {
  try {
    const chunks = [];
    const snap = spawn('ffmpeg', [
      '-rtsp_transport', 'tcp',
      '-i', RTSP_URL,
      '-frames:v', '1',
      '-f', 'image2',
      '-q:v', '2',
      '-'
    ]);
    snap.stdout.on('data', chunk => chunks.push(chunk));
    snap.stderr.on('data', () => {});
    snap.on('error', () => res.status(500).json({ error: 'Snapshot failed' }));
    snap.on('close', code => {
      if (code === 0) {
        const img = Buffer.concat(chunks);
        res.json({ image: img.toString('base64') });
      } else {
        res.status(500).json({ error: 'Snapshot failed' });
      }
    });
  } catch {
    res.status(500).json({ error: 'Snapshot failed' });
  }
});

const server = http.createServer(app);
server.listen(HTTP_PORT, HTTP_HOST, () => {
  // No startup log
});

// Graceful shutdown
process.on('SIGINT', () => {
  stopStream();
  process.exit(0);
});
process.on('SIGTERM', () => {
  stopStream();
  process.exit(0);
});