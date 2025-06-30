import os
import threading
import io
import time
from flask import Flask, Response, jsonify, request, stream_with_context, abort
import cv2

app = Flask(__name__)

# Configuration from environment variables
RTSP_HOST = os.environ.get("RTSP_HOST", "127.0.0.1")
RTSP_PORT = os.environ.get("RTSP_PORT", "554")
RTSP_PATH = os.environ.get("RTSP_PATH", "stream")
RTSP_USERNAME = os.environ.get("RTSP_USERNAME", "")
RTSP_PASSWORD = os.environ.get("RTSP_PASSWORD", "")

HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))

# Construct RTSP URL
if RTSP_USERNAME and RTSP_PASSWORD:
    RTSP_URL = f"rtsp://{RTSP_USERNAME}:{RTSP_PASSWORD}@{RTSP_HOST}:{RTSP_PORT}/{RTSP_PATH}"
else:
    RTSP_URL = f"rtsp://{RTSP_HOST}:{RTSP_PORT}/{RTSP_PATH}"

# Global stream state
stream_state = {
    "active": False,
    "rtsp_url": RTSP_URL,
    "cap": None,
    "lock": threading.Lock()
}
frame_cache = {
    "frame": None,
    "ts": 0
}

class StreamingThread(threading.Thread):
    def __init__(self, cap):
        super().__init__()
        self.cap = cap
        self.running = True

    def run(self):
        global frame_cache
        while self.running and self.cap.isOpened():
            ret, frame = self.cap.read()
            if not ret:
                break
            ret, jpeg = cv2.imencode('.jpg', frame)
            if not ret:
                continue
            with stream_state["lock"]:
                frame_cache["frame"] = jpeg.tobytes()
                frame_cache["ts"] = time.time()
        self.cap.release()

    def stop(self):
        self.running = False

streaming_thread = None

def start_stream():
    global streaming_thread
    with stream_state["lock"]:
        if stream_state["active"]:
            return True
        cap = cv2.VideoCapture(RTSP_URL)
        if not cap.isOpened():
            return False
        stream_state["cap"] = cap
        streaming_thread = StreamingThread(cap)
        streaming_thread.start()
        stream_state["active"] = True
        return True

def stop_stream():
    global streaming_thread
    with stream_state["lock"]:
        if not stream_state["active"]:
            return True
        if streaming_thread is not None:
            streaming_thread.stop()
            streaming_thread.join()
            streaming_thread = None
        stream_state["active"] = False
        stream_state["cap"] = None
        frame_cache["frame"] = None
        return True

@app.route('/stream', methods=['GET'])
def get_stream_status():
    with stream_state["lock"]:
        return jsonify({
            "active": stream_state["active"],
            "rtsp_url": stream_state["rtsp_url"] if stream_state["active"] else None
        })

@app.route('/stream', methods=['POST'])
def post_stream_command():
    cmd = request.args.get("cmd")
    if cmd == "start":
        return start_stream_api()
    elif cmd == "stop":
        return stop_stream_api()
    else:
        return abort(400, "Unknown cmd")

@app.route('/cmd/start', methods=['POST'])
@app.route('/stream/start', methods=['POST'])
def start_stream_api():
    success = start_stream()
    return jsonify({
        "success": success,
        "active": stream_state["active"],
        "rtsp_url": stream_state["rtsp_url"] if stream_state["active"] else None
    }), 200 if success else 500

@app.route('/cmd/stop', methods=['POST'])
@app.route('/stream/stop', methods=['POST'])
def stop_stream_api():
    success = stop_stream()
    return jsonify({
        "success": success,
        "active": stream_state["active"]
    }), 200

def generate_mjpeg():
    while True:
        with stream_state["lock"]:
            if not stream_state["active"]:
                break
            frame = frame_cache["frame"]
        if frame is not None:
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        else:
            time.sleep(0.04)

@app.route('/video')
def video_feed():
    with stream_state["lock"]:
        if not stream_state["active"]:
            abort(404, "Stream not started")
    return Response(stream_with_context(generate_mjpeg()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)