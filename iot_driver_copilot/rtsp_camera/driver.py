import os
import cv2
import threading
import queue
import time
from flask import Flask, Response, send_file, jsonify, make_response

# Environment variables for configuration
CAMERA_IP = os.environ.get("CAMERA_IP", "127.0.0.1")
CAMERA_RTSP_PORT = int(os.environ.get("CAMERA_RTSP_PORT", 554))
CAMERA_RTSP_PATH = os.environ.get("CAMERA_RTSP_PATH", "stream")
CAMERA_RTSP_USER = os.environ.get("CAMERA_RTSP_USER", "")
CAMERA_RTSP_PASSWORD = os.environ.get("CAMERA_RTSP_PASSWORD", "")
HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", 8000))

app = Flask(__name__)

# RTSP URL Construction
def build_rtsp_url():
    auth = ""
    if CAMERA_RTSP_USER and CAMERA_RTSP_PASSWORD:
        auth = f"{CAMERA_RTSP_USER}:{CAMERA_RTSP_PASSWORD}@"
    return f"rtsp://{auth}{CAMERA_IP}:{CAMERA_RTSP_PORT}/{CAMERA_RTSP_PATH}"

RTSP_URL = build_rtsp_url()

# Streaming control
streaming_thread = None
streaming_running = False
frame_queue = queue.Queue(maxsize=100)

def rtsp_stream_worker(rtsp_url, frame_queue, stop_event):
    cap = cv2.VideoCapture(rtsp_url)
    if not cap.isOpened():
        stop_event.set()
        return
    while not stop_event.is_set():
        ret, frame = cap.read()
        if not ret:
            time.sleep(0.1)
            continue
        if not frame_queue.full():
            frame_queue.put(frame)
    cap.release()

stop_event = threading.Event()

@app.route("/snapshot", methods=["GET"])
def get_snapshot():
    cap = cv2.VideoCapture(RTSP_URL)
    if not cap.isOpened():
        return make_response(jsonify({'error': 'Unable to open camera stream'}), 503)
    ret, frame = cap.read()
    cap.release()
    if not ret or frame is None:
        return make_response(jsonify({'error': 'Unable to capture snapshot'}), 500)
    ret, jpeg = cv2.imencode('.jpg', frame)
    if not ret:
        return make_response(jsonify({'error': 'Unable to encode snapshot'}), 500)
    response = Response(jpeg.tobytes(), mimetype='image/jpeg')
    response.headers['Content-Disposition'] = 'inline; filename=snapshot.jpg'
    return response

@app.route("/stream/start", methods=["POST"])
def start_stream():
    global streaming_thread, streaming_running, stop_event, frame_queue
    if streaming_running:
        return jsonify({'status': 'already running'})
    # Clear prior state
    stop_event.clear()
    with frame_queue.mutex:
        frame_queue.queue.clear()
    streaming_thread = threading.Thread(target=rtsp_stream_worker, args=(RTSP_URL, frame_queue, stop_event))
    streaming_thread.daemon = True
    streaming_thread.start()
    streaming_running = True
    return jsonify({'status': 'stream started'})

@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    global streaming_running, stop_event
    if not streaming_running:
        return jsonify({'status': 'already stopped'})
    stop_event.set()
    streaming_running = False
    return jsonify({'status': 'stream stopped'})

def gen_mjpeg():
    """Generator that yields MJPEG frames from frame_queue."""
    while streaming_running:
        try:
            frame = frame_queue.get(timeout=2)
        except queue.Empty:
            continue
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + jpeg.tobytes() + b'\r\n')
    # End stream
    yield b''

@app.route("/stream/live")
def live_stream():
    if not streaming_running:
        return make_response(jsonify({'error': 'Stream not running. Please POST /stream/start first.'}), 409)
    return Response(gen_mjpeg(), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)