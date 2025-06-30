import os
import io
import threading
import time
from flask import Flask, Response, jsonify, request
import cv2

app = Flask(__name__)

# Environment Variables
CAMERA_IP = os.environ.get('CAMERA_IP', '127.0.0.1')
CAMERA_PORT = os.environ.get('CAMERA_PORT', '554')
CAMERA_USER = os.environ.get('CAMERA_USER', '')
CAMERA_PASS = os.environ.get('CAMERA_PASS', '')
CAMERA_PATH = os.environ.get('CAMERA_PATH', 'stream')
SERVER_HOST = os.environ.get('SERVER_HOST', '0.0.0.0')
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))

# RTSP URL Construction (authentication optional)
if CAMERA_USER and CAMERA_PASS:
    RTSP_URL = f"rtsp://{CAMERA_USER}:{CAMERA_PASS}@{CAMERA_IP}:{CAMERA_PORT}/{CAMERA_PATH}"
else:
    RTSP_URL = f"rtsp://{CAMERA_IP}:{CAMERA_PORT}/{CAMERA_PATH}"

# Streaming State
streaming_status = {
    "active": False,
    "rtsp_url": "",
    "encoding": {
        "video": "H.264/H.265",
        "audio": "AAC"
    },
    "filters": {}
}
stream_lock = threading.Lock()
video_capture = None
stream_thread = None
stop_streaming = threading.Event()


def capture_stream_generator(filters):
    global video_capture
    while not stop_streaming.is_set():
        if video_capture is None or not video_capture.isOpened():
            break
        ret, frame = video_capture.read()
        if not ret:
            break
        # Apply simple filter: resize (if requested)
        if filters.get("resolution"):
            try:
                width, height = map(int, filters["resolution"].split("x"))
                frame = cv2.resize(frame, (width, height))
            except Exception:
                pass
        # Encode frame as JPEG
        ret, buffer = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        jpg_bytes = buffer.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + jpg_bytes + b'\r\n')
        time.sleep(0.04)  # ~25fps


def start_video_capture(filters):
    global video_capture, streaming_status
    with stream_lock:
        if video_capture is not None and video_capture.isOpened():
            return
        video_capture = cv2.VideoCapture(RTSP_URL)
        # Optionally set resolution
        if filters.get("resolution"):
            try:
                width, height = map(int, filters["resolution"].split("x"))
                video_capture.set(cv2.CAP_PROP_FRAME_WIDTH, width)
                video_capture.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
            except Exception:
                pass
        streaming_status["active"] = True
        streaming_status["rtsp_url"] = RTSP_URL
        streaming_status["filters"] = filters


def stop_video_capture():
    global video_capture, streaming_status
    with stream_lock:
        if video_capture is not None:
            video_capture.release()
            video_capture = None
        streaming_status["active"] = False
        streaming_status["rtsp_url"] = ""
        streaming_status["filters"] = {}


@app.route('/stream', methods=['GET'])
def get_stream_status():
    # Query parameters: resolution, bitrate (bitrate ignored in this mock)
    filters = {}
    if 'resolution' in request.args:
        filters['resolution'] = request.args['resolution']
    resp = dict(streaming_status)
    if filters:
        resp["filters"] = filters
    return jsonify(resp)


@app.route('/stream/start', methods=['POST'])
def start_stream():
    # Accept optional JSON params for filters (resolution, ...)
    filters = {}
    if request.is_json:
        data = request.get_json()
        if 'resolution' in data:
            filters['resolution'] = data['resolution']
    start_video_capture(filters)
    return jsonify({"success": True, "message": "Stream started."})


@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    stop_video_capture()
    stop_streaming.set()
    return jsonify({"success": True, "message": "Stream stopped."})


@app.route('/stream/video', methods=['GET'])
def http_video_stream():
    # This is the HTTP MJPEG stream endpoint for browser/CLI consumption
    # Requires stream to be started
    if not streaming_status["active"]:
        return jsonify({"error": "Stream is not active. Start with /stream/start"}), 400
    filters = streaming_status.get("filters", {})
    stop_streaming.clear()
    def generate():
        for frame in capture_stream_generator(filters):
            if stop_streaming.is_set():
                break
            yield frame
    return Response(generate(), mimetype='multipart/x-mixed-replace; boundary=frame')


if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)