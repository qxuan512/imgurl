import os
import threading
import io
import time
from typing import Optional

from fastapi import FastAPI, Response, Request, HTTPException, status
from fastapi.responses import StreamingResponse, JSONResponse
from pydantic import BaseModel
import cv2
import uvicorn

# =========================
# Environment Variable Configuration
# =========================

CAMERA_IP = os.getenv("CAMERA_IP", "127.0.0.1")
CAMERA_RTSP_PORT = os.getenv("CAMERA_RTSP_PORT", "554")
CAMERA_USERNAME = os.getenv("CAMERA_USERNAME", None)
CAMERA_PASSWORD = os.getenv("CAMERA_PASSWORD", None)
CAMERA_STREAM_PATH = os.getenv("CAMERA_STREAM_PATH", "stream")
CAMERA_PROTOCOL = os.getenv("CAMERA_PROTOCOL", "rtsp")
SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.getenv("SERVER_PORT", "8000"))

# RTSP URL Construction
def build_rtsp_url():
    userinfo = ""
    if CAMERA_USERNAME and CAMERA_PASSWORD:
        userinfo = f"{CAMERA_USERNAME}:{CAMERA_PASSWORD}@"
    return f"{CAMERA_PROTOCOL}://{userinfo}{CAMERA_IP}:{CAMERA_RTSP_PORT}/{CAMERA_STREAM_PATH}"

RTSP_URL = build_rtsp_url()

# =========================
# Streaming State
# =========================

class StreamStatus:
    def __init__(self):
        self.active = False
        self.encoding = {
            "video": "H.264/H.265",
            "audio": "AAC"
        }
        self.capture: Optional[cv2.VideoCapture] = None
        self.last_frame = None
        self.last_frame_time = None
        self.lock = threading.Lock()

    def start(self):
        with self.lock:
            if not self.active:
                self.capture = cv2.VideoCapture(RTSP_URL)
                if not self.capture.isOpened():
                    self.capture = None
                    raise RuntimeError("Unable to open RTSP stream")
                self.active = True

    def stop(self):
        with self.lock:
            if self.active and self.capture:
                self.capture.release()
                self.capture = None
            self.active = False

    def get_status(self):
        with self.lock:
            return {
                "streaming": self.active,
                "rtsp_url": RTSP_URL if self.active else None,
                "encoding": self.encoding
            }

    def read_frame(self):
        with self.lock:
            if not self.active or self.capture is None:
                return None
            ret, frame = self.capture.read()
            if ret:
                self.last_frame = frame
                self.last_frame_time = time.time()
                return frame
            else:
                return None

stream_status = StreamStatus()

# =========================
# FastAPI Application
# =========================

app = FastAPI(title="RTSP Camera DeviceShifu Driver")

# 1. GET /stream: Fetch current stream status and metadata
@app.get("/stream")
async def get_stream_status(request: Request):
    # Optional query params: resolution, bitrate (not used in this example)
    status_data = stream_status.get_status()
    return JSONResponse(content=status_data)

# 2. POST /stream/start: Start the RTSP stream
@app.post("/stream/start")
async def start_stream():
    try:
        stream_status.start()
        return JSONResponse(content={"success": True, "message": "Stream started."})
    except Exception as ex:
        raise HTTPException(status_code=500, detail=f"Failed to start stream: {str(ex)}")

# 3. POST /stream/stop: Stop the RTSP stream
@app.post("/stream/stop")
async def stop_stream():
    stream_status.stop()
    return JSONResponse(content={"success": True, "message": "Stream stopped."})

# 4. GET /stream/live: HTTP MJPEG Video Streaming Endpoint
def generate_mjpeg():
    # Try to keep the stream open as long as the state is "active"
    boundary = "--frame"
    while True:
        if not stream_status.active:
            time.sleep(0.1)
            continue
        frame = stream_status.read_frame()
        if frame is None:
            time.sleep(0.01)
            continue
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        jpg_bytes = jpeg.tobytes()
        yield (
            b"%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % (boundary.encode(), len(jpg_bytes))
            + jpg_bytes
            + b"\r\n"
        )
        # 10-20 fps is enough for MJPEG (configurable if needed)
        time.sleep(0.05)

@app.get("/stream/live")
async def stream_live():
    if not stream_status.active:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Stream is not active. Use /stream/start to initiate."
        )
    headers = {
        "Age": "0",
        "Cache-Control": "no-cache, private",
        "Pragma": "no-cache",
        "Content-Type": "multipart/x-mixed-replace; boundary=--frame"
    }
    return StreamingResponse(generate_mjpeg(), headers=headers)

# ================
# Main Entrypoint
# ================

if __name__ == "__main__":
    uvicorn.run(app, host=SERVER_HOST, port=SERVER_PORT)