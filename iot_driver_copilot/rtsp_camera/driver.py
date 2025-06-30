```python
import os
import threading
from flask import Flask, Response, jsonify, request, stream_with_context, abort
import cv2

app = Flask(__name__)

# Environment Variables
RTSP_URL = os.environ.get("RTSP_URL")  # e.g. "rtsp://username:password@192.168.1.10:554/Streaming/Channels/101/"
HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))

# Streaming State
stream_state = {
    "active": False,
    "rtsp_url": RTSP_URL,
    "thread": None,
    "stop_event": threading.Event()
}

def gen_frames(stop_event):
    cap = cv2.VideoCapture(RTSP_URL)
    if not cap.isOpened():
        return
    try:
        while not stop_event.is_set():
            ret, frame = cap.read()
            if not ret:
                break
            # Encode the frame in JPEG format
            ret, buffer = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            frame_bytes = buffer.tobytes()
            # Yield frame in multipart/x-mixed-replace format
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
    finally:
        cap.release()

@app.route("/stream", methods=["GET"])
def stream_status():
    return jsonify({
        "active": stream_state["active"],
        "rtsp_url": RTSP_URL if stream_state["active"] else None
    })

@app.route("/stream", methods=["GET"])
def stream_status_duplicate():
    # API includes two GET /stream endpoints, so both are implemented identically
    return stream_status()

@app.route("/cmd/start", methods=["POST"])
@app.route("/stream/start", methods=["POST"])
def start_stream():
    if stream_state["active"]:
        return jsonify({
            "status": "already_active",
            "rtsp_url": RTSP_URL
        })
    # Validate RTSP URL
    if not RTSP_URL:
        return jsonify({
            "status": "error",
            "message": "RTSP_URL not configured"
        }), 500
    # Test connection to RTSP
    cap = cv2.VideoCapture(RTSP_URL)
    if not cap.isOpened():
        cap.release()
        return jsonify({
            "status": "error",
            "message": "Cannot connect to RTSP stream"
        }), 500
    cap.release()
    stream_state["active"] = True
    stream_state["stop_event"].clear()
    return jsonify({
        "status": "started",
        "rtsp_url": RTSP_URL
    })

@app.route("/cmd/stop", methods=["POST"])
@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    if not stream_state["active"]:
        return jsonify({
            "status": "already_stopped"
        })
    stream_state["active"] = False
    stream_state["stop_event"].set()
    return jsonify({
        "status": "stopped"
    })

@app.route("/video", methods=["GET"])
def video_feed():
    if not stream_state["active"]:
        abort(404, description="Stream not active")
    stop_event = stream_state["stop_event"]
    return Response(stream_with_context(gen_frames(stop_event)),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)
```
