import os
import threading
import time
from flask import Flask, Response, jsonify, request, stream_with_context, send_file
import cv2
import io
import queue

# Configuration from environment variables
RTSP_URL = os.environ.get('RTSP_URL')
SERVER_HOST = os.environ.get('SERVER_HOST', '0.0.0.0')
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))
MJPEG_FPS = int(os.environ.get('MJPEG_FPS', '10'))

if not RTSP_URL:
    raise RuntimeError('RTSP_URL environment variable is required')

app = Flask(__name__)

# Global controls for RTSP stream
streaming_lock = threading.Lock()
streaming_active = threading.Event()
frame_queue = queue.Queue(maxsize=2)
video_thread = None

def rtsp_stream_worker():
    cap = None
    try:
        cap = cv2.VideoCapture(RTSP_URL)
        if not cap.isOpened():
            raise RuntimeError("Unable to open RTSP stream")
        while streaming_active.is_set():
            ret, frame = cap.read()
            if not ret:
                time.sleep(0.1)
                continue
            # Only keep the latest frame
            if not frame_queue.empty():
                try:
                    frame_queue.get_nowait()
                except queue.Empty:
                    pass
            frame_queue.put(frame)
            time.sleep(1.0 / MJPEG_FPS)
    finally:
        if cap:
            cap.release()
        # Clear the queue to avoid stale frames
        while not frame_queue.empty():
            try:
                frame_queue.get_nowait()
            except queue.Empty:
                break

def start_stream():
    with streaming_lock:
        if not streaming_active.is_set():
            streaming_active.set()
            global video_thread
            video_thread = threading.Thread(target=rtsp_stream_worker, daemon=True)
            video_thread.start()
            # Wait for at least one frame to become available
            for _ in range(30):
                if not frame_queue.empty():
                    break
                time.sleep(0.1)

def stop_stream():
    with streaming_lock:
        streaming_active.clear()
        # Wait for thread to finish
        global video_thread
        if video_thread and video_thread.is_alive():
            video_thread.join(timeout=2)
        video_thread = None
        while not frame_queue.empty():
            try:
                frame_queue.get_nowait()
            except queue.Empty:
                break

def mjpeg_generator():
    while streaming_active.is_set():
        try:
            frame = frame_queue.get(timeout=2)
        except queue.Empty:
            continue
        ret, jpeg = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + jpeg.tobytes() + b'\r\n')

@app.route('/video', methods=['GET'])
def get_video():
    if not streaming_active.is_set():
        start_stream()
    return Response(stream_with_context(mjpeg_generator()),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/cmd/start', methods=['POST'])
def api_cmd_start():
    if streaming_active.is_set():
        return jsonify({"status": "already started"}), 200
    start_stream()
    return jsonify({"status": "started"}), 200

@app.route('/cmd/stop', methods=['POST'])
def api_cmd_stop():
    if not streaming_active.is_set():
        return jsonify({"status": "already stopped"}), 200
    stop_stream()
    return jsonify({"status": "stopped"}), 200

@app.route('/snap', methods=['GET'])
def get_snapshot():
    if not streaming_active.is_set():
        start_stream()
    # Try to get a fresh frame
    for _ in range(10):
        try:
            frame = frame_queue.get(timeout=2)
            break
        except queue.Empty:
            continue
    else:
        return jsonify({"error": "Unable to capture snapshot"}), 500
    ret, jpeg = cv2.imencode('.jpg', frame)
    if not ret:
        return jsonify({"error": "Failed to encode JPEG"}), 500
    return Response(jpeg.tobytes(), mimetype='image/jpeg',
                    headers={"Content-Disposition": "inline; filename=snapshot.jpg"})

@app.route('/')
def index():
    return jsonify({
        "device": "RTSP Camera",
        "status": "running",
        "endpoints": ["/video", "/snap", "/cmd/start", "/cmd/stop"]
    })

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)