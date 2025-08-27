import os
import json
from flask import Flask, request, Response, jsonify
import requests

app = Flask(__name__)

DEVICE_HOST = os.environ.get("DEVICE_HOST", "192.168.2.165")
DEVICE_PORT = int(os.environ.get("DEVICE_PORT", "49665"))
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))
TIMEOUT = float(os.environ.get("DEVICE_HTTP_TIMEOUT", "5"))

def device_base_url():
    return f"http://{DEVICE_HOST}:{DEVICE_PORT}"

@app.route("/api/data", methods=["GET"])
def api_data():
    url = f"{device_base_url()}/api/data"
    try:
        resp = requests.get(url, timeout=TIMEOUT)
        resp.raise_for_status()
        # Ensure content is JSON
        return Response(resp.content, status=resp.status_code, mimetype="application/json")
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502

@app.route("/api/control", methods=["POST"])
def api_control():
    url = f"{device_base_url()}/api/control"
    # Content-Type must be application/json
    if not request.is_json:
        return jsonify({"error": "Request body must be JSON"}), 400
    try:
        resp = requests.post(
            url,
            json=request.get_json(force=True),
            timeout=TIMEOUT,
            headers={'Content-Type': 'application/json'}
        )
        resp.raise_for_status()
        # Return whatever the device returns, defaulting to JSON
        return Response(resp.content, status=resp.status_code, mimetype="application/json")
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502

@app.route('/')
def index():
    return jsonify({
        "device": "BACnet-based HVAC Controller",
        "routes": [
            {"method": "GET", "path": "/api/data", "description": "Get device status data"},
            {"method": "POST", "path": "/api/control", "description": "Send control commands to device"}
        ]
    })

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT)