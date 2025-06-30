import os
import threading
import time
from flask import Flask, Response, jsonify, request
import cv2

# Configuration via environment variables
RTSP_URL = os.environ.get("RTSP_URL")
HTTP_HOST = os.environ.get("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8080"))
CAMERA_NAME = os.environ.get("CAMERA_NAME", "RTSP Camera")
CAMERA_MODEL = os.environ.get("CAMERA_MODEL", "RTSP Camera")
CAMERA_MANUFACTURER = os.environ.get("CAMERA_MANUFACTURER", "Generic")

if not RTSP_URL:
    raise EnvironmentError("RTSP_URL environment variable must be set")

app = Flask(__name__)

class StreamState:
    def __init__(self):
        self.streaming = False
        self.resolution = None
        self.bitrate = None
        self.codec_video = "H.264"  # default, will be updated if detected
        self.codec_audio = "AAC"
        self.capture_thread = None
        self.lock = threading.Lock()
        self.frame = None
        self.should_run = False

    def start_stream(self, filters=None):
        with self.lock:
            if self.streaming:
                return
            self.should_run = True
            self.capture_thread = threading.Thread(target=self._capture, args=(filters,))
            self.capture_thread.daemon = True
            self.capture_thread.start()
            # Wait for the first frame or timeout
            for _ in range(20):
                if self.frame is not None:
                    break
                time.sleep(0.1)
            self.streaming = True

    def stop_stream(self):
        with self.lock:
            self.should_run = False
            self.streaming = False
            if self.capture_thread and self.capture_thread.is_alive():
                self.capture_thread.join(timeout=2)
            self.capture_thread = None
            self.frame = None

    def _capture(self, filters):
        cap = cv2.VideoCapture(RTSP_URL)
        # Apply filters if available (resolution, etc.)
        if filters:
            if "resolution" in filters:
                width, height = map(int, filters["resolution"].split("x"))
                cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
                cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        # Attempt to auto-detect properties
        self.resolution = f"{int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))}x{int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))}"
        self.bitrate = int(cap.get(cv2.CAP_PROP_BITRATE)) if cap.get(cv2.CAP_PROP_BITRATE) > 0 else None
        # (Codec detection may not be reliable through OpenCV; keep defaults)
        while self.should_run and cap.isOpened():
            ret, frame = cap.read()
            if not ret:
                continue
            with self.lock:
                self.frame = frame
        cap.release()

    def get_jpeg_frame(self):
        with self.lock:
            if self.frame is None:
                return None
            ret, jpeg = cv2.imencode('.jpg', self.frame)
            if ret:
                return jpeg.tobytes()
            return None

stream_state = StreamState()

@app.route("/stream", methods=["GET"])
def get_stream_status():
    # Optional filters: resolution, bitrate
    filters = {}
    if "resolution" in request.args:
        filters["resolution"] = request.args["resolution"]
    if "bitrate" in request.args:
        filters["bitrate"] = request.args["bitrate"]

    status = {
        "device_name": CAMERA_NAME,
        "device_model": CAMERA_MODEL,
        "manufacturer": CAMERA_MANUFACTURER,
        "status": "active" if stream_state.streaming else "inactive",
        "rtsp_url": RTSP_URL if stream_state.streaming else None,
        "video_codec": stream_state.codec_video,
        "audio_codec": stream_state.codec_audio,
        "resolution": stream_state.resolution,
        "bitrate": stream_state.bitrate
    }
    return jsonify(status)

@app.route("/stream/start", methods=["POST"])
def start_stream():
    # Allow starting with optional resolution/bitrate in JSON body
    filters = {}
    if request.is_json:
        body = request.get_json()
        if "resolution" in body:
            filters["resolution"] = body["resolution"]
        if "bitrate" in body:
            filters["bitrate"] = body["bitrate"]
    stream_state.start_stream(filters=filters)
    return jsonify({"result": "RTSP stream started", "status": "active"}), 200

@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    stream_state.stop_stream()
    return jsonify({"result": "RTSP stream stopped", "status": "inactive"}), 200

def mjpeg_stream_generator():
    while stream_state.streaming:
        frame = stream_state.get_jpeg_frame()
        if frame is not None:
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        else:
            time.sleep(0.05)

@app.route("/stream/live", methods=["GET"])
def stream_live():
    # Start stream if not already running
    if not stream_state.streaming:
        stream_state.start_stream()
    return Response(mjpeg_stream_generator(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)