```javascript
const http = require('http');
const { spawn } = require('child_process');
const { PassThrough } = require('stream');
const url = require('url');

// Environment variables
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const SERVER_PORT = parseInt(process.env.SERVER_PORT || '8080');
const RTSP_URL = process.env.RTSP_URL || '';
const HTTP_STREAM_PORT = parseInt(process.env.HTTP_STREAM_PORT || '8081'); // for HTTP stream endpoint

if (!RTSP_URL) {
  console.error('RTSP_URL environment variable is required');
  process.exit(1);
}

// State
let streaming = false;
let streamProcess = null;
let streamClients = [];
const streamPassThrough = new PassThrough();

// Helper: Launch ffmpeg to proxy RTSP to HTTP/MJPEG
function startStream() {
  if (streaming) return;
  // ffmpeg command: input RTSP, output MJPEG to stdout
  streamProcess = spawn('ffmpeg', [
    '-rtsp_transport', 'tcp',
    '-i', RTSP_URL,
    '-f', 'mjpeg',
    '-q:v', '5',
    '-preset', 'ultrafast',
    '-r', '15',
    '-an', // no audio
    '-'
  ]);

  streamProcess.stdout.on('data', (chunk) => {
    streamPassThrough.write(chunk);
  });

  streamProcess.stderr.on('data', () => {
    // Suppress ffmpeg logs
  });

  streamProcess.on('close', () => {
    streaming = false;
    streamProcess = null;
    streamPassThrough.end();
  });

  streaming = true;
}

function stopStream() {
  if (!streaming || !streamProcess) return;
  streamProcess.kill('SIGKILL');
  streaming = false;
  streamProcess = null;
  streamPassThrough.end();
}

// HTTP MJPEG Streaming Server
const httpStreamServer = http.createServer((req, res) => {
  if (req.url !== '/video') {
    res.writeHead(404);
    res.end();
    return;
  }
  if (!streaming) {
    startStream();
    // Wait a bit to buffer initial data
  }
  res.writeHead(200, {
    'Content-Type': 'multipart/x-mixed-replace; boundary=ffserver',
    'Cache-Control': 'no-cache',
    'Connection': 'close',
    'Pragma': 'no-cache'
  });

  const onData = (chunk) => {
    res.write(`--ffserver\r\nContent-Type: image/jpeg\r\nContent-Length: ${chunk.length}\r\n\r\n`);
    res.write(chunk);
    res.write('\r\n');
  };
  streamPassThrough.on('data', onData);

  res.on('close', () => {
    streamPassThrough.removeListener('data', onData);
    streamClients = streamClients.filter(c => c !== res);
    if (streamClients.length === 0) {
      stopStream();
    }
  });

  streamClients.push(res);
});

// Main HTTP API Server
const apiServer = http.createServer(async (req, res) => {
  const parsed = url.parse(req.url, true);

  // CORS for browser access
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET,POST,OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  // GET /stream (two endpoints)
  if (req.method === 'GET' && parsed.pathname === '/stream') {
    // Return URLs and embed snippet
    const mjpegURL = `http://${SERVER_HOST}:${HTTP_STREAM_PORT}/video`;
    res.writeHead(200, {'Content-Type': 'application/json'});
    res.end(JSON.stringify({
      status: streaming ? 'streaming' : 'stopped',
      urls: {
        mjpeg: mjpegURL
      },
      html5_embed: `<img src="${mjpegURL}" style="max-width:100%;"/>`
    }));
    return;
  }

  // POST /commands/start and /cmd/start
  if (req.method === 'POST' && (parsed.pathname === '/commands/start' || parsed.pathname === '/cmd/start')) {
    if (!streaming) startStream();
    res.writeHead(200, {'Content-Type': 'application/json'});
    res.end(JSON.stringify({
      status: 'streaming',
      message: 'RTSP stream started',
      mjpeg_url: `http://${SERVER_HOST}:${HTTP_STREAM_PORT}/video`
    }));
    return;
  }

  // POST /commands/stop and /cmd/stop
  if (req.method === 'POST' && (parsed.pathname === '/commands/stop' || parsed.pathname === '/cmd/stop')) {
    stopStream();
    res.writeHead(200, {'Content-Type': 'application/json'});
    res.end(JSON.stringify({
      status: 'stopped',
      message: 'RTSP stream stopped'
    }));
    return;
  }

  // 404 fallback
  res.writeHead(404, {'Content-Type': 'application/json'});
  res.end(JSON.stringify({error: 'Not found'}));
});

// Start servers
apiServer.listen(SERVER_PORT, SERVER_HOST, () => {
  console.log(`API Server listening on http://${SERVER_HOST}:${SERVER_PORT}`);
});
httpStreamServer.listen(HTTP_STREAM_PORT, SERVER_HOST, () => {
  console.log(`HTTP Stream (MJPEG) Server on http://${SERVER_HOST}:${HTTP_STREAM_PORT}/video`);
});
```
