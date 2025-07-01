const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { Readable } = require('stream');

// Configuration via environment variables
const DEVICE_IP = process.env.DEVICE_IP || '127.0.0.1';
const RTSP_PORT = process.env.RTSP_PORT || '554';
const RTSP_PATH = process.env.RTSP_PATH || '/h264';
const RTSP_USER = process.env.RTSP_USER || '';
const RTSP_PASS = process.env.RTSP_PASS || '';
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT || '8080', 10);

// Compose RTSP URL
function getRtspUrl() {
  let auth = '';
  if (RTSP_USER && RTSP_PASS) {
    auth = `${encodeURIComponent(RTSP_USER)}:${encodeURIComponent(RTSP_PASS)}@`;
  } else if (RTSP_USER) {
    auth = `${encodeURIComponent(RTSP_USER)}@`;
  }
  return `rtsp://${auth}${DEVICE_IP}:${RTSP_PORT}${RTSP_PATH}`;
}

// Stream state
let streamActive = false;
let videoClients = [];
let audioClients = [];
let ffmpegVideoProcess = null;
let ffmpegAudioProcess = null;

// Utility to start FFmpeg process for video or audio streaming
function startFFmpegStream(type) {
  const rtspUrl = getRtspUrl();
  if (type === 'video' && !ffmpegVideoProcess) {
    // Video: Transcode to MJPEG for browser compatibility, output as multipart/x-mixed-replace
    ffmpegVideoProcess = spawn(
      'ffmpeg',
      [
        '-rtsp_transport', 'tcp',
        '-i', rtspUrl,
        '-an',
        '-c:v', 'mjpeg',
        '-f', 'mjpeg',
        '-q:v', '5',
        '-r', '15',
        '-'
      ],
      { stdio: ['ignore', 'pipe', 'ignore'] }
    );
    ffmpegVideoProcess.on('close', () => {
      ffmpegVideoProcess = null;
      videoClients.forEach(res => res.end());
      videoClients = [];
    });
  } else if (type === 'audio' && !ffmpegAudioProcess) {
    // Audio: Transcode to WAV (browser playable), output as wav
    ffmpegAudioProcess = spawn(
      'ffmpeg',
      [
        '-rtsp_transport', 'tcp',
        '-i', rtspUrl,
        '-vn',
        '-acodec', 'pcm_s16le',
        '-ar', '44100',
        '-ac', '2',
        '-f', 'wav',
        '-'
      ],
      { stdio: ['ignore', 'pipe', 'ignore'] }
    );
    ffmpegAudioProcess.on('close', () => {
      ffmpegAudioProcess = null;
      audioClients.forEach(res => res.end());
      audioClients = [];
    });
  }
  streamActive = true;
}

// Utility to stop FFmpeg processes
function stopFFmpegStream() {
  if (ffmpegVideoProcess) {
    ffmpegVideoProcess.kill('SIGTERM');
    ffmpegVideoProcess = null;
  }
  if (ffmpegAudioProcess) {
    ffmpegAudioProcess.kill('SIGTERM');
    ffmpegAudioProcess = null;
  }
  streamActive = false;
}

// HTTP Server
const server = http.createServer((req, res) => {
  const parsedUrl = url.parse(req.url, true);

  // POST /commands/start or /cmd/start
  if (
    (req.method === 'POST' && (parsedUrl.pathname === '/commands/start' || parsedUrl.pathname === '/cmd/start'))
  ) {
    if (!streamActive) {
      startFFmpegStream('video');
      startFFmpegStream('audio');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ status: 'started', streamActive: true }));
    } else {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ status: 'already_active', streamActive: true }));
    }
    return;
  }

  // POST /commands/stop or /cmd/stop
  if (
    (req.method === 'POST' && (parsedUrl.pathname === '/commands/stop' || parsedUrl.pathname === '/cmd/stop'))
  ) {
    if (streamActive) {
      stopFFmpegStream();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ status: 'stopped', streamActive: false }));
    } else {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ status: 'already_stopped', streamActive: false }));
    }
    return;
  }

  // GET /stream
  if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      active: streamActive,
      videoFormats: ['H.264', 'H.265', 'MJPEG'],
      audioFormats: ['AAC', 'G.711'],
      rtspUrl: getRtspUrl()
    }));
    return;
  }

  // GET /streams/video
  if (req.method === 'GET' && parsedUrl.pathname === '/streams/video') {
    if (!streamActive) startFFmpegStream('video');
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=ffserver',
      'Connection': 'close',
      'Cache-Control': 'no-cache',
      'Pragma': 'no-cache'
    });
    videoClients.push(res);

    // Pipe FFmpeg output to all video clients
    if (ffmpegVideoProcess && ffmpegVideoProcess.stdout) {
      const onData = chunk => {
        videoClients.forEach(client => {
          client.write(`--ffserver\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
          client.write(chunk);
        });
      };
      ffmpegVideoProcess.stdout.on('data', onData);

      // Remove listener when client disconnects
      res.on('close', () => {
        videoClients = videoClients.filter(c => c !== res);
        if (videoClients.length === 0 && ffmpegVideoProcess) {
          ffmpegVideoProcess.stdout.removeListener('data', onData);
        }
      });
    }
    return;
  }

  // GET /streams/audio
  if (req.method === 'GET' && parsedUrl.pathname === '/streams/audio') {
    if (!streamActive) startFFmpegStream('audio');
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Connection': 'close',
      'Cache-Control': 'no-cache',
      'Pragma': 'no-cache'
    });
    audioClients.push(res);

    if (ffmpegAudioProcess && ffmpegAudioProcess.stdout) {
      const onData = chunk => {
        audioClients.forEach(client => {
          client.write(chunk);
        });
      };
      ffmpegAudioProcess.stdout.on('data', onData);

      res.on('close', () => {
        audioClients = audioClients.filter(c => c !== res);
        if (audioClients.length === 0 && ffmpegAudioProcess) {
          ffmpegAudioProcess.stdout.removeListener('data', onData);
        }
      });
    }
    return;
  }

  // 404 Not Found for all other routes
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'Not found' }));
});

server.listen(SERVER_PORT, SERVER_HOST, () => {
  // Server started
});