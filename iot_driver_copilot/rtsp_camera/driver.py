import os
import threading
import time
from flask import Flask, Response, jsonify, request, stream_with_context, abort
import cv2

app = Flask(__name__)

RTSP_URL = os.environ.get("RTSP_URL")
CAMERA_IP = os.environ.get("CAMERA_IP")
CAMERA_USERNAME = os.environ.get("CAMERA_USERNAME")
CAMERA_PASSWORD = os.environ.get("CAMERA_PASSWORD")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))
HTTP_STREAM_PATH = os.environ.get("HTTP_STREAM_PATH", "/video")
RTSP_PORT = int(os.environ.get("RTSP_PORT", "554"))

# RTSP URL construction if not directly provided
if not RTSP_URL and CAMERA_IP:
    userinfo = ""
    if CAMERA_USERNAME and CAMERA_PASSWORD:
        userinfo = f"{CAMERA_USERNAME}:{CAMERA_PASSWORD}@"
    RTSP_URL = f"rtsp://{userinfo}{CAMERA_IP}:{RTSP_PORT}/"

class StreamState:
    def __init__(self):
        self.active = False
        self.thread = None
        self.frame = None
        self.lock = threading.Lock()
        self.capture = None
        self.last_access = 0

    def start(self, rtsp_url):
        with self.lock:
            if self.active:
                return
            self.active = True
            self.capture = cv2.VideoCapture(rtsp_url)
            self.thread = threading.Thread(target=self._update, daemon=True)
            self.thread.start()

    def stop(self):
        with self.lock:
            self.active = False
            if self.capture is not None:
                self.capture.release()
                self.capture = None
            self.frame = None

    def _update(self):
        while self.active:
            ret, frame = False, None
            if self.capture is not None:
                ret, frame = self.capture.read()
            if ret:
                with self.lock:
                    self.frame = frame
                    self.last_access = time.time()
            else:
                # Wait and retry (RTSP may be flaky)
                time.sleep(0.1)
        # Cleanup
        if self.capture is not None:
            self.capture.release()
            self.capture = None

    def get_frame(self):
        with self.lock:
            if self.frame is not None:
                ret, jpeg = cv2.imencode('.jpg', self.frame)
                if ret:
                    return jpeg.tobytes()
            return None

    def is_active(self):
        with self.lock:
            return self.active

stream_state = StreamState()

@app.route("/stream", methods=["GET"])
def get_stream_status():
    active = stream_state.is_active()
    stream_url = None
    if active:
        # The HTTP URL for the MJPEG stream
        stream_url = f"http://{request.host}{HTTP_STREAM_PATH}"
    return jsonify({
        "stream_active": active,
        "http_stream_url": stream_url,
        "rtsp_stream_url": RTSP_URL if active else None
    })

@app.route("/stream/start", methods=["POST"])
def start_stream():
    if not RTSP_URL:
        return jsonify({"error": "RTSP_URL not configured"}), 400
    stream_state.start(RTSP_URL)
    return jsonify({"result": "Stream started", "stream_active": True})

@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    stream_state.stop()
    return jsonify({"result": "Stream stopped", "stream_active": False})

@app.route(HTTP_STREAM_PATH, methods=["GET"])
def mjpeg_stream():
    if not stream_state.is_active():
        abort(404, "Stream is not active. Start it first via /stream/start.")

    def generate():
        while stream_state.is_active():
            frame = stream_state.get_frame()
            if frame is not None:
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
            else:
                time.sleep(0.05)
    return Response(stream_with_context(generate()), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)