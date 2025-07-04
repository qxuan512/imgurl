import os
import io
import threading
import time
from flask import Flask, Response, request, jsonify, abort
import cv2

# Configurations from environment variables
DEVICE_IP = os.environ.get("DEVICE_IP")
DEVICE_RTSP_PORT = int(os.environ.get("DEVICE_RTSP_PORT", "554"))
RTSP_USERNAME = os.environ.get("RTSP_USERNAME", "")
RTSP_PASSWORD = os.environ.get("RTSP_PASSWORD", "")
RTSP_PATH = os.environ.get("RTSP_PATH", "Streaming/Channels/101")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# Build RTSP URL
def build_rtsp_url():
    userinfo = ""
    if RTSP_USERNAME:
        userinfo = RTSP_USERNAME
        if RTSP_PASSWORD:
            userinfo += f":{RTSP_PASSWORD}"
        userinfo += "@"
    return f"rtsp://{userinfo}{DEVICE_IP}:{DEVICE_RTSP_PORT}/{RTSP_PATH}"

RTSP_URL = build_rtsp_url()

app = Flask(__name__)

class StreamHandler:
    def __init__(self, rtsp_url):
        self.rtsp_url = rtsp_url
        self.cap = None
        self.running = False
        self.frame = None
        self.lock = threading.Lock()
        self.thread = None

    def start(self):
        if self.running:
            return True
        self.running = True
        self.thread = threading.Thread(target=self.update, daemon=True)
        self.thread.start()
        # Wait for initial frame to be available
        t0 = time.time()
        while self.frame is None and time.time() - t0 < 5:
            time.sleep(0.1)
        return self.frame is not None

    def stop(self):
        self.running = False
        if self.cap is not None:
            self.cap.release()
            self.cap = None
        self.thread = None

    def update(self):
        self.cap = cv2.VideoCapture(self.rtsp_url)
        if not self.cap.isOpened():
            self.stop()
            return
        while self.running:
            ret, frame = self.cap.read()
            if not ret:
                time.sleep(0.2)
                continue
            with self.lock:
                self.frame = frame
        if self.cap is not None:
            self.cap.release()
            self.cap = None

    def get_jpeg_frame(self):
        with self.lock:
            frame = self.frame
        if frame is None:
            return None
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            return None
        return jpeg.tobytes()

    def generate_mjpeg(self):
        while self.running:
            jpeg = self.get_jpeg_frame()
            if jpeg is not None:
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + jpeg + b'\r\n')
            else:
                time.sleep(0.1)

stream_handler = StreamHandler(RTSP_URL)

@app.route("/snapshot", methods=["GET"])
def snapshot():
    if not stream_handler.running:
        started = stream_handler.start()
        if not started:
            abort(503, "Could not start RTSP stream")
    t0 = time.time()
    jpeg = None
    while time.time() - t0 < 3:
        jpeg = stream_handler.get_jpeg_frame()
        if jpeg is not None:
            break
        time.sleep(0.1)
    if jpeg is None:
        abort(504, "Could not retrieve snapshot from stream")
    return Response(jpeg, mimetype='image/jpeg')

@app.route("/stream/start", methods=["POST"])
def stream_start():
    success = stream_handler.start()
    if not success:
        return jsonify({"success": False, "error": "Could not start RTSP stream"}), 503
    return jsonify({"success": True, "message": "Stream started"}), 200

@app.route("/stream/stop", methods=["POST"])
def stream_stop():
    stream_handler.stop()
    return jsonify({"success": True, "message": "Stream stopped"}), 200

@app.route("/stream", methods=["GET"])
def stream():
    if not stream_handler.running:
        started = stream_handler.start()
        if not started:
            abort(503, "Could not start RTSP stream")
    return Response(stream_handler.generate_mjpeg(),
                    mimetype="multipart/x-mixed-replace; boundary=frame")

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)