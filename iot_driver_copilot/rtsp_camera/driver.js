const http = require('http');
const url = require('url');
const Stream = require('stream');
const { spawn } = require('child_process');

// ==== Configuration ====
const CAMERA_HOST = process.env.CAMERA_HOST || '127.0.0.1';
const CAMERA_PORT = process.env.CAMERA_RTSP_PORT || '554';
const CAMERA_USER = process.env.CAMERA_USER || '';
const CAMERA_PASS = process.env.CAMERA_PASS || '';
const CAMERA_PATH = process.env.CAMERA_PATH || 'stream1';
const HTTP_HOST = process.env.HTTP_HOST || '0.0.0.0';
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '8080', 10);

// Supported codecs (could be used for info/validation)
const SUPPORTED_VIDEO_CODECS = ['H264', 'H265', 'MJPEG'];
const SUPPORTED_AUDIO_CODECS = ['AAC', 'G711'];

// Streaming State
let streamActive = false;
let streamClients = []; // { res, type }
let ffmpegProcess = null;

// ==== RTSP URL Construction ====
function buildRTSPUrl() {
  let auth = '';
  if (CAMERA_USER && CAMERA_PASS) {
    auth = `${encodeURIComponent(CAMERA_USER)}:${encodeURIComponent(CAMERA_PASS)}@`;
  } else if (CAMERA_USER) {
    auth = `${encodeURIComponent(CAMERA_USER)}@`;
  }
  return `rtsp://${auth}${CAMERA_HOST}:${CAMERA_PORT}/${CAMERA_PATH}`;
}

// ==== FFmpeg Spawn (in-process piping, no exec/commandline, only for media proxy) ====
function startFFmpegStream({ video = false, audio = false } = {}) {
  if (ffmpegProcess) return;

  // Build ffmpeg args for video or audio
  const args = [
    '-rtsp_transport', 'tcp',
    '-i', buildRTSPUrl(),
    '-analyzeduration', '1000000', '-probesize', '1000000',
    // Video
    ...(video ? ['-map', '0:v:0', '-c:v', 'copy'] : []),
    // Audio
    ...(audio ? ['-map', '0:a:0', '-c:a', 'aac'] : []),
    // Output to stdout as fragmented MP4 for browser compatibility
    '-f', 'mp4',
    '-movflags', 'frag_keyframe+empty_moov+default_base_moof',
    'pipe:1'
  ];

  ffmpegProcess = spawn('ffmpeg', args, { stdio: ['ignore', 'pipe', 'ignore'] });

  ffmpegProcess.stdout.on('data', (chunk) => {
    streamClients.forEach(client => {
      if (!client.res.closed) {
        client.res.write(chunk);
      }
    });
  });

  ffmpegProcess.on('close', () => {
    ffmpegProcess = null;
    streamActive = false;
    streamClients.forEach(client => {
      if (!client.res.closed) {
        client.res.end();
      }
    });
    streamClients = [];
  });
}

function stopFFmpegStream() {
  if (ffmpegProcess) {
    ffmpegProcess.kill();
    ffmpegProcess = null;
  }
  streamActive = false;
}

// ==== HTTP Server ====
const server = http.createServer(async (req, res) => {
  const parsedUrl = url.parse(req.url, true);

  // Helper: Parse POST Body as JSON
  function getJsonBody(req, cb) {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      try { cb(JSON.parse(body)); } catch { cb({}); }
    });
  }

  // Routing
  // Start Stream: /commands/start or /cmd/start (POST)
  if (
    (req.method === 'POST' && parsedUrl.pathname === '/commands/start') ||
    (req.method === 'POST' && parsedUrl.pathname === '/cmd/start')
  ) {
    if (!streamActive) {
      startFFmpegStream({ video: true, audio: true });
      streamActive = true;
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'started', stream: true }));
    return;
  }

  // Stop Stream: /commands/stop or /cmd/stop (POST)
  if (
    (req.method === 'POST' && parsedUrl.pathname === '/commands/stop') ||
    (req.method === 'POST' && parsedUrl.pathname === '/cmd/stop')
  ) {
    stopFFmpegStream();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'stopped', stream: false }));
    return;
  }

  // Streaming Status & Info: /stream (GET)
  if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      streamActive,
      video: streamActive,
      audio: streamActive,
      videoCodec: 'H264/H265/MJPEG',
      audioCodec: 'AAC/G711',
      rtsp: buildRTSPUrl()
    }));
    return;
  }

  // Video Stream: /streams/video (GET)
  if (req.method === 'GET' && parsedUrl.pathname === '/streams/video') {
    // Serve as JSON with browser-compatible endpoint
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      format: 'mp4',
      codec: 'H264/H265/MJPEG',
      stream_url: `http://${HTTP_HOST}:${HTTP_PORT}/video`,
      active: streamActive
    }));
    return;
  }

  // Audio Stream: /streams/audio (GET)
  if (req.method === 'GET' && parsedUrl.pathname === '/streams/audio') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      format: 'mp4',
      codec: 'AAC/G711',
      stream_url: `http://${HTTP_HOST}:${HTTP_PORT}/audio`,
      active: streamActive
    }));
    return;
  }

  // ---- Media Streaming Endpoints ----

  // Browser/Player Video Stream Proxy: /video (GET)
  if (req.method === 'GET' && parsedUrl.pathname === '/video') {
    res.writeHead(200, {
      'Content-Type': 'video/mp4',
      'Connection': 'keep-alive',
      'Cache-Control': 'no-cache'
    });
    // Register client
    streamClients.push({ res, type: 'video' });
    if (!streamActive) {
      startFFmpegStream({ video: true, audio: true });
      streamActive = true;
    }
    req.on('close', () => {
      res.closed = true;
      streamClients = streamClients.filter(c => c.res !== res);
      if (streamClients.length === 0) stopFFmpegStream();
    });
    return;
  }

  // Browser/Player Audio Stream Proxy: /audio (GET)
  if (req.method === 'GET' && parsedUrl.pathname === '/audio') {
    res.writeHead(200, {
      'Content-Type': 'audio/mp4',
      'Connection': 'keep-alive',
      'Cache-Control': 'no-cache'
    });
    // Register client
    streamClients.push({ res, type: 'audio' });
    if (!streamActive) {
      startFFmpegStream({ video: false, audio: true });
      streamActive = true;
    }
    req.on('close', () => {
      res.closed = true;
      streamClients = streamClients.filter(c => c.res !== res);
      if (streamClients.length === 0) stopFFmpegStream();
    });
    return;
  }

  // Fallback: Not Found
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'Not found' }));
});

// ==== Start Server ====
server.listen(HTTP_PORT, HTTP_HOST, () => {
  // No logging per instruction
});

process.on('SIGINT', () => {
  stopFFmpegStream();
  process.exit();
});