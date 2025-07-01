import os
import threading
import queue
import time
from flask import Flask, Response, jsonify, request
import cv2

# Configuration from environment variables
RTSP_CAMERA_IP = os.environ.get("RTSP_CAMERA_IP")
RTSP_CAMERA_PORT = os.environ.get("RTSP_CAMERA_PORT", "554")
RTSP_CAMERA_USER = os.environ.get("RTSP_CAMERA_USER", "")
RTSP_CAMERA_PASSWORD = os.environ.get("RTSP_CAMERA_PASSWORD", "")
RTSP_CAMERA_PATH = os.environ.get("RTSP_CAMERA_PATH", "stream1")
HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))

# Build RTSP URL
def build_rtsp_url():
    if RTSP_CAMERA_USER and RTSP_CAMERA_PASSWORD:
        return f"rtsp://{RTSP_CAMERA_USER}:{RTSP_CAMERA_PASSWORD}@{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}/{RTSP_CAMERA_PATH}"
    else:
        return f"rtsp://{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}/{RTSP_CAMERA_PATH}"

RTSP_URL = build_rtsp_url()

app = Flask(__name__)

# Shared state for streaming
stream_state = {
    "active": False,
    "thread": None,
    "frame_queue": None,
    "stop_event": None
}

def camera_stream_worker(rtsp_url, frame_queue, stop_event):
    cap = cv2.VideoCapture(rtsp_url)
    if not cap.isOpened():
        frame_queue.put(None)
        return
    while not stop_event.is_set():
        ret, frame = cap.read()
        if not ret:
            time.sleep(0.05)
            continue
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        try:
            frame_queue.put(jpeg.tobytes(), timeout=1)
        except queue.Full:
            pass
    cap.release()

def gen_mjpeg_frames():
    while stream_state["active"]:
        try:
            frame = stream_state["frame_queue"].get(timeout=2)
        except queue.Empty:
            frame = None
        if frame is None:
            break
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
    stream_state["active"] = False

@app.route("/stream", methods=["GET"])
def status_stream():
    status = {
        "active": stream_state["active"],
        "stream_url": f"http://{HTTP_SERVER_HOST}:{HTTP_SERVER_PORT}/stream/video" if stream_state["active"] else None,
        "rtsp_url": RTSP_URL if stream_state["active"] else None
    }
    return jsonify(status)

@app.route("/stream/video", methods=["GET"])
def stream_video():
    if not stream_state["active"]:
        return jsonify({"error": "Stream is not active. Please start the stream first."}), 409
    return Response(gen_mjpeg_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route("/stream/start", methods=["POST"])
def start_stream():
    if stream_state["active"]:
        return jsonify({"result": "Stream already active.", "stream_url": f"http://{HTTP_SERVER_HOST}:{HTTP_SERVER_PORT}/stream/video"}), 200
    frame_queue = queue.Queue(maxsize=10)
    stop_event = threading.Event()
    t = threading.Thread(target=camera_stream_worker, args=(RTSP_URL, frame_queue, stop_event))
    t.daemon = True
    stream_state.update({
        "active": True,
        "thread": t,
        "frame_queue": frame_queue,
        "stop_event": stop_event
    })
    t.start()
    # Wait for camera to warm up and give us first frame
    try:
        frame = frame_queue.get(timeout=4)
        if frame is None:
            stream_state["active"] = False
            return jsonify({"error": "Unable to connect to RTSP stream."}), 500
    except queue.Empty:
        stream_state["active"] = False
        return jsonify({"error": "Timeout connecting to RTSP stream."}), 504
    # Put the frame back for streaming
    frame_queue.put(frame)
    return jsonify({
        "result": "Stream started.",
        "stream_url": f"http://{HTTP_SERVER_HOST}:{HTTP_SERVER_PORT}/stream/video"
    })

@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    if not stream_state["active"]:
        return jsonify({"result": "Stream already stopped."}), 200
    stream_state["stop_event"].set()
    # Drain the thread
    if stream_state["thread"].is_alive():
        stream_state["thread"].join(timeout=3)
    stream_state["active"] = False
    stream_state["thread"] = None
    stream_state["frame_queue"] = None
    stream_state["stop_event"] = None
    return jsonify({"result": "Stream stopped."})

if __name__ == "__main__":
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)