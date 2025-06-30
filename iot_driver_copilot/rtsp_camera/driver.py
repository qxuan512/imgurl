```python
import os
import io
import threading
import queue
import time
from flask import Flask, Response, jsonify, request, stream_with_context, abort
import cv2

# Configuration from environment variables
RTSP_URL = os.environ.get("RTSP_URL", "")
DEVICE_IP = os.environ.get("DEVICE_IP", "")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))
HTTP_STREAM_PORT = int(os.environ.get("HTTP_STREAM_PORT", SERVER_PORT))  # Alias for clarity if needed

# Build RTSP URL if only IP is provided
if not RTSP_URL and DEVICE_IP:
    RTSP_URL = f"rtsp://{DEVICE_IP}/stream"

app = Flask(__name__)

class Streamer:
    def __init__(self, rtsp_url):
        self.rtsp_url = rtsp_url
        self.active = False
        self.thread = None
        self.frame_queue = queue.Queue(maxsize=10)
        self.lock = threading.Lock()
        self.capture = None
        self._stop_event = threading.Event()

    def start(self):
        with self.lock:
            if self.active:
                return
            self._stop_event.clear()
            self.thread = threading.Thread(target=self._capture_thread, daemon=True)
            self.active = True
            self.thread.start()

    def stop(self):
        with self.lock:
            self._stop_event.set()
            self.active = False
            if self.capture:
                try:
                    self.capture.release()
                except Exception:
                    pass
                self.capture = None
            # Drain queue
            while not self.frame_queue.empty():
                try:
                    self.frame_queue.get_nowait()
                except queue.Empty:
                    pass

    def is_active(self):
        return self.active

    def _capture_thread(self):
        try:
            self.capture = cv2.VideoCapture(self.rtsp_url)
            if not self.capture.isOpened():
                self.active = False
                return
            while not self._stop_event.is_set():
                ret, frame = self.capture.read()
                if not ret:
                    break
                ret, jpeg = cv2.imencode('.jpg', frame)
                if not ret:
                    continue
                try:
                    # Keep the queue with 1 frame to reduce latency
                    if not self.frame_queue.empty():
                        self.frame_queue.get_nowait()
                    self.frame_queue.put(jpeg.tobytes())
                except queue.Full:
                    pass
                time.sleep(0.04)  # ~25fps
        finally:
            if self.capture:
                self.capture.release()
            self.active = False

    def get_frame(self, timeout=1.0):
        try:
            return self.frame_queue.get(timeout=timeout)
        except queue.Empty:
            return None

streamer = Streamer(RTSP_URL)

@app.route('/stream', methods=['GET'])
def stream_status():
    # Returns status and RTSP url if available
    return jsonify({
        "active": streamer.is_active(),
        "rtsp_url": RTSP_URL if streamer.is_active() else None
    })

@app.route('/cmd/start', methods=['POST'])
@app.route('/stream/start', methods=['POST'])
def start_stream():
    streamer.start()
    # Wait briefly to confirm it is active
    time.sleep(0.5)
    return jsonify({
        "status": "started" if streamer.is_active() else "failed",
        "active": streamer.is_active(),
        "rtsp_url": RTSP_URL if streamer.is_active() else None
    })

@app.route('/cmd/stop', methods=['POST'])
@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    streamer.stop()
    return jsonify({
        "status": "stopped",
        "active": streamer.is_active()
    })

def gen_mjpeg_stream():
    while streamer.is_active():
        frame = streamer.get_frame(timeout=2.0)
        if frame is None:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')

@app.route('/stream/video', methods=['GET'])
def stream_video():
    if not streamer.is_active():
        abort(503, description="Stream not active. Start the stream first.")
    return Response(stream_with_context(gen_mjpeg_stream()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=HTTP_STREAM_PORT, threaded=True)
```
