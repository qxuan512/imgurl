import os
import threading
import time
import queue

from flask import Flask, Response, jsonify, request

import cv2

# --- Environment Variables ---
HTTP_SERVER_HOST = os.environ.get('HTTP_SERVER_HOST', '0.0.0.0')
HTTP_SERVER_PORT = int(os.environ.get('HTTP_SERVER_PORT', '8080'))
RTSP_CAMERA_IP = os.environ.get('RTSP_CAMERA_IP')
RTSP_CAMERA_PORT = os.environ.get('RTSP_CAMERA_PORT', '554')
RTSP_USERNAME = os.environ.get('RTSP_USERNAME', '')
RTSP_PASSWORD = os.environ.get('RTSP_PASSWORD', '')
RTSP_STREAM_PATH = os.environ.get('RTSP_STREAM_PATH', 'stream1')
RTSP_TRANSPORT = os.environ.get('RTSP_TRANSPORT', '')  # e.g., 'tcp' or 'udp'

# --- RTSP URL Construction ---
def build_rtsp_url():
    # e.g., rtsp://user:pass@192.168.1.10:554/stream1
    userinfo = ''
    if RTSP_USERNAME and RTSP_PASSWORD:
        userinfo = f"{RTSP_USERNAME}:{RTSP_PASSWORD}@"
    elif RTSP_USERNAME:
        userinfo = f"{RTSP_USERNAME}@"
    url = f"rtsp://{userinfo}{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}/{RTSP_STREAM_PATH}"
    return url

# --- Streaming State Management ---
class StreamState:
    def __init__(self):
        self.active = False
        self.thread = None
        self.frame_queue = queue.Queue(maxsize=10)
        self.stop_event = threading.Event()
        self.rtsp_url = build_rtsp_url()
        self.transport = RTSP_TRANSPORT
        self.last_error = None

    def start(self):
        if self.active:
            return
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._capture_stream, daemon=True)
        self.thread.start()
        self.active = True

    def stop(self):
        self.stop_event.set()
        self.active = False
        # Clear frame queue
        while not self.frame_queue.empty():
            try:
                self.frame_queue.get_nowait()
            except queue.Empty:
                break

    def _capture_stream(self):
        cap = self._open_cv_capture()
        if not cap or not cap.isOpened():
            self.last_error = "Failed to open RTSP stream"
            self.active = False
            return
        self.last_error = None
        while not self.stop_event.is_set():
            ret, frame = cap.read()
            if not ret:
                time.sleep(0.1)
                continue
            # Encode to JPEG
            ret, jpeg = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            try:
                self.frame_queue.put(jpeg.tobytes(), timeout=1)
            except queue.Full:
                pass
        cap.release()

    def _open_cv_capture(self):
        url = self.rtsp_url
        # OpenCV supports transport option via API params
        if self.transport.lower() == "tcp":
            return cv2.VideoCapture(url, cv2.CAP_FFMPEG, [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 5000, cv2.CAP_PROP_RTSP_TRANSPORT, cv2.VideoWriter_fourcc(*'TCP ')])
        elif self.transport.lower() == "udp":
            return cv2.VideoCapture(url, cv2.CAP_FFMPEG, [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 5000, cv2.CAP_PROP_RTSP_TRANSPORT, cv2.VideoWriter_fourcc(*'UDP ')])
        else:
            return cv2.VideoCapture(url)

    def get_frame(self):
        try:
            return self.frame_queue.get(timeout=2)
        except queue.Empty:
            return None

    def get_status(self):
        return {
            "active": self.active,
            "rtsp_url": self.rtsp_url if self.active else None,
            "error": self.last_error
        }

stream_state = StreamState()

# --- Flask App ---
app = Flask(__name__)

@app.route('/stream', methods=['GET'])
def get_stream_status():
    return jsonify(stream_state.get_status()), 200

@app.route('/stream/start', methods=['POST'])
def start_stream():
    if not stream_state.active:
        stream_state.start()
        # Give some time for stream to start
        time.sleep(1)
    status = stream_state.get_status()
    return jsonify({
        "status": "started" if stream_state.active else "error",
        "detail": status
    }), 200 if stream_state.active else 500

@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    if stream_state.active:
        stream_state.stop()
    return jsonify({
        "status": "stopped",
        "detail": stream_state.get_status()
    }), 200

def gen_mjpeg():
    while stream_state.active:
        frame = stream_state.get_frame()
        if frame is None:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
    # When stream is not active, close generator

@app.route('/stream/video', methods=['GET'])
def video_feed():
    if not stream_state.active:
        return jsonify({"error": "Stream not active. Start the stream first."}), 400
    return Response(gen_mjpeg(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)