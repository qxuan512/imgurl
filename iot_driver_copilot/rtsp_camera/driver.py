import os
import threading
import io
import time
from typing import Optional
from flask import Flask, Response, jsonify, request, stream_with_context, abort
import cv2

app = Flask(__name__)

# Configuration from environment variables
RTSP_HOST = os.environ.get('RTSP_HOST', '127.0.0.1')
RTSP_PORT = int(os.environ.get('RTSP_PORT', '554'))
RTSP_PATH = os.environ.get('RTSP_PATH', '/stream')
RTSP_USERNAME = os.environ.get('RTSP_USERNAME', '')
RTSP_PASSWORD = os.environ.get('RTSP_PASSWORD', '')
RTSP_TRANSPORT = os.environ.get('RTSP_TRANSPORT', '')  # e.g., 'tcp' or 'udp'
HTTP_HOST = os.environ.get('HTTP_HOST', '0.0.0.0')
HTTP_PORT = int(os.environ.get('HTTP_PORT', '8080'))

def build_rtsp_url():
    auth_part = ""
    if RTSP_USERNAME and RTSP_PASSWORD:
        auth_part = f"{RTSP_USERNAME}:{RTSP_PASSWORD}@"
    url = f"rtsp://{auth_part}{RTSP_HOST}:{RTSP_PORT}{RTSP_PATH}"
    if RTSP_TRANSPORT.lower() in ('tcp', 'udp'):
        url += f"?rtsp_transport={RTSP_TRANSPORT.lower()}"
    return url

class RTSPStreamProxy:
    def __init__(self):
        self.rtsp_url = build_rtsp_url()
        self.active = False
        self.thread = None
        self.frame = None
        self.frame_lock = threading.Lock()
        self.stop_event = threading.Event()
        self.capture: Optional[cv2.VideoCapture] = None

    def start(self):
        if self.active:
            return True
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._capture_loop, daemon=True)
        self.thread.start()
        for _ in range(50):
            if self.active:
                return True
            time.sleep(0.1)
        return False

    def stop(self):
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join(timeout=2)
        if self.capture is not None:
            self.capture.release()
            self.capture = None
        self.active = False
        self.frame = None

    def _capture_loop(self):
        self.capture = cv2.VideoCapture(self.rtsp_url)
        if not self.capture.isOpened():
            self.active = False
            return
        self.active = True
        while not self.stop_event.is_set():
            ret, frame = self.capture.read()
            if not ret:
                self.active = False
                break
            with self.frame_lock:
                self.frame = frame
            time.sleep(0.01)
        self.active = False
        if self.capture is not None:
            self.capture.release()
            self.capture = None

    def get_jpeg(self):
        with self.frame_lock:
            frame = self.frame.copy() if self.frame is not None else None
        if frame is None:
            return None
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            return None
        return jpeg.tobytes()

    def get_status(self):
        return {
            'active': self.active,
            'rtsp_url': self.rtsp_url if self.active else None
        }

rtsp_proxy = RTSPStreamProxy()

@app.route('/stream', methods=['GET'])
def get_stream_status():
    return jsonify(rtsp_proxy.get_status())

@app.route('/cmd/start', methods=['POST'])
@app.route('/stream/start', methods=['POST'])
def start_stream():
    started = rtsp_proxy.start()
    status = rtsp_proxy.get_status()
    return jsonify({
        'result': 'success' if started else 'failed',
        'stream_status': status
    }), 200 if started else 500

@app.route('/cmd/stop', methods=['POST'])
@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    rtsp_proxy.stop()
    status = rtsp_proxy.get_status()
    return jsonify({
        'result': 'success',
        'stream_status': status
    })

def gen_mjpeg():
    boundary = '--frame'
    while True:
        if not rtsp_proxy.active:
            time.sleep(0.1)
            continue
        frame = rtsp_proxy.get_jpeg()
        if frame is None:
            time.sleep(0.01)
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        time.sleep(0.04)

@app.route('/video', methods=['GET'])
def video_feed():
    if not rtsp_proxy.active:
        abort(503, description="Stream not active. Use /cmd/start to start the stream.")
    return Response(stream_with_context(gen_mjpeg()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)