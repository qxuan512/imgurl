import os
import threading
import time
from flask import Flask, Response, request, jsonify
import cv2

# Environment Variables Setup
HTTP_SERVER_HOST = os.getenv('SHIFU_HTTP_SERVER_HOST', '0.0.0.0')
HTTP_SERVER_PORT = int(os.getenv('SHIFU_HTTP_SERVER_PORT', '8080'))
RTSP_CAMERA_IP = os.getenv('SHIFU_RTSP_CAMERA_IP', '127.0.0.1')
RTSP_CAMERA_PORT = os.getenv('SHIFU_RTSP_CAMERA_RTSP_PORT', '554')
RTSP_CAMERA_USER = os.getenv('SHIFU_RTSP_CAMERA_USER', '')
RTSP_CAMERA_PASSWORD = os.getenv('SHIFU_RTSP_CAMERA_PASSWORD', '')
RTSP_CAMERA_PATH = os.getenv('SHIFU_RTSP_CAMERA_PATH', 'stream')
# e.g., "stream", "h264", "live.sdp", etc.

SUPPORTED_VIDEO_CODECS = ["H.264", "H.265"]
SUPPORTED_AUDIO_CODECS = ["AAC"]

def build_rtsp_url():
    userinfo = ''
    if RTSP_CAMERA_USER and RTSP_CAMERA_PASSWORD:
        userinfo = f"{RTSP_CAMERA_USER}:{RTSP_CAMERA_PASSWORD}@"
    elif RTSP_CAMERA_USER:
        userinfo = f"{RTSP_CAMERA_USER}@"
    return f"rtsp://{userinfo}{RTSP_CAMERA_IP}:{RTSP_CAMERA_PORT}/{RTSP_CAMERA_PATH}"

class RTSPStreamManager:
    def __init__(self):
        self.rtsp_url = build_rtsp_url()
        self.streaming = False
        self.capture = None
        self.lock = threading.Lock()
        self.last_frame = None
        self.thread = None
        self.resolution = None
        self.bitrate = None
        self.video_codec = "H.264"  # OpenCV does not expose this, so we assume common default.
        self.audio_codec = "AAC"    # Assumed, as OpenCV does not handle audio.

    def start_stream(self, resolution=None, bitrate=None):
        with self.lock:
            if self.streaming:
                return True
            self.capture = cv2.VideoCapture(self.rtsp_url)
            if not self.capture.isOpened():
                self.capture = None
                return False
            self.streaming = True
            self.resolution = resolution
            self.bitrate = bitrate
            self.thread = threading.Thread(target=self._frame_worker)
            self.thread.daemon = True
            self.thread.start()
            return True

    def stop_stream(self):
        with self.lock:
            if not self.streaming:
                return True
            self.streaming = False
            if self.capture:
                self.capture.release()
                self.capture = None
            self.thread = None
            self.last_frame = None
            return True

    def _frame_worker(self):
        while self.streaming and self.capture:
            ret, frame = self.capture.read()
            if not ret:
                self.last_frame = None
                time.sleep(0.05)
                continue
            # Optionally, resize frame to requested resolution
            if self.resolution:
                try:
                    width, height = map(int, self.resolution.lower().split('x'))
                    frame = cv2.resize(frame, (width, height))
                except Exception:
                    pass
            # Encode frame as JPEG for browser streaming
            ok, jpeg = cv2.imencode('.jpg', frame)
            if ok:
                self.last_frame = jpeg.tobytes()
            else:
                self.last_frame = None
            time.sleep(0.03)  # ~30fps

    def get_frame(self):
        with self.lock:
            return self.last_frame

    def is_streaming(self):
        with self.lock:
            return self.streaming

    def get_metadata(self):
        meta = {
            "status": "streaming" if self.streaming else "stopped",
            "rtsp_url": self.rtsp_url if self.streaming else None,
            "video_codec": self.video_codec,
            "audio_codec": self.audio_codec,
            "resolution": self.resolution,
            "bitrate": self.bitrate,
        }
        return meta

# Flask App Setup
app = Flask(__name__)
stream_manager = RTSPStreamManager()

@app.route('/stream', methods=['GET'])
def stream_status():
    # Optional filters
    resolution = request.args.get('resolution')
    bitrate = request.args.get('bitrate')
    meta = stream_manager.get_metadata()
    if resolution and meta["resolution"] != resolution:
        meta["status"] = "stopped"  # Not streaming at requested resolution
        meta["rtsp_url"] = None
    if bitrate and meta["bitrate"] != bitrate:
        meta["status"] = "stopped"
        meta["rtsp_url"] = None
    return jsonify(meta)

@app.route('/stream/start', methods=['POST'])
def start_stream():
    req = request.get_json(silent=True) or {}
    resolution = req.get('resolution')
    bitrate = req.get('bitrate')
    success = stream_manager.start_stream(resolution=resolution, bitrate=bitrate)
    if success:
        return jsonify({"result": "success", "message": "Stream started"}), 200
    else:
        return jsonify({"result": "error", "message": "Failed to start stream"}), 500

@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    success = stream_manager.stop_stream()
    if success:
        return jsonify({"result": "success", "message": "Stream stopped"}), 200
    else:
        return jsonify({"result": "error", "message": "Failed to stop stream"}), 500

@app.route('/stream/video', methods=['GET'])
def stream_video():
    if not stream_manager.is_streaming():
        return jsonify({"result": "error", "message": "Stream is not active"}), 503

    def generate():
        while stream_manager.is_streaming():
            frame = stream_manager.get_frame()
            if frame is not None:
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
            else:
                time.sleep(0.05)
    return Response(generate(), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)