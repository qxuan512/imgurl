import os
import threading
import queue
import time
from flask import Flask, Response, jsonify, request

import cv2

# Environment variables for configuration
RTSP_CAMERA_HOST = os.environ.get("RTSP_CAMERA_HOST", "127.0.0.1")
RTSP_CAMERA_PORT = os.environ.get("RTSP_CAMERA_PORT", "554")
RTSP_CAMERA_PATH = os.environ.get("RTSP_CAMERA_PATH", "/stream")
RTSP_CAMERA_USERNAME = os.environ.get("RTSP_CAMERA_USERNAME", "")
RTSP_CAMERA_PASSWORD = os.environ.get("RTSP_CAMERA_PASSWORD", "")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# Build RTSP URL
if RTSP_CAMERA_USERNAME and RTSP_CAMERA_PASSWORD:
    RTSP_URL = f"rtsp://{RTSP_CAMERA_USERNAME}:{RTSP_CAMERA_PASSWORD}@{RTSP_CAMERA_HOST}:{RTSP_CAMERA_PORT}{RTSP_CAMERA_PATH}"
else:
    RTSP_URL = f"rtsp://{RTSP_CAMERA_HOST}:{RTSP_CAMERA_PORT}{RTSP_CAMERA_PATH}"

app = Flask(__name__)

# Streaming state and thread management
streaming_active = False
stream_thread = None
frame_queue = queue.Queue(maxsize=10)
thread_lock = threading.Lock()

def camera_stream_worker():
    global streaming_active
    cap = cv2.VideoCapture(RTSP_URL)
    if not cap.isOpened():
        streaming_active = False
        return

    while streaming_active:
        ret, frame = cap.read()
        if not ret:
            continue
        # Encode as JPEG
        ret2, jpeg = cv2.imencode('.jpg', frame)
        if not ret2:
            continue
        try:
            frame_queue.put(jpeg.tobytes(), timeout=1)
        except queue.Full:
            pass
    cap.release()

def mjpeg_stream_generator():
    while streaming_active:
        try:
            frame = frame_queue.get(timeout=1)
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        except queue.Empty:
            continue

@app.route('/stream', methods=['GET'])
def get_stream_status():
    query_type = request.args.get('type', 'video')
    if streaming_active:
        stream_url = f"http://{SERVER_HOST}:{SERVER_PORT}/stream/video"
    else:
        stream_url = None
    return jsonify({
        'active': streaming_active,
        'type': query_type,
        'stream_url': stream_url
    })

@app.route('/stream/video', methods=['GET'])
def video_stream():
    if not streaming_active:
        return jsonify({'error': 'Stream not active'}), 400
    return Response(mjpeg_stream_generator(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/stream/start', methods=['POST'])
def start_stream():
    global streaming_active, stream_thread
    with thread_lock:
        if streaming_active:
            return jsonify({'result': 'already streaming'}), 200
        streaming_active = True
        # Clear queue
        while not frame_queue.empty():
            try:
                frame_queue.get_nowait()
            except queue.Empty:
                break
        stream_thread = threading.Thread(target=camera_stream_worker, daemon=True)
        stream_thread.start()
    return jsonify({'result': 'stream started'}), 200

@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    global streaming_active, stream_thread
    with thread_lock:
        if not streaming_active:
            return jsonify({'result': 'not streaming'}), 200
        streaming_active = False
        # Wait for thread to finish
        if stream_thread is not None:
            stream_thread.join(timeout=2)
        stream_thread = None
        # Clear queue
        while not frame_queue.empty():
            try:
                frame_queue.get_nowait()
            except queue.Empty:
                break
    return jsonify({'result': 'stream stopped'}), 200

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)