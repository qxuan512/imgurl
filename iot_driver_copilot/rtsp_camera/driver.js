const http = require('http');
const { spawn } = require('child_process');
const url = require('url');

// Environment Variables
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT || '8080', 10);
const RTSP_CAMERA_IP = process.env.RTSP_CAMERA_IP || '127.0.0.1';
const RTSP_CAMERA_PORT = process.env.RTSP_CAMERA_PORT || '554';
const RTSP_USERNAME = process.env.RTSP_USERNAME || '';
const RTSP_PASSWORD = process.env.RTSP_PASSWORD || '';
const RTSP_PATH = process.env.RTSP_PATH || 'stream1';
const VIDEO_TRANSCODE_PORT = process.env.VIDEO_TRANSCODE_PORT || '8554';
const AUDIO_TRANSCODE_PORT = process.env.AUDIO_TRANSCODE_PORT || '8555';

// RTSP URL construction
function getRtspUrl() {
  let auth = '';
  if (RTSP_USERNAME && RTSP_PASSWORD) {
    auth = `${encodeURIComponent(RTSP_USERNAME)}:${encodeURIComponent(RTSP_PASSWORD)}@`;
  }
  return `rtsp://${auth}${RTSP_CAMERA_IP}:${RTSP_CAMERA_PORT}/${RTSP_PATH}`;
}

// Stream State
let streamActive = false;
let ffmpegVideoProc = null;
let ffmpegAudioProc = null;

// Start Stream Command
function startStream(cb) {
  if (streamActive) {
    cb({ status: 'already_started' });
    return;
  }
  const rtspUrl = getRtspUrl();

  // Video: proxy as MJPEG stream over HTTP
  ffmpegVideoProc = spawn('ffmpeg', [
    '-rtsp_transport', 'tcp',
    '-i', rtspUrl,
    '-an', // no audio
    '-f', 'mjpeg',
    '-q:v', '5',
    '-'
  ], { stdio: ['ignore', 'pipe', 'ignore'] });

  // Audio: proxy as AAC over HTTP
  ffmpegAudioProc = spawn('ffmpeg', [
    '-rtsp_transport', 'tcp',
    '-i', rtspUrl,
    '-vn', // no video
    '-acodec', 'aac',
    '-f', 'adts',
    '-'
  ], { stdio: ['ignore', 'pipe', 'ignore'] });

  ffmpegVideoProc.on('exit', () => { ffmpegVideoProc = null; streamActive = false; });
  ffmpegAudioProc.on('exit', () => { ffmpegAudioProc = null; streamActive = false; });

  streamActive = true;
  cb({ status: 'started' });
}

// Stop Stream Command
function stopStream(cb) {
  if (!streamActive) {
    cb({ status: 'already_stopped' });
    return;
  }
  if (ffmpegVideoProc) {
    ffmpegVideoProc.kill('SIGTERM');
    ffmpegVideoProc = null;
  }
  if (ffmpegAudioProc) {
    ffmpegAudioProc.kill('SIGTERM');
    ffmpegAudioProc = null;
  }
  streamActive = false;
  cb({ status: 'stopped' });
}

// HTTP Server
const server = http.createServer((req, res) => {
  const parsedUrl = url.parse(req.url, true);
  const method = req.method;
  const path = parsedUrl.pathname;

  // API Endpoints

  // POST /commands/start and /cmd/start
  if ((path === '/commands/start' || path === '/cmd/start') && method === 'POST') {
    startStream((result) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(result));
    });
    return;
  }

  // POST /commands/stop and /cmd/stop
  if ((path === '/commands/stop' || path === '/cmd/stop') && method === 'POST') {
    stopStream((result) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(result));
    });
    return;
  }

  // GET /stream
  if (path === '/stream' && method === 'GET') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      active: streamActive,
      formats: {
        video: ['H.264', 'H.265', 'MJPEG'],
        audio: ['AAC', 'G.711']
      },
      rtsp_url: getRtspUrl()
    }));
    return;
  }

  // GET /streams/video
  if (path === '/streams/video' && method === 'GET') {
    if (!streamActive || !ffmpegVideoProc) {
      res.writeHead(503, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'Stream not active' }));
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=ffserver',
      'Cache-Control': 'no-cache',
      'Connection': 'close',
      'Pragma': 'no-cache'
    });
    ffmpegVideoProc.stdout.pipe(res);
    req.on('close', () => {
      try { res.end(); } catch (e) {}
    });
    return;
  }

  // GET /streams/audio
  if (path === '/streams/audio' && method === 'GET') {
    if (!streamActive || !ffmpegAudioProc) {
      res.writeHead(503, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'Stream not active' }));
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'audio/aac',
      'Cache-Control': 'no-cache',
      'Connection': 'close',
      'Pragma': 'no-cache'
    });
    ffmpegAudioProc.stdout.pipe(res);
    req.on('close', () => {
      try { res.end(); } catch (e) {}
    });
    return;
  }

  // Unknown endpoint
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'not_found' }));
});

// Start server
server.listen(SERVER_PORT, SERVER_HOST, () => {
  console.log(`RTSP Camera HTTP Proxy running at http://${SERVER_HOST}:${SERVER_PORT}/`);
});