import os
import threading
import io
import time
import json
from flask import Flask, Response, request, jsonify, stream_with_context, abort
import cv2

# Load configuration from environment variables
RTSP_CAMERA_IP = os.environ.get("RTSP_CAMERA_IP", "127.0.0.1")
RTSP_CAMERA_PORT = os.environ.get("RTSP_CAMERA_PORT", "554")
RTSP_CAMERA_USERNAME = os.environ.get("RTSP_CAMERA_USERNAME", "")
RTSP_CAMERA_PASSWORD = os.environ.get("RTSP_CAMERA_PASSWORD", "")
RTSP_CAMERA_PATH = os.environ.get("RTSP_CAMERA_PATH", "Streaming/Channels/101")
HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))

# Compose RTSP URL
def build_rtsp_url():
    creds = ""
    if RTSP_CAMERA_USERNAME and RTSP_CAMERA_PASSWORD:
        creds = f"{RTSP_CAMERA_USERNAME}:{RTSP_CAMERA_PASSWORD}@"
    return f"rtsp://{creds}{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}/{RTSP_CAMERA_PATH}"

RTSP_URL = build_rtsp_url()

app = Flask(__name__)

class CameraStreamer:
    def __init__(self, rtsp_url):
        self.rtsp_url = rtsp_url
        self.cap = None
        self.frame = None
        self.active = False
        self.thread = None
        self.lock = threading.Lock()
        self.stopped = threading.Event()

    def start(self):
        with self.lock:
            if not self.active:
                self.stopped.clear()
                self.cap = cv2.VideoCapture(self.rtsp_url)
                if not self.cap.isOpened():
                    self.cap.release()
                    self.cap = None
                    return False
                self.active = True
                self.thread = threading.Thread(target=self.update, daemon=True)
                self.thread.start()
        return True

    def stop(self):
        with self.lock:
            self.active = False
            self.stopped.set()
            if self.cap:
                self.cap.release()
                self.cap = None

    def update(self):
        while self.active and self.cap and self.cap.isOpened():
            ret, frame = self.cap.read()
            if ret:
                self.frame = frame
            else:
                time.sleep(0.05)
            if self.stopped.is_set():
                break

    def get_jpeg_frame(self):
        with self.lock:
            if self.frame is not None:
                ret, jpeg = cv2.imencode('.jpg', self.frame)
                if ret:
                    return jpeg.tobytes()
            return None

    def is_active(self):
        with self.lock:
            return self.active and self.cap is not None and self.cap.isOpened()

streamer = CameraStreamer(RTSP_URL)

@app.route('/stream', methods=['GET'])
def get_stream_status():
    status = {
        "active": streamer.is_active(),
        "stream_http_url": None
    }
    if streamer.is_active():
        host = request.host.split(':')[0]
        port = HTTP_SERVER_PORT
        status["stream_http_url"] = f"http://{host}:{port}/stream/video"
    return jsonify(status)

@app.route('/stream/start', methods=['POST'])
def start_stream():
    started = streamer.start()
    if not started:
        return jsonify({"active": False, "error": "Failed to open RTSP stream"}), 500
    return jsonify({"active": True, "message": "Stream started"})

@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    streamer.stop()
    return jsonify({"active": False, "message": "Stream stopped"})

def mjpeg_streamer():
    while streamer.is_active():
        frame = streamer.get_jpeg_frame()
        if frame is not None:
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        else:
            time.sleep(0.05)

@app.route('/stream/video', methods=['GET'])
def stream_video():
    if not streamer.is_active():
        abort(404, description="Stream is not active. Start it first.")
    return Response(stream_with_context(mjpeg_streamer()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)