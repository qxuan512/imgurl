import os
import io
import threading
import time
from flask import Flask, Response, jsonify, request, stream_with_context
import cv2

# Environment Variables
RTSP_URL = os.environ.get("RTSP_URL")  # full RTSP URL, e.g. "rtsp://user:pass@192.168.1.10:554/stream"
HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))
CAMERA_NAME = os.environ.get("CAMERA_NAME", "RTSP Camera")
CAMERA_MODEL = os.environ.get("CAMERA_MODEL", "RTSP Camera")
CAMERA_MANUFACTURER = os.environ.get("CAMERA_MANUFACTURER", "Generic")

if not RTSP_URL:
    raise ValueError("RTSP_URL environment variable must be set.")

app = Flask(__name__)

# Streaming State
streaming_state = {
    "active": False,
    "thread": None,
    "capture": None,
    "encoding": {
        "video": "H.264",  # Most RTSP cameras use H.264, could be parameterized
        "audio": "AAC",    # Most RTSP cameras use AAC, could be parameterized
    },
    "current_frame": None,
    "stop_signal": False,
    "last_status": None
}

# Camera Streaming Thread
def camera_stream_worker(rtsp_url, state, resolution=None, bitrate=None):
    cap = cv2.VideoCapture(rtsp_url)
    state["capture"] = cap
    state["active"] = cap.isOpened()
    state["stop_signal"] = False

    if not cap.isOpened():
        state["active"] = False
        return

    while not state["stop_signal"]:
        ret, frame = cap.read()
        if not ret:
            time.sleep(0.04)
            continue
        # Optional: resize or filter frame if needed
        if resolution:
            width, height = resolution
            frame = cv2.resize(frame, (width, height))
        # Store latest frame
        state["current_frame"] = frame
        # throttle
        time.sleep(0.01)
    cap.release()
    state["capture"] = None
    state["active"] = False
    state["current_frame"] = None

# API: GET /stream (status & metadata)
@app.route("/stream", methods=["GET"])
def stream_status():
    resolution = request.args.get("resolution")
    bitrate = request.args.get("bitrate")
    # Resolution parsing
    if resolution:
        try:
            width, height = map(int, resolution.lower().split("x"))
        except:
            width, height = None, None
    else:
        width, height = None, None

    status = {
        "streaming": streaming_state["active"],
        "rtsp_url": RTSP_URL if streaming_state["active"] else None,
        "encoding": streaming_state["encoding"],
        "camera": {
            "name": CAMERA_NAME,
            "model": CAMERA_MODEL,
            "manufacturer": CAMERA_MANUFACTURER,
        },
        "resolution": f"{width}x{height}" if width and height else None,
        "bitrate": bitrate if bitrate else None
    }
    streaming_state["last_status"] = status
    return jsonify(status)

# API: POST /stream/start
@app.route("/stream/start", methods=["POST"])
def stream_start():
    resolution = request.json.get("resolution") if request.is_json and "resolution" in request.json else None
    bitrate = request.json.get("bitrate") if request.is_json and "bitrate" in request.json else None

    # Parse resolution if present
    res_tuple = None
    if resolution:
        try:
            width, height = map(int, resolution.lower().split("x"))
            res_tuple = (width, height)
        except:
            return jsonify({"success": False, "message": "Invalid resolution format. Use WIDTHxHEIGHT."}), 400

    if streaming_state["active"]:
        return jsonify({"success": True, "message": "Stream already active."})

    streaming_state["stop_signal"] = False
    t = threading.Thread(target=camera_stream_worker, args=(RTSP_URL, streaming_state, res_tuple, bitrate), daemon=True)
    t.start()
    time.sleep(1)  # Give thread time to start
    if streaming_state["active"]:
        return jsonify({"success": True, "message": "Stream started."})
    else:
        return jsonify({"success": False, "message": "Failed to start stream (camera not available)."}), 500

# API: POST /stream/stop
@app.route("/stream/stop", methods=["POST"])
def stream_stop():
    if not streaming_state["active"]:
        return jsonify({"success": True, "message": "Stream already stopped."})
    streaming_state["stop_signal"] = True
    # Wait for thread to exit
    for _ in range(30):
        if not streaming_state["active"]:
            break
        time.sleep(0.1)
    return jsonify({"success": True, "message": "Stream stopped."})

def gen_frames():
    while streaming_state["active"]:
        frame = streaming_state["current_frame"]
        if frame is None:
            time.sleep(0.02)
            continue
        ret, buffer = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        jpg_bytes = buffer.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + jpg_bytes + b'\r\n')
        time.sleep(0.02) # ~50 fps max

# API: GET /stream/live (HTTP MJPEG video stream for browser)
@app.route("/stream/live")
def stream_live():
    if not streaming_state["active"]:
        return jsonify({"success": False, "message": "Stream is not active. Start it first via /stream/start."}), 400
    return Response(stream_with_context(gen_frames()), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)