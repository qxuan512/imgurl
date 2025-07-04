import os
import io
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
import socketserver
import cv2
import numpy as np
import time

# Read configuration from environment variables
RTSP_USERNAME = os.environ.get('RTSP_USERNAME', '')
RTSP_PASSWORD = os.environ.get('RTSP_PASSWORD', '')
RTSP_IP = os.environ.get('RTSP_IP', '127.0.0.1')
RTSP_PORT = os.environ.get('RTSP_PORT', '554')
RTSP_PATH = os.environ.get('RTSP_PATH', 'stream1')
HTTP_SERVER_HOST = os.environ.get('HTTP_SERVER_HOST', '0.0.0.0')
HTTP_SERVER_PORT = int(os.environ.get('HTTP_SERVER_PORT', '8080'))

# Build RTSP URL
def get_rtsp_url():
    if RTSP_USERNAME and RTSP_PASSWORD:
        return f"rtsp://{RTSP_USERNAME}:{RTSP_PASSWORD}@{RTSP_IP}:{RTSP_PORT}/{RTSP_PATH}"
    else:
        return f"rtsp://{RTSP_IP}:{RTSP_PORT}/{RTSP_PATH}"

# Global state for stream management
class RTSPStream:
    def __init__(self):
        self.capture = None
        self.streaming = False
        self.lock = threading.Lock()
        self.last_frame = None

    def start_stream(self):
        with self.lock:
            if self.streaming and self.capture is not None and self.capture.isOpened():
                return True
            self.capture = cv2.VideoCapture(get_rtsp_url())
            # Try to open connection for at most 10 attempts
            for i in range(10):
                if self.capture.isOpened():
                    self.streaming = True
                    # Start frame fetch thread
                    threading.Thread(target=self._update_frames, daemon=True).start()
                    return True
                time.sleep(0.5)
            self.streaming = False
            if self.capture is not None:
                self.capture.release()
            self.capture = None
            return False

    def _update_frames(self):
        # Continuously read frames while streaming
        while True:
            with self.lock:
                if not self.streaming or self.capture is None:
                    break
                ret, frame = self.capture.read()
                if not ret:
                    # Connection error: break and stop streaming
                    self.streaming = False
                    self.capture.release()
                    self.capture = None
                    break
                self.last_frame = frame
            time.sleep(0.03)  # ~30 FPS

    def stop_stream(self):
        with self.lock:
            if self.capture is not None:
                self.capture.release()
                self.capture = None
            self.streaming = False
            self.last_frame = None

    def get_snapshot(self):
        with self.lock:
            if not self.streaming or self.capture is None:
                # Try to open, grab one, release
                tmp_cap = cv2.VideoCapture(get_rtsp_url())
                if not tmp_cap.isOpened():
                    return None
                ret, frame = tmp_cap.read()
                tmp_cap.release()
                if not ret:
                    return None
                return frame
            # Use last frame if available, else try to grab new
            frame = self.last_frame
            if frame is not None:
                return frame.copy()
            ret, frame = self.capture.read()
            if not ret:
                return None
            return frame

    def get_mjpeg_generator(self):
        boundary = "--frame"
        while True:
            with self.lock:
                if not self.streaming or self.capture is None:
                    break
                frame = self.last_frame
                if frame is None:
                    continue
                ret, jpeg = cv2.imencode('.jpg', frame)
                if not ret:
                    continue
                img_bytes = jpeg.tobytes()
            yield (b"%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % (boundary.encode(), len(img_bytes)) + img_bytes + b"\r\n")
            time.sleep(0.03)  # ~30 FPS

stream_state = RTSPStream()

class CameraRequestHandler(BaseHTTPRequestHandler):
    def _set_headers(self, status_code=200, content_type='application/json'):
        self.send_response(status_code)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET,POST,OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.send_header('Content-Type', content_type)
        self.end_headers()

    def do_OPTIONS(self):
        self._set_headers()

    def do_GET(self):
        if self.path == '/snapshot':
            frame = stream_state.get_snapshot()
            if frame is None:
                self._set_headers(500, 'application/json')
                self.wfile.write(b'{"error":"Unable to fetch snapshot from camera"}')
                return
            ret, jpeg = cv2.imencode('.jpg', frame)
            if not ret:
                self._set_headers(500, 'application/json')
                self.wfile.write(b'{"error":"Failed to encode JPEG"}')
                return
            self._set_headers(200, 'image/jpeg')
            self.wfile.write(jpeg.tobytes())
            return
        elif self.path == '/stream.mjpg':
            if not stream_state.streaming:
                started = stream_state.start_stream()
                if not started:
                    self._set_headers(500, 'application/json')
                    self.wfile.write(b'{"error":"Unable to start camera stream"}')
                    return
            self.send_response(200)
            self.send_header('Content-type',
                             'multipart/x-mixed-replace; boundary=--frame')
            self.end_headers()
            try:
                for chunk in stream_state.get_mjpeg_generator():
                    self.wfile.write(chunk)
            except Exception:
                pass
            return
        else:
            self._set_headers(404, 'application/json')
            self.wfile.write(b'{"error":"Not found"}')
            return

    def do_POST(self):
        if self.path == '/stream/start':
            started = stream_state.start_stream()
            if started:
                self._set_headers(200, 'application/json')
                self.wfile.write(b'{"status":"streaming started"}')
            else:
                self._set_headers(500, 'application/json')
                self.wfile.write(b'{"error":"Unable to start camera stream"}')
            return
        elif self.path == '/stream/stop':
            stream_state.stop_stream()
            self._set_headers(200, 'application/json')
            self.wfile.write(b'{"status":"streaming stopped"}')
            return
        else:
            self._set_headers(404, 'application/json')
            self.wfile.write(b'{"error":"Not found"}')
            return

    def log_message(self, format, *args):
        # Suppress default logging to stderr
        return

class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True

def run_server():
    server_address = (HTTP_SERVER_HOST, HTTP_SERVER_PORT)
    httpd = ThreadingHTTPServer(server_address, CameraRequestHandler)
    print(f"RTSP Camera HTTP Proxy running at http://{HTTP_SERVER_HOST}:{HTTP_SERVER_PORT}/")
    httpd.serve_forever()

if __name__ == '__main__':
    run_server()