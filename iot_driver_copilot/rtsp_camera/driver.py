import os
import io
import asyncio
import threading
from typing import Optional, Dict, Any
from fastapi import FastAPI, Response, Request, BackgroundTasks, HTTPException, Query
from fastapi.responses import StreamingResponse, JSONResponse
from starlette.concurrency import run_in_threadpool
import av

# Configuration from environment variables
RTSP_HOST = os.getenv("RTSP_CAMERA_IP", "127.0.0.1")
RTSP_PORT = int(os.getenv("RTSP_CAMERA_RTSP_PORT", "554"))
RTSP_USER = os.getenv("RTSP_CAMERA_USERNAME", "")
RTSP_PASS = os.getenv("RTSP_CAMERA_PASSWORD", "")
RTSP_PATH = os.getenv("RTSP_CAMERA_PATH", "stream")
HTTP_SERVER_HOST = os.getenv("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.getenv("HTTP_SERVER_PORT", "8080"))

RTSP_URL = f"rtsp://{f'{RTSP_USER}:{RTSP_PASS}@' if RTSP_USER else ''}{RTSP_HOST}:{RTSP_PORT}/{RTSP_PATH}"

app = FastAPI(title="RTSP Camera HTTP Proxy")

# Streaming state and metadata
stream_state = {
    "active": False,
    "rtsp_url": RTSP_URL,
    "video_codec": None,
    "audio_codec": None,
    "resolution": None,
    "bitrate": None,
    "container": None,
    "task": None
}

stream_state_lock = threading.Lock()

def probe_rtsp_stream(rtsp_url: str, timeout: float = 5.0) -> Dict[str, Any]:
    try:
        container = av.open(rtsp_url, timeout=timeout)
        info = {
            "video_codec": None,
            "audio_codec": None,
            "resolution": None,
            "bitrate": None,
            "container": container.format.name if container.format else None
        }
        for stream in container.streams:
            if stream.type == "video":
                info["video_codec"] = stream.codec.name
                info["resolution"] = f"{stream.width}x{stream.height}"
                info["bitrate"] = stream.bit_rate
            elif stream.type == "audio":
                info["audio_codec"] = stream.codec.name
        container.close()
        return info
    except Exception:
        return {
            "video_codec": None,
            "audio_codec": None,
            "resolution": None,
            "bitrate": None,
            "container": None
        }

def get_stream_metadata():
    meta = probe_rtsp_stream(RTSP_URL)
    return {
        "active": stream_state["active"],
        "rtsp_url": RTSP_URL if stream_state["active"] else None,
        "video_codec": meta["video_codec"],
        "audio_codec": meta["audio_codec"],
        "resolution": meta["resolution"],
        "bitrate": meta["bitrate"],
        "container": meta["container"]
    }

def start_stream_worker():
    with stream_state_lock:
        if not stream_state["active"]:
            stream_state["active"] = True
            meta = probe_rtsp_stream(RTSP_URL)
            stream_state.update(meta)

def stop_stream_worker():
    with stream_state_lock:
        stream_state["active"] = False

def rtp_to_mjpeg(rtsp_url: str, filter_resolution: Optional[str]=None, filter_bitrate: Optional[int]=None):
    """
    Convert RTSP stream to multipart/x-mixed-replace (MJPEG) HTTP stream.
    """
    try:
        container = av.open(rtsp_url)
        video_stream = next((s for s in container.streams if s.type == "video"), None)
        if not video_stream:
            raise RuntimeError("No video stream found")
        for packet in container.demux(video_stream):
            if not stream_state["active"]:
                break
            for frame in packet.decode():
                # Optionally filter by resolution
                if filter_resolution:
                    w, h = map(int, filter_resolution.lower().split("x"))
                    if frame.width != w or frame.height != h:
                        continue
                # Optionally filter by bitrate (approximate, not enforced strictly)
                # Bitrate filter is not directly enforced here
                buf = io.BytesIO()
                frame.to_image().save(buf, format="JPEG")
                jpg = buf.getvalue()
                yield (
                    b"--frame\r\n"
                    b"Content-Type: image/jpeg\r\n\r\n" + jpg + b"\r\n"
                )
        container.close()
    except Exception as e:
        stop_stream_worker()

@app.get("/stream", response_class=JSONResponse)
async def get_stream_status(
    resolution: Optional[str] = Query(None, description="Resolution filter, e.g. 1920x1080"),
    bitrate: Optional[int] = Query(None, description="Bitrate filter (bps)")
):
    meta = get_stream_metadata()
    # Optionally filter by parameters
    if resolution and meta["resolution"]:
        if meta["resolution"].lower() != resolution.lower():
            return JSONResponse(content={"error": "No stream matching the requested resolution"}, status_code=404)
    if bitrate and meta["bitrate"]:
        if int(meta["bitrate"]) != int(bitrate):
            return JSONResponse(content={"error": "No stream matching the requested bitrate"}, status_code=404)
    return meta

@app.post("/stream/start")
async def start_stream():
    with stream_state_lock:
        if stream_state["active"]:
            return {"status": "already active"}
        start_stream_worker()
        return {"status": "stream started", "rtsp_url": RTSP_URL}

@app.post("/stream/stop")
async def stop_stream():
    with stream_state_lock:
        if not stream_state["active"]:
            return {"status": "already stopped"}
        stop_stream_worker()
        return {"status": "stream stopped"}

@app.get("/stream/video")
async def get_video_stream(
    resolution: Optional[str] = Query(None, description="Resolution filter, e.g. 1920x1080"),
    bitrate: Optional[int] = Query(None, description="Bitrate filter (bps)")
):
    with stream_state_lock:
        if not stream_state["active"]:
            raise HTTPException(status_code=404, detail="Stream not active")
    return StreamingResponse(
        rtp_to_mjpeg(RTSP_URL, filter_resolution=resolution, filter_bitrate=bitrate),
        media_type="multipart/x-mixed-replace; boundary=frame"
    )

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT)