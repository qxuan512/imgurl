import os
import threading
import queue
import time
from flask import Flask, Response, jsonify, request
import cv2

app = Flask(__name__)

# Configuration from environment variables
RTSP_URL = os.environ.get('RTSP_URL', 'rtsp://localhost:554/stream')
HTTP_SERVER_HOST = os.environ.get('HTTP_SERVER_HOST', '0.0.0.0')
HTTP_SERVER_PORT = int(os.environ.get('HTTP_SERVER_PORT', '8080'))
STREAM_RESOLUTION = os.environ.get('STREAM_RESOLUTION')  # e.g., '1280x720'
STREAM_BITRATE = os.environ.get('STREAM_BITRATE')        # e.g., '2048k'

# Streaming state
streaming_active = False
stream_thread = None
frame_queue = queue.Queue(maxsize=100)
stream_lock = threading.Lock()
stream_meta = {
    "video_encoding": "H.264/H.265",
    "audio_encoding": "AAC",
    "rtsp_url": RTSP_URL
}

def parse_resolution(res_str):
    if res_str and 'x' in res_str:
        w, h = res_str.lower().split('x')
        return int(w), int(h)
    return None, None

def frame_capture_thread(rtsp_url, resolution=None):
    global streaming_active
    cap = cv2.VideoCapture(rtsp_url)
    if resolution and all(resolution):
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, resolution[0])
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, resolution[1])
    if not cap.isOpened():
        streaming_active = False
        return
    streaming_active = True
    while streaming_active:
        ret, frame = cap.read()
        if not ret:
            time.sleep(0.1)
            continue
        ret, jpeg = cv2.imencode('.jpg', frame)
        if ret:
            try:
                if not frame_queue.full():
                    frame_queue.put(jpeg.tobytes())
            except Exception:
                pass
    cap.release()

def start_streaming():
    global stream_thread, streaming_active
    with stream_lock:
        if streaming_active:
            return
        w, h = parse_resolution(STREAM_RESOLUTION)
        stream_thread = threading.Thread(target=frame_capture_thread, args=(RTSP_URL, (w, h)), daemon=True)
        streaming_active = True
        stream_thread.start()

def stop_streaming():
    global streaming_active, frame_queue
    with stream_lock:
        streaming_active = False
        while not frame_queue.empty():
            frame_queue.get()

@app.route('/stream', methods=['GET'])
def get_stream_status():
    status = {
        "streaming": streaming_active,
        "rtsp_url": RTSP_URL if streaming_active else None,
        "video_encoding": stream_meta["video_encoding"],
        "audio_encoding": stream_meta["audio_encoding"]
    }
    filters = {}
    res = request.args.get('resolution')
    br = request.args.get('bitrate')
    if res:
        filters["resolution"] = res
    if br:
        filters["bitrate"] = br
    if filters:
        status["filters"] = filters
    return jsonify(status), 200

@app.route('/stream/start', methods=['POST'])
def start_stream():
    if streaming_active:
        return jsonify({"result": "Stream already running."}), 200
    start_streaming()
    if streaming_active:
        return jsonify({"result": "Stream started successfully."}), 200
    else:
        return jsonify({"result": "Failed to start stream."}), 500

@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    if not streaming_active:
        return jsonify({"result": "Stream already stopped."}), 200
    stop_streaming()
    return jsonify({"result": "Stream stopped successfully."}), 200

def generate_mjpeg():
    while streaming_active:
        try:
            frame = frame_queue.get(timeout=1)
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        except queue.Empty:
            continue

@app.route('/stream/video', methods=['GET'])
def stream_video():
    if not streaming_active:
        return jsonify({"error": "Stream is not active. Start stream first."}), 400
    return Response(generate_mjpeg(), mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)