import os
import json
from flask import Flask, request, jsonify, abort
import requests

app = Flask(__name__)

# Load configuration from environment variables
DEVICE_HOST = os.environ.get("DEVICE_HOST", "192.168.2.165")
DEVICE_PORT = int(os.environ.get("DEVICE_PORT", "49665"))
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# For demonstration, assume the device exposes an HTTP API compatible with the required operations.
# All requests to the device are proxied and responses are returned in JSON.

DEVICE_BASE_URL = f"http://{DEVICE_HOST}:{DEVICE_PORT}"

def proxy_device_get(path):
    url = f"{DEVICE_BASE_URL}{path}"
    try:
        resp = requests.get(url, timeout=5)
        resp.raise_for_status()
        return jsonify(resp.json())
    except requests.exceptions.RequestException as e:
        return jsonify({"error": str(e)}), 502

def proxy_device_put(path, payload):
    url = f"{DEVICE_BASE_URL}{path}"
    headers = {"Content-Type": "application/json"}
    try:
        resp = requests.put(url, data=json.dumps(payload), headers=headers, timeout=5)
        resp.raise_for_status()
        return jsonify(resp.json())
    except requests.exceptions.RequestException as e:
        return jsonify({"error": str(e)}), 502

@app.route("/device/preset", methods=["PUT"])
def set_preset_temperature():
    if not request.is_json:
        abort(400, description="Payload must be JSON")
    payload = request.get_json()
    # Proxy to device endpoint
    return proxy_device_put("/api/preset", payload)

@app.route("/sensors/outdoor", methods=["GET"])
def get_outdoor_temperature():
    return proxy_device_get("/api/sensors/outdoor")

@app.route("/sensors/indoor", methods=["GET"])
def get_indoor_temperature():
    return proxy_device_get("/api/sensors/indoor")

@app.route("/sensors/water", methods=["GET"])
def get_water_temperature():
    return proxy_device_get("/api/sensors/water")

@app.route("/presets", methods=["GET"])
def get_presets():
    return proxy_device_get("/api/presets")

@app.route("/device/status", methods=["GET"])
def get_device_status():
    return proxy_device_get("/api/status")

@app.route("/device/mode", methods=["PUT"])
def set_device_mode():
    if not request.is_json:
        abort(400, description="Payload must be JSON")
    payload = request.get_json()
    # Proxy to device endpoint
    return proxy_device_put("/api/mode", payload)

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT)