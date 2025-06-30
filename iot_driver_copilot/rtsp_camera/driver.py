import os
import threading
import queue
import time
from flask import Flask, Response, jsonify, request

import cv2

# === Configuration from Environment Variables ===
RTSP_URL = os.environ.get("RTSP_URL")
RTSP_USERNAME = os.environ.get("RTSP_USERNAME")
RTSP_PASSWORD = os.environ.get("RTSP_PASSWORD")
RTSP_IP = os.environ.get("RTSP_IP")
RTSP_PORT = os.environ.get("RTSP_PORT", "554")
RTSP_PATH = os.environ.get("RTSP_PATH", "Streaming/Channels/101")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# Compose RTSP url if not fully specified
def build_rtsp_url():
    if RTSP_URL:
        return RTSP_URL
    userinfo = ""
    if RTSP_USERNAME and RTSP_PASSWORD:
        userinfo = f"{RTSP_USERNAME}:{RTSP_PASSWORD}@"
    elif RTSP_USERNAME:
        userinfo = f"{RTSP_USERNAME}@"
    return f"rtsp://{userinfo}{RTSP_IP}:{RTSP_PORT}/{RTSP_PATH}"

# === Stream Management ===
class RTSPStreamManager:
    def __init__(self):
        self.rtsp_url = build_rtsp_url()
        self.active = False
        self.capture = None
        self.frame_queue = queue.Queue(maxsize=10)
        self.thread = None
        self.lock = threading.Lock()
        self.stop_event = threading.Event()

    def start_stream(self):
        with self.lock:
            if self.active:
                return True
            self.stop_event.clear()
            self.capture = cv2.VideoCapture(self.rtsp_url)
            if not self.capture.isOpened():
                self.capture.release()
                self.capture = None
                return False
            self.active = True
            self.thread = threading.Thread(target=self._reader_thread, daemon=True)
            self.thread.start()
            return True

    def _reader_thread(self):
        while self.active and not self.stop_event.is_set():
            ret, frame = self.capture.read()
            if not ret:
                time.sleep(0.1)
                continue
            ret, jpeg = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            try:
                if self.frame_queue.full():
                    self.frame_queue.get_nowait()
                self.frame_queue.put_nowait(jpeg.tobytes())
            except queue.Full:
                pass
        if self.capture:
            self.capture.release()
        self.capture = None

    def stop_stream(self):
        with self.lock:
            if not self.active:
                return
            self.active = False
            self.stop_event.set()
            if self.thread and self.thread.is_alive():
                self.thread.join(timeout=2)
            self.frame_queue.queue.clear()

    def get_frame(self, timeout=2):
        try:
            return self.frame_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def is_active(self):
        return self.active

    def get_rtsp_url(self):
        return self.rtsp_url

# === Flask App ===
app = Flask(__name__)
stream_manager = RTSPStreamManager()

# --- API Endpoints ---

@app.route('/stream', methods=['GET'])
def stream_status():
    return jsonify({
        "active": stream_manager.is_active(),
        "rtsp_url": stream_manager.get_rtsp_url() if stream_manager.is_active() else None
    })

@app.route('/cmd/start', methods=['POST'])
@app.route('/stream/start', methods=['POST'])
def start_stream():
    success = stream_manager.start_stream()
    return jsonify({
        "success": success,
        "rtsp_url": stream_manager.get_rtsp_url() if success else None,
        "active": stream_manager.is_active()
    }), 200 if success else 500

@app.route('/cmd/stop', methods=['POST'])
@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    stream_manager.stop_stream()
    return jsonify({
        "success": True,
        "active": stream_manager.is_active()
    })

@app.route('/video', methods=['GET'])
def video_feed():
    if not stream_manager.is_active():
        started = stream_manager.start_stream()
        if not started:
            return jsonify({"error": "Cannot start RTSP stream"}), 500

    def generate():
        while stream_manager.is_active():
            frame = stream_manager.get_frame(timeout=5)
            if frame is None:
                continue
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
    return Response(generate(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

# === Main Entrypoint ===
if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)