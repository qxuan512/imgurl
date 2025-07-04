import os
import cv2
import threading
import io
import time
from flask import Flask, Response, send_file, jsonify, request, abort

# Configuration from environment variables
DEVICE_IP = os.getenv('DEVICE_IP', '127.0.0.1')
RTSP_PORT = int(os.getenv('RTSP_PORT', 554))
RTSP_USER = os.getenv('RTSP_USER', '')
RTSP_PASSWORD = os.getenv('RTSP_PASSWORD', '')
RTSP_PATH = os.getenv('RTSP_PATH', 'stream1')
SERVER_HOST = os.getenv('SERVER_HOST', '0.0.0.0')
SERVER_PORT = int(os.getenv('SERVER_PORT', 8080))

# Compose RTSP URL with optional authentication
if RTSP_USER and RTSP_PASSWORD:
    RTSP_URL = f"rtsp://{RTSP_USER}:{RTSP_PASSWORD}@{DEVICE_IP}:{RTSP_PORT}/{RTSP_PATH}"
else:
    RTSP_URL = f"rtsp://{DEVICE_IP}:{RTSP_PORT}/{RTSP_PATH}"

app = Flask(__name__)

class RTSPCameraStreamer:
    def __init__(self, rtsp_url):
        self.rtsp_url = rtsp_url
        self.cap = None
        self.running = False
        self.lock = threading.Lock()
        self.latest_frame = None
        self.thread = None

    def start(self):
        with self.lock:
            if self.running:
                return True
            self.cap = cv2.VideoCapture(self.rtsp_url)
            if not self.cap.isOpened():
                self.cap.release()
                self.cap = None
                return False
            self.running = True
            self.thread = threading.Thread(target=self._update_frames, daemon=True)
            self.thread.start()
            return True

    def stop(self):
        with self.lock:
            self.running = False
            if self.cap is not None:
                self.cap.release()
                self.cap = None

    def _update_frames(self):
        while self.running:
            if self.cap is not None:
                ret, frame = self.cap.read()
                if ret:
                    self.latest_frame = frame
                else:
                    # Try to reconnect
                    self.cap.release()
                    time.sleep(1)
                    self.cap = cv2.VideoCapture(self.rtsp_url)
            else:
                time.sleep(0.5)

    def get_latest_frame(self):
        with self.lock:
            if self.latest_frame is not None:
                return self.latest_frame.copy()
            else:
                return None

    def is_running(self):
        with self.lock:
            return self.running

streamer = RTSPCameraStreamer(RTSP_URL)

@app.route('/snapshot', methods=['GET'])
def snapshot():
    if not streamer.is_running():
        started = streamer.start()
        if not started:
            abort(503, description="Unable to connect to the RTSP stream.")

    # Wait for at least one frame
    timeout = 5
    for _ in range(timeout * 10):
        frame = streamer.get_latest_frame()
        if frame is not None:
            break
        time.sleep(0.1)
    else:
        abort(504, description="Snapshot timeout: Unable to retrieve frame.")

    # Encode frame as JPEG
    ret, jpeg = cv2.imencode('.jpg', frame)
    if not ret:
        abort(500, description="Failed to encode JPEG.")

    return Response(jpeg.tobytes(), mimetype='image/jpeg')

@app.route('/stream/start', methods=['POST'])
def stream_start():
    if streamer.is_running():
        return jsonify({"status": "already streaming"}), 200
    started = streamer.start()
    if not started:
        return jsonify({"status": "error", "message": "Unable to start stream"}), 503
    return jsonify({"status": "started"}), 200

@app.route('/stream/stop', methods=['POST'])
def stream_stop():
    if not streamer.is_running():
        return jsonify({"status": "already stopped"}), 200
    streamer.stop()
    return jsonify({"status": "stopped"}), 200

def mjpeg_generator():
    while streamer.is_running():
        frame = streamer.get_latest_frame()
        if frame is not None:
            ret, jpeg = cv2.imencode('.jpg', frame)
            if ret:
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + jpeg.tobytes() + b'\r\n')
        time.sleep(0.05)

@app.route('/stream.mjpg', methods=['GET'])
def stream_mjpeg():
    if not streamer.is_running():
        started = streamer.start()
        if not started:
            abort(503, description="Unable to start RTSP stream.")
    return Response(mjpeg_generator(), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)