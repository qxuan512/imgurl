import os
import threading
import time
import io
from http import server
from http.server import BaseHTTPRequestHandler, HTTPServer
import socketserver
import json

import cv2
import numpy as np

# Configuration from environment variables
RTSP_HOST = os.environ.get("RTSP_CAMERA_IP", "127.0.0.1")
RTSP_PORT = int(os.environ.get("RTSP_CAMERA_RTSP_PORT", "554"))
RTSP_PATH = os.environ.get("RTSP_CAMERA_STREAM_PATH", "stream")
RTSP_USERNAME = os.environ.get("RTSP_CAMERA_USERNAME", "")
RTSP_PASSWORD = os.environ.get("RTSP_CAMERA_PASSWORD", "")
HTTP_HOST = os.environ.get("DRIVER_HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("DRIVER_HTTP_PORT", "8080"))

def build_rtsp_url():
    cred = ""
    if RTSP_USERNAME and RTSP_PASSWORD:
        cred = f"{RTSP_USERNAME}:{RTSP_PASSWORD}@"
    return f"rtsp://{cred}{RTSP_HOST}:{RTSP_PORT}/{RTSP_PATH}"

class StreamState:
    def __init__(self):
        self.active = False
        self.lock = threading.Lock()
        self.capture = None
        self.last_frame = None
        self.rtsp_url = build_rtsp_url()
        self.thread = None

    def start_stream(self):
        with self.lock:
            if self.active:
                return True
            self.capture = cv2.VideoCapture(self.rtsp_url)
            if not self.capture.isOpened():
                self.capture = None
                self.active = False
                return False
            self.active = True
            self.thread = threading.Thread(target=self._read_frames, daemon=True)
            self.thread.start()
            return True

    def _read_frames(self):
        while self.active and self.capture:
            ret, frame = self.capture.read()
            if not ret:
                time.sleep(0.1)
                continue
            self.last_frame = frame
        if self.capture:
            self.capture.release()
            self.capture = None

    def stop_stream(self):
        with self.lock:
            self.active = False
            if self.capture:
                self.capture.release()
                self.capture = None
            self.thread = None

    def get_status(self):
        return {"active": self.active, "rtsp_url": self.rtsp_url if self.active else None}

    def get_jpeg_frame(self):
        frame = self.last_frame
        if frame is not None:
            ret, jpeg = cv2.imencode('.jpg', frame)
            if ret:
                return jpeg.tobytes()
        return None

stream_state = StreamState()

class RequestHandler(BaseHTTPRequestHandler):
    server_version = "RTSP2HTTP/1.0"

    def do_GET(self):
        if self.path == "/stream":
            self._handle_stream_status()
        elif self.path == "/stream/video":
            self._handle_video_stream()
        else:
            self.send_error(404, "Not Found")

    def do_POST(self):
        if self.path in ("/cmd/start", "/stream/start"):
            self._handle_cmd_start()
        elif self.path in ("/cmd/stop", "/stream/stop"):
            self._handle_cmd_stop()
        else:
            self.send_error(404, "Not Found")

    def _handle_stream_status(self):
        status = stream_state.get_status()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(status).encode("utf-8"))

    def _handle_cmd_start(self):
        ok = stream_state.start_stream()
        status = stream_state.get_status()
        if ok:
            resp = {"status": "started", **status}
            self.send_response(200)
        else:
            resp = {"status": "failed", **status}
            self.send_response(500)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(resp).encode("utf-8"))

    def _handle_cmd_stop(self):
        stream_state.stop_stream()
        status = stream_state.get_status()
        resp = {"status": "stopped", **status}
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(resp).encode("utf-8"))

    def _handle_video_stream(self):
        # multipart/x-mixed-replace for MJPEG stream
        if not stream_state.active:
            self.send_error(503, "Stream Not Active")
            return
        self.send_response(200)
        self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
        self.end_headers()
        try:
            while stream_state.active:
                frame = stream_state.get_jpeg_frame()
                if frame is None:
                    time.sleep(0.05)
                    continue
                self.wfile.write(b"--frame\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                self.wfile.write(f"Content-Length: {len(frame)}\r\n".encode())
                self.wfile.write(b"\r\n")
                self.wfile.write(frame)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
                time.sleep(0.03)  # ~30 fps max
        except Exception:
            pass

    def log_message(self, format, *args):
        return  # Suppress logs for clean output

class ThreadedHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True

def main():
    server_address = (HTTP_HOST, HTTP_PORT)
    httpd = ThreadedHTTPServer(server_address, RequestHandler)
    print(f"RTSP Camera Driver HTTP server running at http://{HTTP_HOST}:{HTTP_PORT}")
    print(f"  - /stream         : JSON stream status")
    print(f"  - /stream/video   : MJPEG HTTP stream (browser-playable)")
    print(f"  - /cmd/start, /stream/start : Start stream (POST)")
    print(f"  - /cmd/stop,  /stream/stop  : Stop stream (POST)")
    httpd.serve_forever()

if __name__ == "__main__":
    main()