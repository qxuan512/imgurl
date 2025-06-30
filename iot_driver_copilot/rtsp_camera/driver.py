import os
import threading
import time
from flask import Flask, Response, jsonify, request, stream_with_context
import cv2

app = Flask(__name__)

# Configuration from environment variables
RTSP_CAMERA_IP = os.environ.get('RTSP_CAMERA_IP', '127.0.0.1')
RTSP_CAMERA_PORT = os.environ.get('RTSP_CAMERA_PORT', '554')
RTSP_CAMERA_USERNAME = os.environ.get('RTSP_CAMERA_USERNAME', '')
RTSP_CAMERA_PASSWORD = os.environ.get('RTSP_CAMERA_PASSWORD', '')
RTSP_STREAM_PATH = os.environ.get('RTSP_STREAM_PATH', 'stream')
HTTP_SERVER_HOST = os.environ.get('HTTP_SERVER_HOST', '0.0.0.0')
HTTP_SERVER_PORT = int(os.environ.get('HTTP_SERVER_PORT', '8080'))

# Build RTSP URL
def build_rtsp_url():
    if RTSP_CAMERA_USERNAME and RTSP_CAMERA_PASSWORD:
        return f'rtsp://{RTSP_CAMERA_USERNAME}:{RTSP_CAMERA_PASSWORD}@{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}/{RTSP_STREAM_PATH}'
    else:
        return f'rtsp://{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}/{RTSP_STREAM_PATH}'

RTSP_URL = build_rtsp_url()

class RTSPStreamHandler:
    def __init__(self):
        self.active = False
        self.thread = None
        self.capture = None
        self.lock = threading.Lock()
        self.latest_frame = None
        self.stop_event = threading.Event()
        self.clients = 0

    def start_stream(self):
        with self.lock:
            if self.active:
                return True
            self.stop_event.clear()
            self.capture = cv2.VideoCapture(RTSP_URL)
            if not self.capture.isOpened():
                self.capture.release()
                self.capture = None
                return False
            self.active = True
            self.thread = threading.Thread(target=self._update_frames, daemon=True)
            self.thread.start()
            return True

    def _update_frames(self):
        while not self.stop_event.is_set():
            ret, frame = self.capture.read()
            if not ret:
                break
            _, jpeg = cv2.imencode('.jpg', frame)
            self.latest_frame = jpeg.tobytes()
            time.sleep(0.01)
        self.active = False
        if self.capture is not None:
            self.capture.release()
            self.capture = None
        self.latest_frame = None

    def stop_stream(self):
        with self.lock:
            if not self.active:
                return
            self.stop_event.set()
            if self.thread and self.thread.is_alive():
                self.thread.join(timeout=2)
            self.active = False
            self.latest_frame = None

    def get_status(self):
        return {'active': self.active, 'rtsp_url': RTSP_URL if self.active else None}

    def get_frame(self):
        return self.latest_frame

rtsp_handler = RTSPStreamHandler()

@app.route('/stream', methods=['GET'])
def get_stream_status():
    return jsonify(rtsp_handler.get_status())

@app.route('/cmd/start', methods=['POST'])
@app.route('/stream/start', methods=['POST'])
def start_stream():
    success = rtsp_handler.start_stream()
    return jsonify({
        'success': success,
        'active': rtsp_handler.active,
        'rtsp_url': RTSP_URL if rtsp_handler.active else None
    })

@app.route('/cmd/stop', methods=['POST'])
@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    rtsp_handler.stop_stream()
    return jsonify({
        'success': True,
        'active': rtsp_handler.active,
        'rtsp_url': RTSP_URL if rtsp_handler.active else None
    })

def gen_mjpeg():
    while True:
        frame = rtsp_handler.get_frame()
        if frame is None:
            time.sleep(0.05)
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        time.sleep(0.04)  # ~25fps

@app.route('/stream/video', methods=['GET'])
def video_stream():
    if not rtsp_handler.active:
        started = rtsp_handler.start_stream()
        if not started:
            return jsonify({'error': 'Failed to start RTSP stream'}), 500
    return Response(stream_with_context(gen_mjpeg()), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)