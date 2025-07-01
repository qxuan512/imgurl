import os
import cv2
import threading
import time
from flask import Flask, Response, jsonify, stream_with_context, request

app = Flask(__name__)

# Config from environment variables
RTSP_URL = os.environ.get("RTSP_URL")   # Full RTSP URL (e.g., rtsp://user:pass@ip:port/stream)
HTTP_HOST = os.environ.get("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8080"))
FRAME_RATE = float(os.environ.get("FRAME_RATE", "10.0"))  # frame/sec for MJPEG stream

if not RTSP_URL:
    raise RuntimeError("RTSP_URL environment variable must be set.")

class CameraStream:
    def __init__(self, rtsp_url):
        self.rtsp_url = rtsp_url
        self.cap = None
        self.running = False
        self.lock = threading.Lock()
        self.latest_frame = None
        self.thread = None

    def start(self):
        with self.lock:
            if self.running:
                return
            self.running = True
            self.cap = cv2.VideoCapture(self.rtsp_url)
            self.thread = threading.Thread(target=self.update_frames, daemon=True)
            self.thread.start()

    def stop(self):
        with self.lock:
            self.running = False
            if self.cap:
                self.cap.release()
                self.cap = None
            self.latest_frame = None
            self.thread = None

    def update_frames(self):
        while self.running and self.cap and self.cap.isOpened():
            ret, frame = self.cap.read()
            if not ret:
                time.sleep(0.2)
                continue
            self.latest_frame = frame
            time.sleep(1.0 / FRAME_RATE)
        self.stop()

    def get_frame(self):
        with self.lock:
            return self.latest_frame.copy() if self.latest_frame is not None else None

    def is_running(self):
        with self.lock:
            return self.running

camera_stream = CameraStream(RTSP_URL)

@app.route("/video", methods=["GET"])
def video_feed():
    if not camera_stream.is_running():
        camera_stream.start()

    def mjpeg_stream():
        while camera_stream.is_running():
            frame = camera_stream.get_frame()
            if frame is not None:
                ret, jpeg = cv2.imencode('.jpg', frame)
                if not ret:
                    continue
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + jpeg.tobytes() + b'\r\n')
            else:
                time.sleep(0.05)
    return Response(stream_with_context(mjpeg_stream()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route("/snap", methods=["GET"])
def snapshot():
    if not camera_stream.is_running():
        camera_stream.start()
        # Wait for a frame to be available
        timeout = 5
        t0 = time.time()
        while camera_stream.get_frame() is None and (time.time() - t0) < timeout:
            time.sleep(0.1)
    frame = camera_stream.get_frame()
    if frame is None:
        return jsonify({"error": "Unable to retrieve frame from camera."}), 500
    ret, jpeg = cv2.imencode('.jpg', frame)
    if not ret:
        return jsonify({"error": "Failed to encode JPEG."}), 500
    return Response(jpeg.tobytes(), mimetype='image/jpeg')

@app.route("/cmd/start", methods=["POST"])
def start_stream():
    camera_stream.start()
    return jsonify({"status": "started"})

@app.route("/cmd/stop", methods=["POST"])
def stop_stream():
    camera_stream.stop()
    return jsonify({"status": "stopped"})

if __name__ == "__main__":
    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)