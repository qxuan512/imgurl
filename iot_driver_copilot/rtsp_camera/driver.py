import os
import threading
import time
from flask import Flask, Response, jsonify, request, stream_with_context
import cv2

# Configuration from environment variables
DEVICE_IP = os.environ.get("DEVICE_IP", "127.0.0.1")
RTSP_PORT = int(os.environ.get("RTSP_PORT", "554"))
RTSP_PATH = os.environ.get("RTSP_PATH", "stream")
RTSP_USER = os.environ.get("RTSP_USER", "")
RTSP_PASS = os.environ.get("RTSP_PASS", "")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# Construct RTSP URL
if RTSP_USER and RTSP_PASS:
    RTSP_URL = f"rtsp://{RTSP_USER}:{RTSP_PASS}@{DEVICE_IP}:{RTSP_PORT}/{RTSP_PATH}"
else:
    RTSP_URL = f"rtsp://{DEVICE_IP}:{RTSP_PORT}/{RTSP_PATH}"

app = Flask(__name__)

# Streaming status
stream_status = {
    "active": False,
    "encoding": {
        "video": "H.264/H.265",
        "audio": "AAC"
    },
    "rtsp_url": RTSP_URL,
    "resolution": None,
    "bitrate": None,
    "last_error": None
}

# Internal state
stream_lock = threading.Lock()
stream_capture = None
stream_thread = None
frame_cache = [None]
exit_event = threading.Event()

def _start_capture():
    global stream_capture, frame_cache, stream_status
    with stream_lock:
        if stream_capture is not None:
            return
        stream_capture = cv2.VideoCapture(RTSP_URL)
        if not stream_capture.isOpened():
            stream_status["active"] = False
            stream_status["last_error"] = "Failed to open RTSP stream"
            stream_capture = None
            return
        stream_status["active"] = True
        stream_status["last_error"] = None
        # Optionally update resolution and bitrate
        width = int(stream_capture.get(cv2.CAP_PROP_FRAME_WIDTH))
        height = int(stream_capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
        stream_status["resolution"] = f"{width}x{height}"
        bitrate = int(stream_capture.get(cv2.CAP_PROP_BITRATE))
        stream_status["bitrate"] = bitrate if bitrate > 0 else None

def _release_capture():
    global stream_capture, frame_cache, stream_status
    with stream_lock:
        if stream_capture is not None:
            stream_capture.release()
            stream_capture = None
        stream_status["active"] = False

def _capture_frames():
    global stream_capture, frame_cache, exit_event
    while not exit_event.is_set():
        with stream_lock:
            if stream_capture is None:
                break
            ret, frame = stream_capture.read()
            if not ret:
                frame_cache[0] = None
                continue
            frame_cache[0] = frame
        time.sleep(0.01)

def gen_mjpeg():
    global stream_capture, frame_cache
    while True:
        if not stream_status["active"]:
            time.sleep(0.1)
            continue
        with stream_lock:
            frame = frame_cache[0]
            if frame is None:
                continue
            ret, jpeg = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            data = jpeg.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + data + b'\r\n')
        time.sleep(0.04)  # ~25fps

@app.route("/stream", methods=["GET"])
def get_stream_status():
    query_res = request.args.get('resolution')
    query_bitrate = request.args.get('bitrate')
    data = dict(stream_status)
    if query_res is not None:
        data["resolution"] = data["resolution"] if data["resolution"] == query_res else None
    if query_bitrate is not None:
        try:
            qb = int(query_bitrate)
            data["bitrate"] = data["bitrate"] if data["bitrate"] == qb else None
        except:
            data["bitrate"] = None
    return jsonify(data)

@app.route("/stream/start", methods=["POST"])
def start_stream():
    global stream_thread, exit_event
    _start_capture()
    if stream_status["active"]:
        if stream_thread is None or not stream_thread.is_alive():
            exit_event.clear()
            stream_thread = threading.Thread(target=_capture_frames, daemon=True)
            stream_thread.start()
        return jsonify({"result": "Stream started", "rtsp_url": RTSP_URL, "encoding": stream_status["encoding"]}), 200
    else:
        return jsonify({"result": "Failed to start stream", "error": stream_status["last_error"]}), 500

@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    global stream_thread, exit_event
    exit_event.set()
    _release_capture()
    if stream_thread is not None:
        stream_thread.join(timeout=2)
        stream_thread = None
    return jsonify({"result": "Stream stopped"}), 200

@app.route("/stream/video", methods=["GET"])
def stream_video():
    if not stream_status["active"]:
        return jsonify({"error": "Stream not active"}), 503
    return Response(stream_with_context(gen_mjpeg()), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)