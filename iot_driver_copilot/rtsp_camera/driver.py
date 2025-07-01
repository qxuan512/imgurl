import os
import threading
import queue
import time
from flask import Flask, Response, jsonify, request
import cv2

# Configuration from environment variables
RTSP_CAMERA_HOST = os.environ.get("RTSP_CAMERA_HOST", "127.0.0.1")
RTSP_CAMERA_PORT = os.environ.get("RTSP_CAMERA_PORT", "554")
RTSP_CAMERA_PATH = os.environ.get("RTSP_CAMERA_PATH", "")
RTSP_CAMERA_USERNAME = os.environ.get("RTSP_CAMERA_USERNAME", "")
RTSP_CAMERA_PASSWORD = os.environ.get("RTSP_CAMERA_PASSWORD", "")
HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))

# Build RTSP URL
if RTSP_CAMERA_USERNAME and RTSP_CAMERA_PASSWORD:
    RTSP_URL = f"rtsp://{RTSP_CAMERA_USERNAME}:{RTSP_CAMERA_PASSWORD}@{RTSP_CAMERA_HOST}:{RTSP_CAMERA_PORT}/{RTSP_CAMERA_PATH}"
else:
    RTSP_URL = f"rtsp://{RTSP_CAMERA_HOST}:{RTSP_CAMERA_PORT}/{RTSP_CAMERA_PATH}"

app = Flask(__name__)

class StreamState:
    def __init__(self):
        self.active = False
        self.frame_queue = queue.Queue(maxsize=10)
        self.capture_thread = None
        self.stop_event = threading.Event()
        self.lock = threading.Lock()

    def start(self):
        with self.lock:
            if not self.active:
                self.stop_event.clear()
                self.capture_thread = threading.Thread(target=self._capture_frames, daemon=True)
                self.active = True
                self.capture_thread.start()

    def stop(self):
        with self.lock:
            self.active = False
            self.stop_event.set()
            # Drain the queue
            while not self.frame_queue.empty():
                try:
                    self.frame_queue.get_nowait()
                except Exception:
                    break

    def _capture_frames(self):
        cap = cv2.VideoCapture(RTSP_URL)
        if not cap.isOpened():
            self.active = False
            return
        while not self.stop_event.is_set():
            ret, frame = cap.read()
            if not ret:
                # Try to reconnect after a short delay
                cap.release()
                time.sleep(2)
                cap = cv2.VideoCapture(RTSP_URL)
                continue
            # Encode frame as JPEG
            ret, buffer = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            try:
                self.frame_queue.put(buffer.tobytes(), timeout=1)
            except queue.Full:
                pass  # Drop frame if queue is full
        cap.release()

    def get_frame(self):
        try:
            return self.frame_queue.get(timeout=2)
        except queue.Empty:
            return None

stream_state = StreamState()

@app.route("/stream", methods=["GET"])
def get_stream_status():
    status = {
        "active": stream_state.active,
        "stream_url": f"http://{HTTP_SERVER_HOST}:{HTTP_SERVER_PORT}/stream/live" if stream_state.active else None
    }
    return jsonify(status), 200

@app.route("/stream/start", methods=["POST"])
def start_stream():
    stream_state.start()
    return jsonify({"result": "Stream started", "active": True, "stream_url": f"http://{HTTP_SERVER_HOST}:{HTTP_SERVER_PORT}/stream/live"}), 200

@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    stream_state.stop()
    return jsonify({"result": "Stream stopped", "active": False}), 200

def mjpeg_stream():
    while stream_state.active:
        frame = stream_state.get_frame()
        if frame is None:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
    # When not active, the stream ends

@app.route("/stream/live")
def stream_live():
    # Only allow streaming if started
    if not stream_state.active:
        return jsonify({"error": "Stream is not active"}), 400
    return Response(mjpeg_stream(), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)