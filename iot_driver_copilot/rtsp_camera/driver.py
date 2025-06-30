import os
import threading
import time
from flask import Flask, Response, jsonify, request, stream_with_context, abort
import cv2

# Configuration from environment variables
RTSP_URL = os.environ.get("RTSP_URL", "")
DEVICE_IP = os.environ.get("DEVICE_IP", "")
RTSP_PORT = int(os.environ.get("RTSP_PORT", "554"))
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

if not RTSP_URL:
    # Construct RTSP URL from device IP and port if not directly given
    username = os.environ.get("RTSP_USERNAME", "")
    password = os.environ.get("RTSP_PASSWORD", "")
    path = os.environ.get("RTSP_PATH", "Streaming/Channels/101")
    if username and password:
        RTSP_URL = f"rtsp://{username}:{password}@{DEVICE_IP}:{RTSP_PORT}/{path}"
    else:
        RTSP_URL = f"rtsp://{DEVICE_IP}:{RTSP_PORT}/{path}"

app = Flask(__name__)

class CameraStream:
    def __init__(self):
        self.active = False
        self.capture = None
        self.lock = threading.Lock()
        self.last_frame = None
        self.thread = None
        self.rtsp_url = RTSP_URL
        self.last_access = 0

    def start(self):
        with self.lock:
            if self.active:
                return True
            self.capture = cv2.VideoCapture(self.rtsp_url)
            if not self.capture.isOpened():
                self.capture = None
                self.active = False
                return False
            self.active = True
            self.thread = threading.Thread(target=self._update, daemon=True)
            self.thread.start()
            return True

    def stop(self):
        with self.lock:
            self.active = False
            if self.capture:
                self.capture.release()
            self.capture = None
            self.last_frame = None
            self.thread = None
            return True

    def _update(self):
        while self.active and self.capture:
            ret, frame = self.capture.read()
            if not ret:
                time.sleep(0.05)
                continue
            self.last_frame = frame
            self.last_access = time.time()
            # Auto-stop after a period of inactivity
            if time.time() - self.last_access > 60:
                self.stop()
                break

    def get_frame(self):
        self.last_access = time.time()
        if self.last_frame is not None:
            ret, jpeg = cv2.imencode('.jpg', self.last_frame)
            if ret:
                return jpeg.tobytes()
        return None

    def is_active(self):
        with self.lock:
            return self.active and self.capture is not None and self.capture.isOpened()

    def get_rtsp_url(self):
        return self.rtsp_url

camera = CameraStream()

@app.route('/stream', methods=['GET'])
def stream_status():
    status = {
        "stream_active": camera.is_active(),
        "rtsp_url": camera.get_rtsp_url() if camera.is_active() else None
    }
    return jsonify(status)

@app.route('/cmd/start', methods=['POST'])
@app.route('/stream/start', methods=['POST'])
def start_stream():
    if camera.start():
        return jsonify({
            "status": "stream started",
            "rtsp_url": camera.get_rtsp_url(),
            "stream_active": True
        })
    else:
        return jsonify({
            "status": "failed to start stream",
            "stream_active": False
        }), 500

@app.route('/cmd/stop', methods=['POST'])
@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    camera.stop()
    return jsonify({
        "status": "stream stopped",
        "stream_active": False
    })

def generate_mjpeg():
    while camera.is_active():
        frame = camera.get_frame()
        if frame is not None:
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        else:
            time.sleep(0.05)

@app.route('/video', methods=['GET'])
def video_feed():
    if not camera.is_active():
        if not camera.start():
            abort(503, "Unable to start stream")
    return Response(stream_with_context(generate_mjpeg()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)