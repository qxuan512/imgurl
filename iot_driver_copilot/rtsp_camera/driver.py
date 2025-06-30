import os
import threading
import time
from flask import Flask, Response, jsonify, request, stream_with_context, abort
import cv2

# Environment Variables
RTSP_URL = os.environ.get("RTSP_URL", "")
RTSP_USERNAME = os.environ.get("RTSP_USERNAME", "")
RTSP_PASSWORD = os.environ.get("RTSP_PASSWORD", "")
RTSP_IP = os.environ.get("RTSP_IP", "")
RTSP_PORT = os.environ.get("RTSP_PORT", "554")
RTSP_PATH = os.environ.get("RTSP_PATH", "")
HTTP_HOST = os.environ.get("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8080"))

# Build RTSP URL if not directly provided
if RTSP_URL == "":
    auth = f"{RTSP_USERNAME}:{RTSP_PASSWORD}@" if RTSP_USERNAME and RTSP_PASSWORD else ""
    RTSP_URL = f"rtsp://{auth}{RTSP_IP}:{RTSP_PORT}/{RTSP_PATH}".rstrip('/')

app = Flask(__name__)

class StreamState:
    def __init__(self):
        self.active = False
        self.capture = None
        self.lock = threading.Lock()
        self.last_frame = None
        self.last_frame_time = 0
        self.thread = None

    def start(self):
        with self.lock:
            if self.active:
                return True
            self.capture = cv2.VideoCapture(RTSP_URL)
            if not self.capture.isOpened():
                self.capture.release()
                self.capture = None
                self.active = False
                return False
            self.active = True
            self.thread = threading.Thread(target=self._reader, daemon=True)
            self.thread.start()
            return True

    def _reader(self):
        while self.active and self.capture:
            ret, frame = self.capture.read()
            if not ret:
                time.sleep(0.1)
                continue
            with self.lock:
                self.last_frame = frame
                self.last_frame_time = time.time()
        if self.capture:
            self.capture.release()
            self.capture = None

    def stop(self):
        with self.lock:
            self.active = False
            if self.capture:
                self.capture.release()
                self.capture = None
            self.last_frame = None
            self.thread = None

    def get_frame(self):
        with self.lock:
            return self.last_frame

    def is_active(self):
        with self.lock:
            return self.active

stream_state = StreamState()

def gen_mjpeg():
    while stream_state.is_active():
        frame = stream_state.get_frame()
        if frame is None:
            time.sleep(0.01)
            continue
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + jpeg.tobytes() + b'\r\n')
        time.sleep(0.03)  # ~30fps

@app.route('/stream', methods=['GET'])
def stream_status():
    status = {
        "active": stream_state.is_active(),
        "rtsp_url": RTSP_URL if stream_state.is_active() else None
    }
    return jsonify(status)

@app.route('/stream/start', methods=['POST'])
@app.route('/cmd/start', methods=['POST'])
def start_stream():
    success = stream_state.start()
    status = {
        "success": success,
        "active": stream_state.is_active(),
        "rtsp_url": RTSP_URL if stream_state.is_active() else None
    }
    return jsonify(status), (200 if success else 500)

@app.route('/stream/stop', methods=['POST'])
@app.route('/cmd/stop', methods=['POST'])
def stop_stream():
    stream_state.stop()
    status = {
        "success": True,
        "active": False
    }
    return jsonify(status)

@app.route('/video', methods=['GET'])
def video_feed():
    if not stream_state.is_active():
        abort(503, description="Stream is not active. Start the stream first.")
    return Response(stream_with_context(gen_mjpeg()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)