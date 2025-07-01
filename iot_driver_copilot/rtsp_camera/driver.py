import os
import io
import threading
import time
from flask import Flask, Response, stream_with_context, request, jsonify
import cv2

app = Flask(__name__)

# Configuration from environment variables
RTSP_URL = os.environ.get("DEVICE_RTSP_URL")
if not RTSP_URL:
    # Allow for legacy or specific values
    RTSP_USER = os.environ.get("DEVICE_RTSP_USER", "")
    RTSP_PASS = os.environ.get("DEVICE_RTSP_PASS", "")
    RTSP_IP = os.environ.get("DEVICE_RTSP_IP", "127.0.0.1")
    RTSP_PORT = os.environ.get("DEVICE_RTSP_PORT", "554")
    RTSP_PATH = os.environ.get("DEVICE_RTSP_PATH", "stream")
    creds = f"{RTSP_USER}:{RTSP_PASS}@" if RTSP_USER or RTSP_PASS else ""
    RTSP_URL = f"rtsp://{creds}{RTSP_IP}:{RTSP_PORT}/{RTSP_PATH}"

HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))

# Stream control state
stream_active = threading.Event()
stream_active.clear()  # Not streaming by default

# To maintain a single RTSP connection/multiplexed for all clients
camera_lock = threading.Lock()
camera_obj = {"cap": None, "thread": None, "last_frame": None, "last_time": 0, "clients": 0}

def camera_start():
    with camera_lock:
        if camera_obj["cap"] is None or not camera_obj["cap"].isOpened():
            cap = cv2.VideoCapture(RTSP_URL)
            if not cap.isOpened():
                camera_obj["cap"] = None
                return False
            camera_obj["cap"] = cap
        # Start background thread grabbing frames if not already running
        if camera_obj["thread"] is None or not camera_obj["thread"].is_alive():
            t = threading.Thread(target=frame_grabber, daemon=True)
            camera_obj["thread"] = t
            t.start()
        return True

def camera_stop():
    with camera_lock:
        if camera_obj["cap"]:
            camera_obj["cap"].release()
        camera_obj["cap"] = None
        camera_obj["last_frame"] = None
        camera_obj["last_time"] = 0
        camera_obj["thread"] = None

def frame_grabber():
    while stream_active.is_set():
        with camera_lock:
            cap = camera_obj["cap"]
            if not cap or not cap.isOpened():
                break
            ret, frame = cap.read()
            if ret:
                camera_obj["last_frame"] = frame
                camera_obj["last_time"] = time.time()
            else:
                camera_obj["last_frame"] = None
        time.sleep(0.02)  # ~50 fps max
    camera_stop()

def gen_mjpeg():
    # Wait until stream is started
    if not stream_active.is_set():
        yield b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + b"\xff\xd8\xff\xd9\r\n"
        return
    camera_obj["clients"] += 1
    try:
        while stream_active.is_set():
            with camera_lock:
                frame = camera_obj["last_frame"]
            if frame is None:
                time.sleep(0.1)
                continue
            ret, jpeg = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            yield (b"--frame\r\n"
                   b"Content-Type: image/jpeg\r\n\r\n" + jpeg.tobytes() + b"\r\n")
            time.sleep(0.04)  # ~25 fps
    finally:
        camera_obj["clients"] -= 1

@app.route('/video', methods=['GET'])
def video_stream():
    if not stream_active.is_set():
        return jsonify({"error": "Stream is not started"}), 503
    # Start camera if not already
    if not camera_start():
        return jsonify({"error": "Unable to access RTSP stream"}), 500
    return Response(stream_with_context(gen_mjpeg()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/snap', methods=['GET'])
def snapshot():
    if not stream_active.is_set():
        return jsonify({"error": "Stream is not started"}), 503
    # Ensure camera and frame
    if not camera_start():
        return jsonify({"error": "Unable to access RTSP stream"}), 500
    for _ in range(10):
        with camera_lock:
            frame = camera_obj["last_frame"]
        if frame is not None:
            ret, jpeg = cv2.imencode('.jpg', frame)
            if ret:
                return Response(jpeg.tobytes(), mimetype='image/jpeg')
        time.sleep(0.1)
    return jsonify({"error": "No frame available"}), 504

@app.route('/cmd/start', methods=['POST'])
def start_stream_cmd():
    if stream_active.is_set():
        return jsonify({"result": "Stream already started"}), 200
    stream_active.set()
    if not camera_start():
        stream_active.clear()
        return jsonify({"error": "Unable to start RTSP stream"}), 500
    return jsonify({"result": "Stream started"}), 200

@app.route('/cmd/stop', methods=['POST'])
def stop_stream_cmd():
    if not stream_active.is_set():
        return jsonify({"result": "Stream already stopped"}), 200
    stream_active.clear()
    # Wait for threads/clients to finish, then release camera
    for _ in range(30):
        with camera_lock:
            if camera_obj["clients"] == 0:
                break
        time.sleep(0.1)
    camera_stop()
    return jsonify({"result": "Stream stopped"}), 200

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)