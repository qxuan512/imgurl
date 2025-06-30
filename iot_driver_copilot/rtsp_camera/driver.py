import os
import threading
import time
import io
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs
import json

import cv2
import numpy as np

# ==== Environment Variables ====
DEVICE_IP = os.environ.get("DEVICE_IP", "127.0.0.1")
DEVICE_RTSP_PORT = int(os.environ.get("DEVICE_RTSP_PORT", "554"))
DEVICE_RTSP_PATH = os.environ.get("DEVICE_RTSP_PATH", "stream1")
DEVICE_RTSP_USER = os.environ.get("DEVICE_RTSP_USER", "")
DEVICE_RTSP_PASS = os.environ.get("DEVICE_RTSP_PASS", "")

HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))

CAMERA_VIDEO_CODEC = os.environ.get("CAMERA_VIDEO_CODEC", "H.264")
CAMERA_AUDIO_CODEC = os.environ.get("CAMERA_AUDIO_CODEC", "AAC")

# ==== Camera Stream State Management ====
class StreamState:
    def __init__(self):
        self.active = False
        self.capture_thread = None
        self.lock = threading.Lock()
        self.frame = None
        self.rtsp_url = self.build_rtsp_url()
        self.stop_event = threading.Event()
        self.last_update = 0

    def build_rtsp_url(self):
        auth = ""
        if DEVICE_RTSP_USER:
            auth = f"{DEVICE_RTSP_USER}:{DEVICE_RTSP_PASS}@"
        return f"rtsp://{auth}{DEVICE_IP}:{DEVICE_RTSP_PORT}/{DEVICE_RTSP_PATH}"

    def start(self):
        with self.lock:
            if not self.active:
                self.stop_event.clear()
                self.capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
                self.active = True
                self.capture_thread.start()
                time.sleep(0.5)  # Give time for first frame to arrive

    def stop(self):
        with self.lock:
            if self.active:
                self.stop_event.set()
                if self.capture_thread is not None:
                    self.capture_thread.join(timeout=2)
                self.active = False
                self.capture_thread = None
                self.frame = None

    def _capture_loop(self):
        cap = cv2.VideoCapture(self.rtsp_url)
        if not cap.isOpened():
            self.active = False
            return
        while not self.stop_event.is_set():
            ret, frame = cap.read()
            if ret:
                with self.lock:
                    self.frame = frame
                    self.last_update = time.time()
            else:
                time.sleep(0.1)
        cap.release()
        with self.lock:
            self.frame = None

    def get_frame(self):
        with self.lock:
            return None if self.frame is None else self.frame.copy()

    def is_active(self):
        with self.lock:
            return self.active

    def get_rtsp_url(self):
        return self.rtsp_url

    def get_encoding(self):
        return {
            "video": CAMERA_VIDEO_CODEC,
            "audio": CAMERA_AUDIO_CODEC
        }

stream_state = StreamState()

# ==== HTTP Server Handler ====
class CameraHTTPRequestHandler(BaseHTTPRequestHandler):
    server_version = "CameraHTTPProxy/1.0"

    def _send_json(self, obj, code=200):
        self.send_response(code)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(obj).encode('utf-8'))

    def _send_mjpeg_stream(self):
        self.send_response(200)
        self.send_header('Content-type', 'multipart/x-mixed-replace; boundary=frame')
        self.end_headers()
        try:
            while stream_state.is_active():
                frame = stream_state.get_frame()
                if frame is not None:
                    # Encode as JPEG
                    ret, jpeg = cv2.imencode('.jpg', frame)
                    if not ret:
                        continue
                    self.wfile.write(b'--frame\r\n')
                    self.wfile.write(b'Content-Type: image/jpeg\r\n\r\n')
                    self.wfile.write(jpeg.tobytes())
                    self.wfile.write(b'\r\n')
                time.sleep(0.04)  # ~25 fps
        except (ConnectionResetError, BrokenPipeError):
            return

    def do_GET(self):
        parsed_path = urlparse(self.path)
        if parsed_path.path == '/stream':
            qs = parse_qs(parsed_path.query)
            # Optionally support filters, e.g., ?resolution=720p
            status = {
                "streaming": stream_state.is_active(),
                "rtsp_url": stream_state.get_rtsp_url() if stream_state.is_active() else "",
                "encoding": stream_state.get_encoding(),
            }
            self._send_json(status)
        elif parsed_path.path == '/stream/live':
            if not stream_state.is_active():
                self.send_response(503)
                self.end_headers()
                self.wfile.write(b"Stream is not active")
                return
            self._send_mjpeg_stream()
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Not Found")

    def do_POST(self):
        if self.path == '/stream/start':
            stream_state.start()
            resp = {
                "status": "started",
                "rtsp_url": stream_state.get_rtsp_url()
            }
            self._send_json(resp)
        elif self.path == '/stream/stop':
            stream_state.stop()
            resp = {
                "status": "stopped"
            }
            self._send_json(resp)
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Not Found")

    def log_message(self, format, *args):
        # Silence or customize as desired
        return

def run_server():
    httpd = HTTPServer((HTTP_SERVER_HOST, HTTP_SERVER_PORT), CameraHTTPRequestHandler)
    print(f"HTTP server running on {HTTP_SERVER_HOST}:{HTTP_SERVER_PORT}")
    httpd.serve_forever()

if __name__ == "__main__":
    run_server()