import os
import threading
import time
from flask import Flask, Response, jsonify, request
import cv2

app = Flask(__name__)

RTSP_URL = os.environ.get("RTSP_URL", "rtsp://localhost:554/stream")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))
RTSP_READ_TIMEOUT = int(os.environ.get("RTSP_READ_TIMEOUT", "5"))
HTTP_STREAM_PATH = "/video"
SUPPORTED_VIDEO_CODECS = ["H264", "H265"]
SUPPORTED_AUDIO_CODECS = ["AAC"]

class StreamState:
    def __init__(self):
        self.active = False
        self.capture = None
        self.thread = None
        self.rtsp_url = RTSP_URL
        self.codec = None
        self.bitrate = None
        self.resolution = None
        self.last_frame = None
        self.lock = threading.Lock()

    def start(self, filters=None):
        with self.lock:
            if not self.active:
                self.active = True
                self.capture = cv2.VideoCapture(self.rtsp_url)
                # Try to read properties (resolution, codec, etc.)
                width = int(self.capture.get(cv2.CAP_PROP_FRAME_WIDTH))
                height = int(self.capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
                fps = self.capture.get(cv2.CAP_PROP_FPS)
                self.resolution = f"{width}x{height}"
                self.bitrate = None  # Not accessible via OpenCV
                self.codec = "H264"  # Assume H264 for generic RTSP
                # Optionally apply filters (not implemented)
            return self.active

    def stop(self):
        with self.lock:
            if self.active:
                self.active = False
                if self.capture:
                    self.capture.release()
                    self.capture = None

    def get_status(self):
        with self.lock:
            return {
                "streaming": self.active,
                "rtsp_url": self.rtsp_url if self.active else None,
                "video_codec": self.codec,
                "audio_codec": "AAC",
                "resolution": self.resolution,
                "bitrate": self.bitrate
            }

    def get_frame(self):
        with self.lock:
            if not self.active or not self.capture:
                return None
            ret, frame = self.capture.read()
            if not ret:
                return None
            self.last_frame = frame
            return frame

stream_state = StreamState()

@app.route("/stream", methods=["GET"])
def stream_status():
    # Optional filters (not implemented; just passthrough)
    filters = {
        "resolution": request.args.get("resolution"),
        "bitrate": request.args.get("bitrate")
    }
    status = stream_state.get_status()
    if filters["resolution"] and status["resolution"]:
        status["resolution"] = filters["resolution"]
    if filters["bitrate"] and status["bitrate"]:
        status["bitrate"] = filters["bitrate"]
    return jsonify(status), 200

@app.route("/stream/start", methods=["POST"])
def start_stream():
    filters = request.get_json() if request.is_json else None
    started = stream_state.start(filters)
    if started:
        return jsonify({"message": "Stream started", "rtsp_url": stream_state.rtsp_url}), 200
    else:
        return jsonify({"error": "Failed to start stream"}), 500

@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    stream_state.stop()
    return jsonify({"message": "Stream stopped"}), 200

def gen_frames():
    while True:
        if not stream_state.active:
            time.sleep(0.2)
            continue
        frame = stream_state.get_frame()
        if frame is None:
            time.sleep(0.1)
            continue
        ret, buffer = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        frame_bytes = buffer.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')

@app.route("/video", methods=["GET"])
def video_feed():
    if not stream_state.active:
        return jsonify({"error": "Stream is not active"}), 400
    return Response(gen_frames(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)