import os
import threading
import queue
import time
from flask import Flask, Response, jsonify, request
import cv2

# Configuration from environment variables
DEVICE_IP = os.environ.get("DEVICE_IP", "127.0.0.1")
DEVICE_RTSP_PORT = int(os.environ.get("DEVICE_RTSP_PORT", "554"))
DEVICE_RTSP_PATH = os.environ.get("DEVICE_RTSP_PATH", "stream")
DEVICE_RTSP_USERNAME = os.environ.get("DEVICE_RTSP_USERNAME", "")
DEVICE_RTSP_PASSWORD = os.environ.get("DEVICE_RTSP_PASSWORD", "")
RTSP_PROTOCOL = os.environ.get("RTSP_PROTOCOL", "rtsp")
HTTP_HOST = os.environ.get("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8080"))

SUPPORTED_VIDEO_CODECS = ["H264", "H265"]
SUPPORTED_AUDIO_CODECS = ["AAC"]

app = Flask(__name__)

# Global stream state
streaming = False
stream_thread = None
frame_queue = queue.Queue(maxsize=10)
stream_metadata = {
    "status": "stopped",
    "rtsp_url": "",
    "video_codec": None,
    "audio_codec": None,
    "resolution": None,
    "bitrate": None,
}

def build_rtsp_url():
    if DEVICE_RTSP_USERNAME and DEVICE_RTSP_PASSWORD:
        userinfo = f"{DEVICE_RTSP_USERNAME}:{DEVICE_RTSP_PASSWORD}@"
    else:
        userinfo = ""
    return f"{RTSP_PROTOCOL}://{userinfo}{DEVICE_IP}:{DEVICE_RTSP_PORT}/{DEVICE_RTSP_PATH}"

def stream_worker(rtsp_url, req_resolution=None, req_bitrate=None):
    global streaming, stream_metadata

    cap = cv2.VideoCapture(rtsp_url)
    if not cap.isOpened():
        streaming = False
        stream_metadata["status"] = "error"
        return

    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    # NOTE: OpenCV can't usually get audio codec or bitrate from RTSP.
    stream_metadata.update({
        "status": "streaming",
        "rtsp_url": rtsp_url,
        "video_codec": "H264",   # Most RTSP cams are H264, OpenCV can't query this, so we assume
        "audio_codec": "AAC",
        "resolution": f"{width}x{height}",
        "bitrate": None
    })

    streaming = True
    while streaming:
        ret, frame = cap.read()
        if not ret:
            break
        if req_resolution:
            try:
                w, h = map(int, req_resolution.split("x"))
                frame = cv2.resize(frame, (w, h))
            except Exception:
                pass
        ret, jpg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        try:
            frame_queue.put(jpg.tobytes(), timeout=0.5)
        except queue.Full:
            pass  # drop frame if queue is full

    cap.release()
    streaming = False
    stream_metadata["status"] = "stopped"

@app.route("/stream", methods=["GET"])
def get_stream_status():
    filters = {}
    res = request.args.get("resolution")
    bitrate = request.args.get("bitrate")
    if res:
        filters["resolution"] = res
    if bitrate:
        filters["bitrate"] = bitrate

    status = {
        "status": stream_metadata.get("status", "stopped"),
        "rtsp_url": stream_metadata.get("rtsp_url", ""),
        "video_codec": stream_metadata.get("video_codec", None),
        "audio_codec": stream_metadata.get("audio_codec", None),
        "resolution": stream_metadata.get("resolution", None),
        "bitrate": stream_metadata.get("bitrate", None),
    }
    # Optionally filter
    if res and status["resolution"] != res:
        status["warning"] = f"Requested resolution {res} not currently set; current {status['resolution']}"
    if bitrate and status["bitrate"] != bitrate:
        status["warning"] = f"Requested bitrate {bitrate} not currently set; current {status['bitrate']}"
    return jsonify(status)

@app.route("/stream/start", methods=["POST"])
def start_stream():
    global streaming, stream_thread, stream_metadata, frame_queue
    if streaming:
        return jsonify({"result": "already streaming"}), 200

    # Reset metadata/queue
    frame_queue = queue.Queue(maxsize=10)
    stream_metadata.update({
        "status": "starting",
        "rtsp_url": "",
        "video_codec": None,
        "audio_codec": None,
        "resolution": None,
        "bitrate": None,
    })

    rtsp_url = build_rtsp_url()
    # Optionally receive resolution or bitrate from POST body
    req_json = request.get_json(silent=True) or {}
    req_resolution = req_json.get("resolution")
    req_bitrate = req_json.get("bitrate")

    stream_thread = threading.Thread(target=stream_worker, args=(rtsp_url, req_resolution, req_bitrate), daemon=True)
    stream_thread.start()
    # Give the thread a moment to start and check if the stream is really running
    time.sleep(0.8)
    if not streaming or stream_metadata["status"] == "error":
        return jsonify({"result": "failed to start stream"}), 500
    return jsonify({"result": "stream started", "rtsp_url": rtsp_url})

@app.route("/stream/stop", methods=["POST"])
def stop_stream():
    global streaming, stream_thread
    if not streaming:
        return jsonify({"result": "not streaming"}), 200
    streaming = False
    if stream_thread:
        stream_thread.join(timeout=2)
    return jsonify({"result": "stream stopped"})

def mjpeg_generator():
    while streaming:
        try:
            frame = frame_queue.get(timeout=1)
        except queue.Empty:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')

@app.route('/video', methods=['GET'])
def video_feed():
    if not streaming:
        return jsonify({"error": "stream is not started"}), 400
    return Response(mjpeg_generator(), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == "__main__":
    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)