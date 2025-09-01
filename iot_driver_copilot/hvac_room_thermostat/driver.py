import os
import json
from flask import Flask, request, jsonify, Response
import requests

app = Flask(__name__)

# Environment Variables for Configuration
DEVICE_ID = os.environ.get("DEVICE_ID", "2523161")
DEVICE_IP = os.environ.get("DEVICE_IP", "192.168.2.165")
DEVICE_PORT = os.environ.get("DEVICE_PORT", "49665")
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# Compose base url for device
DEVICE_BASE_URL = f"http://{DEVICE_IP}:{DEVICE_PORT}"

# Helper to proxy GETs to device and return JSON
def proxy_get(endpoint):
    url = f"{DEVICE_BASE_URL}{endpoint}"
    try:
        resp = requests.get(url, timeout=5)
        resp.raise_for_status()
        return jsonify(resp.json())
    except requests.exceptions.RequestException as e:
        return jsonify({"error": str(e)}), 502

# Helper to proxy PUTs to device with JSON payload
def proxy_put(endpoint, json_payload):
    url = f"{DEVICE_BASE_URL}{endpoint}"
    try:
        resp = requests.put(url, json=json_payload, timeout=5)
        resp.raise_for_status()
        return jsonify(resp.json())
    except requests.exceptions.RequestException as e:
        return jsonify({"error": str(e)}), 502

# API: Retrieve current outdoor temperature as JSON
@app.route('/sensors/outdoor', methods=['GET'])
def get_outdoor_temperature():
    return proxy_get('/sensors/outdoor')

# API: Retrieve current indoor temperature as JSON
@app.route('/sensors/indoor', methods=['GET'])
def get_indoor_temperature():
    return proxy_get('/sensors/indoor')

# API: Retrieve current water temperature as JSON
@app.route('/sensors/water', methods=['GET'])
def get_water_temperature():
    return proxy_get('/sensors/water')

# API: Fetch preset temperature values stored in the thermostat
@app.route('/presets', methods=['GET'])
def get_presets():
    return proxy_get('/presets')

# API: Retrieve the current operating mode and status of the thermostat
@app.route('/device/status', methods=['GET'])
def get_device_status():
    return proxy_get('/device/status')

# API: Set or update a preset temperature value on the thermostat
@app.route('/device/preset', methods=['PUT'])
def set_preset_temperature():
    if not request.is_json:
        return jsonify({"error": "JSON payload required"}), 400
    json_payload = request.get_json()
    return proxy_put('/device/preset', json_payload)

# API: Update the operating mode (e.g., heating, cooling, off) of the thermostat
@app.route('/device/mode', methods=['PUT'])
def set_operating_mode():
    if not request.is_json:
        return jsonify({"error": "JSON payload required"}), 400
    json_payload = request.get_json()
    return proxy_put('/device/mode', json_payload)

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT)