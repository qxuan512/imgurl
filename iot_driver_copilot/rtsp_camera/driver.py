import os
import threading
import time
from flask import Flask, Response, jsonify, request, abort
import cv2

# Configuration from environment variables
RTSP_CAMERA_IP = os.environ.get('RTSP_CAMERA_IP', '127.0.0.1')
RTSP_CAMERA_PORT = os.environ.get('RTSP_CAMERA_PORT', '554')
RTSP_CAMERA_USER = os.environ.get('RTSP_CAMERA_USER', '')
RTSP_CAMERA_PASS = os.environ.get('RTSP_CAMERA_PASS', '')
RTSP_CAMERA_PATH = os.environ.get('RTSP_CAMERA_PATH', '/stream')
RTSP_CAMERA_PROTO = os.environ.get('RTSP_CAMERA_PROTO', 'rtsp')
HTTP_SERVER_HOST = os.environ.get('HTTP_SERVER_HOST', '0.0.0.0')
HTTP_SERVER_PORT = int(os.environ.get('HTTP_SERVER_PORT', '8080'))

# Example: rtsp://user:pass@IP:PORT/PATH
def build_rtsp_url():
    cred = ''
    if RTSP_CAMERA_USER and RTSP_CAMERA_PASS:
        cred = f"{RTSP_CAMERA_USER}:{RTSP_CAMERA_PASS}@"
    return f"{RTSP_CAMERA_PROTO}://{cred}{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}{RTSP_CAMERA_PATH}"

# Shared state for stream status and control
class StreamState:
    def __init__(self):
        self.active = False
        self.lock = threading.Lock()
        self.video_thread = None
        self.encoding = {'video': 'H.264/H.265', 'audio': 'AAC'}
        self.rtsp_url = build_rtsp_url()
        self.frame = None
        self.last_update = 0
        self.stop_event = threading.Event()

    def start_stream(self):
        with self.lock:
            if not self.active:
                self.stop_event.clear()
                self.video_thread = threading.Thread(target=self._capture_frames)
                self.video_thread.daemon = True
                self.active = True
                self.video_thread.start()
                return True
            return False

    def stop_stream(self):
        with self.lock:
            if self.active:
                self.stop_event.set()
                if self.video_thread:
                    self.video_thread.join(timeout=3)
                self.active = False
                self.frame = None
                return True
            return False

    def _capture_frames(self):
        cap = cv2.VideoCapture(self.rtsp_url)
        if not cap.isOpened():
            self.active = False
            return
        try:
            while not self.stop_event.is_set():
                ret, frame = cap.read()
                if not ret:
                    break
                with self.lock:
                    self.frame = frame
                    self.last_update = time.time()
                time.sleep(0.01)
        finally:
            cap.release()
            with self.lock:
                self.active = False

    def get_frame(self):
        with self.lock:
            return self.frame

    def get_metadata(self):
        with self.lock:
            status = 'active' if self.active else 'inactive'
            return {
                'status': status,
                'rtsp_url': self.rtsp_url if self.active else None,
                'encoding': self.encoding
            }

stream_state = StreamState()
app = Flask(__name__)

@app.route("/stream", methods=['GET'])
def get_stream_status():
    # Optional: support resolution, bitrate filters
    meta = stream_state.get_metadata()
    filters = {}
    res = request.args.get('resolution')
    bitrate = request.args.get('bitrate')
    if res:
        filters['resolution'] = res
    if bitrate:
        filters['bitrate'] = bitrate
    return jsonify({**meta, **filters})

@app.route("/stream/start", methods=['POST'])
def start_stream():
    started = stream_state.start_stream()
    if started:
        return jsonify({"result": "OK", "message": "Stream started"}), 200
    else:
        return jsonify({"result": "AlreadyRunning", "message": "Stream already active"}), 200

@app.route("/stream/stop", methods=['POST'])
def stop_stream():
    stopped = stream_state.stop_stream()
    if stopped:
        return jsonify({"result": "OK", "message": "Stream stopped"}), 200
    else:
        return jsonify({"result": "AlreadyStopped", "message": "Stream already stopped"}), 200

def gen_mjpeg():
    while True:
        if not stream_state.active:
            break
        frame = stream_state.get_frame()
        if frame is None:
            time.sleep(0.05)
            continue
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + jpeg.tobytes() + b'\r\n')
        time.sleep(0.04)

@app.route('/video', methods=['GET'])
def video_feed():
    if not stream_state.active:
        abort(503, description='Stream not active. Start it via /stream/start')
    return Response(gen_mjpeg(), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)