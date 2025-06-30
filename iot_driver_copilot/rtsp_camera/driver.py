import os
import threading
import queue
import time
import flask
from flask import Flask, Response, jsonify, request

import cv2

app = Flask(__name__)

# Configuration via environment variables
RTSP_HOST = os.environ.get('RTSP_HOST', '127.0.0.1')
RTSP_PORT = os.environ.get('RTSP_PORT', '554')
RTSP_PATH = os.environ.get('RTSP_PATH', '')  # e.g. '/h264/ch1/main/av_stream'
RTSP_USER = os.environ.get('RTSP_USER', '')
RTSP_PASS = os.environ.get('RTSP_PASS', '')
RTSP_STREAM_PARAMS = os.environ.get('RTSP_STREAM_PARAMS', '')  # e.g. '?param=value'
HTTP_HOST = os.environ.get('HTTP_HOST', '0.0.0.0')
HTTP_PORT = int(os.environ.get('HTTP_PORT', '8080'))

# Compose RTSP URL (credentials if present)
def build_rtsp_url():
    if RTSP_USER and RTSP_PASS:
        return f'rtsp://{RTSP_USER}:{RTSP_PASS}@{RTSP_HOST}:{RTSP_PORT}{RTSP_PATH}{RTSP_STREAM_PARAMS}'
    else:
        return f'rtsp://{RTSP_HOST}:{RTSP_PORT}{RTSP_PATH}{RTSP_STREAM_PARAMS}'

RTSP_URL = build_rtsp_url()

# Stream Manager
class StreamManager:
    def __init__(self):
        self.active = False
        self.thread = None
        self.frame_queue = queue.Queue(maxsize=20)
        self.stop_event = threading.Event()
        self.lock = threading.Lock()

    def start_stream(self):
        with self.lock:
            if self.active:
                return
            self.stop_event.clear()
            self.thread = threading.Thread(target=self._capture_frames, daemon=True)
            self.thread.start()
            self.active = True

    def stop_stream(self):
        with self.lock:
            if not self.active:
                return
            self.stop_event.set()
            if self.thread is not None:
                self.thread.join(timeout=2)
            # Clear any remaining frames
            while not self.frame_queue.empty():
                try:
                    self.frame_queue.get_nowait()
                except Exception:
                    break
            self.active = False

    def _capture_frames(self):
        cap = cv2.VideoCapture(RTSP_URL)
        if not cap.isOpened():
            self.active = False
            return
        while not self.stop_event.is_set():
            ret, frame = cap.read()
            if not ret:
                time.sleep(0.1)
                continue
            # Encode frame as JPEG
            ret, jpeg = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            try:
                self.frame_queue.put(jpeg.tobytes(), timeout=0.2)
            except queue.Full:
                pass
        cap.release()

    def get_frame(self, timeout=1.0):
        try:
            return self.frame_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def is_active(self):
        return self.active

stream_manager = StreamManager()

# API: Start Stream
@app.route('/cmd/start', methods=['POST'])
@app.route('/stream/start', methods=['POST'])
def start_stream():
    stream_manager.start_stream()
    return jsonify({
        'success': True,
        'message': 'Stream started.',
        'rtsp_active': stream_manager.is_active(),
        'rtsp_url': RTSP_URL if stream_manager.is_active() else None
    })

# API: Stop Stream
@app.route('/cmd/stop', methods=['POST'])
@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    stream_manager.stop_stream()
    return jsonify({
        'success': True,
        'message': 'Stream stopped.',
        'rtsp_active': stream_manager.is_active(),
        'rtsp_url': RTSP_URL if stream_manager.is_active() else None
    })

# API: Stream Status
@app.route('/stream', methods=['GET'])
def stream_status():
    return jsonify({
        'rtsp_active': stream_manager.is_active(),
        'rtsp_url': RTSP_URL if stream_manager.is_active() else None
    })

# MJPEG Streaming Endpoint
def mjpeg_generator():
    while stream_manager.is_active():
        frame = stream_manager.get_frame(timeout=1.0)
        if frame is None:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
    # Stream ends when inactive

@app.route('/video', methods=['GET'])
def video_feed():
    if not stream_manager.is_active():
        return jsonify({'error': 'Stream not active. Start stream first.'}), 400
    return Response(mjpeg_generator(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)