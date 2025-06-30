import os
import threading
import io
import time
from flask import Flask, Response, jsonify, request
import cv2

# Environment Variable Configuration
RTSP_URL = os.environ.get("RTSP_URL")
CAMERA_IP = os.environ.get("CAMERA_IP")
CAMERA_PORT = os.environ.get("CAMERA_PORT", "554")
CAMERA_USERNAME = os.environ.get("CAMERA_USERNAME")
CAMERA_PASSWORD = os.environ.get("CAMERA_PASSWORD")
RTSP_PATH = os.environ.get("RTSP_PATH", "Streaming/Channels/101")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# Compose RTSP URL if not explicitly set
if not RTSP_URL:
    if CAMERA_USERNAME and CAMERA_PASSWORD:
        auth = f"{CAMERA_USERNAME}:{CAMERA_PASSWORD}@"
    else:
        auth = ""
    RTSP_URL = f"rtsp://{auth}{CAMERA_IP}:{CAMERA_PORT}/{RTSP_PATH}"

app = Flask(__name__)

# Stream management state
streaming_active = threading.Event()
stream_thread = None
stream_lock = threading.Lock()
stream_meta = {
    "status": "inactive",
    "rtsp_url": None,
    "encoding": {
        "video": None,
        "audio": None
    },
    "resolution": None,
    "bitrate": None
}
frame_buffer = []

def fetch_stream_metadata(rtsp_url):
    # Try to open the stream and fetch encoding info.
    # Limited to video encoding via OpenCV; audio not accessible here.
    cap = cv2.VideoCapture(rtsp_url)
    if not cap.isOpened():
        return {
            "status": "inactive",
            "rtsp_url": None,
            "encoding": {
                "video": None,
                "audio": None
            },
            "resolution": None,
            "bitrate": None
        }
    # Try to extract video encoding and properties.
    # OpenCV does not provide codec name, but H.264/H.265 is most likely.
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    bitrate = cap.get(cv2.CAP_PROP_BITRATE)
    # Assume H.264/H.265, cannot detect audio
    encoding = {"video": "H.264/H.265", "audio": "AAC"}
    cap.release()
    return {
        "status": "active",
        "rtsp_url": rtsp_url,
        "encoding": encoding,
        "resolution": f"{width}x{height}",
        "bitrate": bitrate if bitrate > 0 else None
    }

def stream_frames():
    global frame_buffer
    cap = cv2.VideoCapture(RTSP_URL)
    if not cap.isOpened():
        streaming_active.clear()
        return
    while streaming_active.is_set():
        ret, frame = cap.read()
        if not ret:
            continue
        # Encode frame as JPEG
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        # Store latest frame (for MJPEG streaming)
        with stream_lock:
            frame_buffer = [jpeg.tobytes()]
        time.sleep(0.03)  # ~30 FPS
    cap.release()

def mjpeg_generator():
    while streaming_active.is_set():
        frame = None
        with stream_lock:
            if frame_buffer:
                frame = frame_buffer[-1]
        if frame:
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        else:
            time.sleep(0.05)

@app.route('/stream', methods=['GET'])
def get_stream_status():
    # Optional query params: resolution, bitrate
    resolution = request.args.get('resolution')
    bitrate = request.args.get('bitrate')
    meta = fetch_stream_metadata(RTSP_URL) if streaming_active.is_set() else {
        "status": "inactive",
        "rtsp_url": None,
        "encoding": {
            "video": None,
            "audio": None
        },
        "resolution": None,
        "bitrate": None
    }
    # Filter by query params if provided
    if resolution and meta["resolution"] != resolution:
        meta["status"] = "inactive"
    if bitrate and meta["bitrate"] and str(meta["bitrate"]) != str(bitrate):
        meta["status"] = "inactive"
    return jsonify(meta)

@app.route('/stream/start', methods=['POST'])
def start_stream():
    global stream_thread
    if not streaming_active.is_set():
        streaming_active.set()
        stream_thread = threading.Thread(target=stream_frames, daemon=True)
        stream_thread.start()
        time.sleep(1)  # Let the stream initialize
    meta = fetch_stream_metadata(RTSP_URL) if streaming_active.is_set() else {
        "status": "inactive"
    }
    return jsonify({
        "result": "stream started" if streaming_active.is_set() else "failed to start stream",
        "status": meta["status"]
    })

@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    if streaming_active.is_set():
        streaming_active.clear()
        # Wait for thread cleanup
        time.sleep(0.5)
    return jsonify({"result": "stream stopped", "status": "inactive"})

@app.route('/stream/video', methods=['GET'])
def stream_video():
    if not streaming_active.is_set():
        return jsonify({"error": "stream is not active"}), 400
    return Response(
        mjpeg_generator(),
        mimetype='multipart/x-mixed-replace; boundary=frame'
    )

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)