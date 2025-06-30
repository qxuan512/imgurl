import os
import threading
import time
import queue
import io
from flask import Flask, Response, jsonify, request
import cv2

# Configuration from environment variables
RTSP_URL = os.environ.get('RTSP_URL', '')
HTTP_SERVER_HOST = os.environ.get('HTTP_SERVER_HOST', '0.0.0.0')
HTTP_SERVER_PORT = int(os.environ.get('HTTP_SERVER_PORT', '8080'))
HTTP_STREAM_PATH = os.environ.get('HTTP_STREAM_PATH', '/video')
HTTP_STREAM_PORT = int(os.environ.get('HTTP_STREAM_PORT', HTTP_SERVER_PORT))  # Not used but configurable if needed

# Video streaming state
streaming_state = {
    "active": False,
    "thread": None,
    "frame_queue": None,
    "stop_event": None,
    "encoding": {
        "video": "H.264/H.265",
        "audio": "AAC"
    },
    "rtsp_url": RTSP_URL,
    "resolution": None,
    "bitrate": None,
}

app = Flask(__name__)

def capture_frames(rtsp_url, frame_queue, stop_event):
    cap = cv2.VideoCapture(rtsp_url)
    if not cap.isOpened():
        stop_event.set()
        return
    # Set metadata (resolution)
    streaming_state["resolution"] = {
        "width": int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
        "height": int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    }
    streaming_state["bitrate"] = int(cap.get(cv2.CAP_PROP_BITRATE)) if cap.get(cv2.CAP_PROP_BITRATE) > 0 else None
    while not stop_event.is_set():
        ret, frame = cap.read()
        if not ret:
            continue
        # Encode frame as JPEG
        ret2, jpeg = cv2.imencode('.jpg', frame)
        if not ret2:
            continue
        try:
            frame_queue.put(jpeg.tobytes(), timeout=1)
        except queue.Full:
            pass
    cap.release()

def generate_mjpeg():
    frame_queue = streaming_state["frame_queue"]
    stop_event = streaming_state["stop_event"]
    while streaming_state["active"] and not stop_event.is_set():
        try:
            frame = frame_queue.get(timeout=1)
        except queue.Empty:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
    return

@app.route('/stream', methods=['GET'])
def stream_status():
    filters = {}
    res_filter = request.args.get('resolution')
    bitrate_filter = request.args.get('bitrate')
    result = {
        "streaming": streaming_state["active"],
        "rtsp_url": streaming_state["rtsp_url"] if streaming_state["active"] else None,
        "encoding": streaming_state["encoding"],
        "resolution": streaming_state.get("resolution"),
        "bitrate": streaming_state.get("bitrate"),
    }
    if res_filter and result["resolution"]:
        if res_filter != f'{result["resolution"]["width"]}x{result["resolution"]["height"]}':
            result["streaming"] = False
            result["rtsp_url"] = None
    if bitrate_filter and result["bitrate"]:
        if str(result["bitrate"]) != bitrate_filter:
            result["streaming"] = False
            result["rtsp_url"] = None
    return jsonify(result)

@app.route('/stream/start', methods=['POST'])
def start_stream():
    if streaming_state["active"]:
        return jsonify({"result": "already streaming"}), 200
    if not streaming_state["rtsp_url"]:
        return jsonify({"error": "RTSP_URL not configured"}), 400
    streaming_state["stop_event"] = threading.Event()
    streaming_state["frame_queue"] = queue.Queue(maxsize=10)
    streaming_state["thread"] = threading.Thread(target=capture_frames, args=(streaming_state["rtsp_url"], streaming_state["frame_queue"], streaming_state["stop_event"]))
    streaming_state["thread"].daemon = True
    streaming_state["thread"].start()
    # Wait briefly to ensure the thread starts and connection is successful
    time.sleep(1)
    if streaming_state["stop_event"].is_set():
        streaming_state["active"] = False
        return jsonify({"error": "Failed to connect to RTSP stream"}), 500
    streaming_state["active"] = True
    return jsonify({"result": "stream started", "video_endpoint": f"http://{HTTP_SERVER_HOST}:{HTTP_STREAM_PORT}{HTTP_STREAM_PATH}"}), 200

@app.route('/stream/stop', methods=['POST'])
def stop_stream():
    if not streaming_state["active"]:
        return jsonify({"result": "not streaming"}), 200
    streaming_state["stop_event"].set()
    if streaming_state["thread"]:
        streaming_state["thread"].join(timeout=2)
    streaming_state["active"] = False
    streaming_state["thread"] = None
    streaming_state["frame_queue"] = None
    streaming_state["stop_event"] = None
    return jsonify({"result": "stream stopped"}), 200

@app.route(HTTP_STREAM_PATH, methods=['GET'])
def video_feed():
    if not streaming_state["active"]:
        return Response("Stream not started", status=503)
    return Response(generate_mjpeg(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)