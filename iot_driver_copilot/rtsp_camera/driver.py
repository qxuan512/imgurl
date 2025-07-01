import os
import threading
import cv2
import time
from flask import Flask, Response, jsonify, request, abort

# Configuration from environment variables
RTSP_URL = os.environ.get('RTSP_URL')
CAMERA_IP = os.environ.get('CAMERA_IP')
CAMERA_PORT = os.environ.get('CAMERA_PORT', '554')
CAMERA_USER = os.environ.get('CAMERA_USER', '')
CAMERA_PASS = os.environ.get('CAMERA_PASS', '')
SERVER_HOST = os.environ.get('SERVER_HOST', '0.0.0.0')
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))

# Build RTSP URL if not explicitly set
if not RTSP_URL:
    userinfo = f"{CAMERA_USER}:{CAMERA_PASS}@" if CAMERA_USER or CAMERA_PASS else ""
    RTSP_URL = f"rtsp://{userinfo}{CAMERA_IP}:{CAMERA_PORT}/"

app = Flask(__name__)

# Global state for streaming
streaming_enabled = threading.Event()
frame_lock = threading.Lock()
latest_frame = [None]
video_capture = [None]
stream_thread = [None]

def _open_camera():
    cap = cv2.VideoCapture(RTSP_URL)
    if not cap.isOpened():
        return None
    return cap

def _release_camera():
    if video_capture[0]:
        video_capture[0].release()
        video_capture[0] = None

def _stream_reader():
    cap = _open_camera()
    if not cap:
        streaming_enabled.clear()
        return
    video_capture[0] = cap
    while streaming_enabled.is_set():
        ok, frame = cap.read()
        if not ok:
            time.sleep(0.1)
            continue
        with frame_lock:
            latest_frame[0] = frame
    _release_camera()

@app.route('/cmd/start', methods=['POST'])
def cmd_start():
    if not streaming_enabled.is_set():
        streaming_enabled.set()
        t = threading.Thread(target=_stream_reader, daemon=True)
        stream_thread[0] = t
        t.start()
        return jsonify({'status': 'streaming started'})
    else:
        return jsonify({'status': 'already streaming'})

@app.route('/cmd/stop', methods=['POST'])
def cmd_stop():
    streaming_enabled.clear()
    time.sleep(0.5)
    _release_camera()
    return jsonify({'status': 'streaming stopped'})

def gen_mjpeg():
    while streaming_enabled.is_set():
        with frame_lock:
            frame = latest_frame[0]
        if frame is None:
            time.sleep(0.05)
            continue
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + jpeg.tobytes() + b'\r\n')
        time.sleep(0.04)  # ~25 fps

@app.route('/video', methods=['GET'])
def video_feed():
    if not streaming_enabled.is_set():
        abort(503, description='Stream not started yet. Use /cmd/start')
    return Response(gen_mjpeg(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/snap', methods=['GET'])
def snapshot():
    if not streaming_enabled.is_set():
        abort(503, description='Stream not started yet. Use /cmd/start')
    with frame_lock:
        frame = latest_frame[0]
    if frame is None:
        abort(503, description='No frame available yet')
    ret, jpeg = cv2.imencode('.jpg', frame)
    if not ret:
        abort(500, description='Failed to encode image')
    return Response(jpeg.tobytes(), mimetype='image/jpeg')

@app.route('/healthz', methods=['GET'])
def health():
    return jsonify({'status': 'ok'})

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)