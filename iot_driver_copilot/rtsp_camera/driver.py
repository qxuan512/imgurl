import os
import threading
import queue
import time
from flask import Flask, Response, request, jsonify
import cv2

# Configuration from environment variables
RTSP_CAMERA_IP = os.environ.get("RTSP_CAMERA_IP", "")
RTSP_CAMERA_PORT = os.environ.get("RTSP_CAMERA_PORT", "554")
RTSP_CAMERA_USER = os.environ.get("RTSP_CAMERA_USER", "")
RTSP_CAMERA_PASSWORD = os.environ.get("RTSP_CAMERA_PASSWORD", "")
RTSP_CAMERA_PATH = os.environ.get("RTSP_CAMERA_PATH", "stream")
RTSP_CAMERA_PARAMS = os.environ.get("RTSP_CAMERA_PARAMS", "")

SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))
HTTP_STREAM_PATH = os.environ.get("HTTP_STREAM_PATH", "/video")
HTTP_STREAM_PORT = int(os.environ.get("HTTP_STREAM_PORT", SERVER_PORT))

# RTSP URI build
def build_rtsp_url():
    cred = ""
    if RTSP_CAMERA_USER and RTSP_CAMERA_PASSWORD:
        cred = f"{RTSP_CAMERA_USER}:{RTSP_CAMERA_PASSWORD}@"
    params = f"?{RTSP_CAMERA_PARAMS}" if RTSP_CAMERA_PARAMS else ""
    return f"rtsp://{cred}{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}/{RTSP_CAMERA_PATH}{params}"

class RTSPCameraStreamer(threading.Thread):
    def __init__(self, rtsp_url):
        super().__init__()
        self.rtsp_url = rtsp_url
        self.frame_queue = queue.Queue(maxsize=2)
        self.running = threading.Event()
        self.active = False
        self.daemon = True
        self.cap = None

    def run(self):
        while self.running.is_set():
            if not self.active:
                time.sleep(0.1)
                continue
            if self.cap is None or not self.cap.isOpened():
                self.cap = cv2.VideoCapture(self.rtsp_url)
                if not self.cap.isOpened():
                    time.sleep(1)
                    continue
            ret, frame = self.cap.read()
            if not ret:
                self.cap.release()
                self.cap = None
                time.sleep(0.5)
                continue
            ret, buffer = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            try:
                if self.frame_queue.full():
                    self.frame_queue.get_nowait()
                self.frame_queue.put_nowait(buffer.tobytes())
            except queue.Full:
                pass
        if self.cap is not None:
            self.cap.release()
            self.cap = None

    def start_stream(self):
        self.active = True

    def stop_stream(self):
        self.active = False
        while not self.frame_queue.empty():
            try:
                self.frame_queue.get_nowait()
            except queue.Empty:
                break

    def get_frame(self, timeout=2):
        try:
            return self.frame_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def is_active(self):
        return self.active

# Initialize streamer
rtsp_url = build_rtsp_url()
streamer = RTSPCameraStreamer(rtsp_url)
streamer.running.set()
streamer.start()

app = Flask(__name__)

@app.route('/stream', methods=['GET'])
def stream_status():
    active = streamer.is_active()
    stream_url = None
    if active:
        stream_url = f"http://{request.host}{HTTP_STREAM_PATH}"
    return jsonify({
        "active": active,
        "stream_url": stream_url,
        "format": "multipart/x-mixed-replace; boundary=frame"
    })

@app.route('/stream/start', methods=['POST'])
def start_stream():
    streamer.start_stream()
    return jsonify({"success": True, "message": "Stream started."})

@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    streamer.stop_stream()
    return jsonify({"success": True, "message": "Stream stopped."})

def gen_mjpeg_frames():
    while streamer.is_active():
        frame = streamer.get_frame()
        if frame is None:
            time.sleep(0.02)
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
    # Stream ends
    yield b''

@app.route(HTTP_STREAM_PATH, methods=['GET'])
def video_feed():
    if not streamer.is_active():
        return jsonify({"error": "Stream is not active."}), 404
    return Response(gen_mjpeg_frames(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)