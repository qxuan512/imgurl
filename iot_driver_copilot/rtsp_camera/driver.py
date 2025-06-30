import os
import threading
import time
from flask import Flask, Response, jsonify, request, stream_with_context, abort
import cv2

app = Flask(__name__)

# Config from environment variables
DEVICE_IP = os.environ.get("DEVICE_IP", "127.0.0.1")
RTSP_PORT = int(os.environ.get("RTSP_PORT", "554"))
RTSP_USERNAME = os.environ.get("RTSP_USERNAME", "")
RTSP_PASSWORD = os.environ.get("RTSP_PASSWORD", "")
RTSP_PATH = os.environ.get("RTSP_PATH", "/stream")
HTTP_HOST = os.environ.get("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8000"))

# Construct RTSP URL
if RTSP_USERNAME and RTSP_PASSWORD:
    RTSP_URL = f"rtsp://{RTSP_USERNAME}:{RTSP_PASSWORD}@{DEVICE_IP}:{RTSP_PORT}{RTSP_PATH}"
else:
    RTSP_URL = f"rtsp://{DEVICE_IP}:{RTSP_PORT}{RTSP_PATH}"

class StreamState:
    def __init__(self):
        self.active = False
        self.capture = None
        self.lock = threading.Lock()
        self.last_frame = None
        self.reader_thread = None
        self.stop_event = threading.Event()
    
    def start(self):
        with self.lock:
            if not self.active:
                self.stop_event.clear()
                self.capture = cv2.VideoCapture(RTSP_URL)
                if not self.capture.isOpened():
                    self.capture.release()
                    self.capture = None
                    return False
                self.active = True
                self.reader_thread = threading.Thread(target=self._reader, daemon=True)
                self.reader_thread.start()
                return True
            return False

    def _reader(self):
        while self.active and not self.stop_event.is_set():
            if self.capture is None:
                break
            ret, frame = self.capture.read()
            if not ret:
                time.sleep(0.05)
                continue
            self.last_frame = frame
        self._release()

    def stop(self):
        with self.lock:
            if self.active:
                self.active = False
                self.stop_event.set()
                if self.reader_thread:
                    self.reader_thread.join(timeout=2)
                self._release()
                return True
            return False

    def _release(self):
        if self.capture is not None:
            self.capture.release()
            self.capture = None

    def get_frame(self):
        if self.last_frame is not None:
            ret, jpeg = cv2.imencode('.jpg', self.last_frame)
            if ret:
                return jpeg.tobytes()
        return None

    def is_active(self):
        return self.active

stream_state = StreamState()

@app.route('/stream', methods=['GET'])
def stream_status():
    return jsonify({
        "active": stream_state.is_active(),
        "rtsp_url": RTSP_URL if stream_state.is_active() else None
    })

@app.route('/stream', methods=['GET'])
def stream_status_duplicate():
    return stream_status()

@app.route('/cmd/start', methods=['POST'])
@app.route('/stream/start', methods=['POST'])
def start_stream():
    started = stream_state.start()
    if started or stream_state.is_active():
        return jsonify({
            "status": "started",
            "active": True,
            "rtsp_url": RTSP_URL
        }), 200
    else:
        return jsonify({
            "status": "error",
            "message": "Failed to start stream"
        }), 500

@app.route('/cmd/stop', methods=['POST'])
@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    stopped = stream_state.stop()
    return jsonify({
        "status": "stopped" if stopped else "already_stopped",
        "active": stream_state.is_active()
    }), 200

def gen_mjpeg():
    while stream_state.is_active():
        frame = stream_state.get_frame()
        if frame is None:
            time.sleep(0.05)
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
    # End of stream
    yield (b'--frame\r\n'
           b'Content-Type: text/plain\r\n\r\nStream stopped\r\n')

@app.route('/video', methods=['GET'])
def video_feed():
    if not stream_state.is_active():
        abort(503, "Stream is not active. Use /cmd/start or /stream/start first.")
    return Response(stream_with_context(gen_mjpeg()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)