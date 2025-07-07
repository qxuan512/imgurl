```javascript
const http = require('http');
const url = require('url');
const { spawn } = require('child_process');
const { PassThrough } = require('stream');

// Environment variables
const RTSP_URL = process.env.RTSP_URL; // e.g. rtsp://user:pass@192.168.1.10:554/stream1
const SERVER_HOST = process.env.SERVER_HOST || '0.0.0.0';
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '8000', 10);
const HTTP_STREAM_PATH = process.env.HTTP_STREAM_PATH || '/video';

if (!RTSP_URL) {
    throw new Error('RTSP_URL environment variable not set.');
}

let ffmpegProcess = null;
let videoClients = [];
let currentStreamActive = false;

// Simple state management for start/stop
function startStream() {
    if (currentStreamActive) return true;
    // Start ffmpeg process as a child
    ffmpegProcess = spawn(
        process.execPath, // node.js itself to avoid using external ffmpeg cmd, see below
        [require.resolve('./rtsp2mjpeg.js')], // local helper script, see below
        {
            env: {
                RTSP_URL,
                HTTP_STREAM_PATH,
            },
            stdio: ['ignore', 'pipe', 'inherit'],
        }
    );

    ffmpegProcess.stdout.on('data', (chunk) => {
        videoClients.forEach((res) => {
            res.write(chunk);
        });
    });

    ffmpegProcess.on('exit', () => {
        ffmpegProcess = null;
        currentStreamActive = false;
        videoClients.forEach((res) => {
            try { res.end(); } catch (e) {}
        });
        videoClients = [];
    });

    currentStreamActive = true;
    return true;
}

function stopStream() {
    if (ffmpegProcess) {
        ffmpegProcess.kill();
        ffmpegProcess = null;
    }
    currentStreamActive = false;
}

function handleVideoStream(req, res) {
    if (!currentStreamActive) {
        res.writeHead(503, {'Content-Type': 'text/plain'});
        res.end('Stream is not active.');
        return;
    }
    res.writeHead(200, {
        'Content-Type': 'multipart/x-mixed-replace; boundary=ffserver',
        'Connection': 'close',
        'Pragma': 'no-cache',
        'Cache-Control': 'no-cache',
    });
    videoClients.push(res);
    req.on('close', () => {
        videoClients = videoClients.filter((r) => r !== res);
        if (videoClients.length === 0) {
            stopStream();
        }
    });
}

// HTML5 embed snippet generator
function getEmbedHTML() {
    return `<img src="${HTTP_STREAM_PATH}" style="max-width:100%;height:auto;" alt="Camera Stream">`;
}

function sendJSON(res, obj) {
    res.writeHead(200, {'Content-Type': 'application/json'});
    res.end(JSON.stringify(obj));
}

function send404(res) {
    res.writeHead(404, {'Content-Type': 'application/json'});
    res.end(JSON.stringify({error: 'Not found'}));
}

// HTTP server
const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    // GET /stream (rtsp info + HTML5 embed)
    if (req.method === 'GET' && parsedUrl.pathname === '/stream') {
        sendJSON(res, {
            stream_url: `http://${SERVER_HOST}:${HTTP_PORT}${HTTP_STREAM_PATH}`,
            html5_embed: getEmbedHTML(),
            status: currentStreamActive ? 'active' : 'inactive',
        });
        return;
    }
    // POST /commands/start or /cmd/start
    if (
        req.method === 'POST' &&
        (parsedUrl.pathname === '/commands/start' || parsedUrl.pathname === '/cmd/start')
    ) {
        startStream();
        sendJSON(res, {
            message: 'Stream started',
            stream_url: `http://${SERVER_HOST}:${HTTP_PORT}${HTTP_STREAM_PATH}`,
            html5_embed: getEmbedHTML(),
            status: 'active',
        });
        return;
    }
    // POST /commands/stop or /cmd/stop
    if (
        req.method === 'POST' &&
        (parsedUrl.pathname === '/commands/stop' || parsedUrl.pathname === '/cmd/stop')
    ) {
        stopStream();
        sendJSON(res, {
            message: 'Stream stopped',
            status: 'inactive',
        });
        return;
    }
    // HTTP MJPEG stream endpoint
    if (req.method === 'GET' && parsedUrl.pathname === HTTP_STREAM_PATH) {
        if (!currentStreamActive) {
            startStream();
            // Delay a bit to let the stream warm up
            setTimeout(() => handleVideoStream(req, res), 500);
        } else {
            handleVideoStream(req, res);
        }
        return;
    }
    send404(res);
});

server.listen(HTTP_PORT, SERVER_HOST, () => {
    // Ready
});

/*
 * Helper script: rtsp2mjpeg.js
 * This script uses the fluent-ffmpeg npm library to connect to the RTSP stream
 * and output a multipart/x-mixed-replace stream (MJPEG) to stdout, without relying on
 * system ffmpeg executable.
 */
```

**You must also create this additional file in the same directory:**

```javascript
// rtsp2mjpeg.js
const ffmpeg = require('fluent-ffmpeg');

const RTSP_URL = process.env.RTSP_URL;
const HTTP_STREAM_PATH = process.env.HTTP_STREAM_PATH || '/video';

if (!RTSP_URL) {
    process.stderr.write('RTSP_URL env not set\n');
    process.exit(1);
}

// Use fluent-ffmpeg as a library, no shell command execution!
ffmpeg(RTSP_URL)
    .addInputOption('-rtsp_transport', 'tcp')
    .format('mjpeg')
    .outputOptions('-q:v', '7') // Quality
    .on('start', function (commandLine) {
        // process.stderr.write(`Spawned ffmpeg with command: ${commandLine}\n`);
    })
    .on('error', function (err) {
        process.stderr.write('FFmpeg error: ' + err.message + '\n');
        process.exit(1);
    })
    .on('end', function () {
        process.exit(0);
    })
    .pipe(process.stdout, {end: true});
```

**Install the fluent-ffmpeg npm package and ensure ffmpeg is available as a library (not as a system command):**
```sh
npm install fluent-ffmpeg
```
