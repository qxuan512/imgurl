import os
import threading
from flask import Flask, Response, request, stream_with_context, jsonify
import cv2

app = Flask(__name__)

# Configuration from environment variables
RTSP_URL = os.environ.get("RTSP_URL", "")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# Stream state control
stream_lock = threading.Lock()
streaming_enabled = False
cap = None

def open_rtsp_stream():
    global cap
    if cap is not None:
        cap.release()
    cap = cv2.VideoCapture(RTSP_URL)

def close_rtsp_stream():
    global cap
    if cap is not None:
        cap.release()
        cap = None

def gen_mjpeg():
    global cap
    while True:
        with stream_lock:
            if not streaming_enabled:
                break
            if cap is None or not cap.isOpened():
                open_rtsp_stream()
            ret, frame = cap.read()
        if not ret:
            break
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        frame_bytes = jpeg.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
    close_rtsp_stream()

def get_snapshot():
    global cap
    with stream_lock:
        if cap is None or not cap.isOpened():
            open_rtsp_stream()
        ret, frame = cap.read()
    if not ret:
        return None
    ret, jpeg = cv2.imencode('.jpg', frame)
    if not ret:
        return None
    return jpeg.tobytes()

@app.route("/video", methods=["GET"])
def video_stream():
    global streaming_enabled
    with stream_lock:
        if not streaming_enabled:
            return Response("Stream not started. Use /cmd/start.", status=400)
    return Response(stream_with_context(gen_mjpeg()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route("/snap", methods=["GET"])
def snapshot():
    with stream_lock:
        if not streaming_enabled:
            return Response("Stream not started. Use /cmd/start.", status=400)
    img = get_snapshot()
    if img is None:
        return Response("Camera not available", status=503)
    return Response(img, mimetype='image/jpeg')

@app.route("/cmd/start", methods=["POST"])
def start_stream():
    global streaming_enabled
    with stream_lock:
        if streaming_enabled:
            return jsonify({"status": "already started"}), 200
        open_rtsp_stream()
        if cap is None or not cap.isOpened():
            return jsonify({"status": "failed", "error": "Cannot open RTSP stream"}), 500
        streaming_enabled = True
    return jsonify({"status": "started"}), 200

@app.route("/cmd/stop", methods=["POST"])
def stop_stream():
    global streaming_enabled
    with stream_lock:
        if not streaming_enabled:
            return jsonify({"status": "already stopped"}), 200
        streaming_enabled = False
        close_rtsp_stream()
    return jsonify({"status": "stopped"}), 200

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)