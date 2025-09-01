import os
import json
from flask import Flask, request, jsonify, abort

app = Flask(__name__)

# Load configuration from environment variables
DEVICE_ID = os.environ.get('DEVICE_ID', '2523161')
DEVICE_ADDRESS = os.environ.get('DEVICE_ADDRESS', '192.168.2.165:49665')
LOCAL_INTERFACE = os.environ.get('LOCAL_INTERFACE', '192.168.2.100/24')
SERVER_HOST = os.environ.get('SERVER_HOST', '0.0.0.0')
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))

# Simulated in-memory device state
device_state = {
    'sensors': {
        'indoor': 23.2,
        'outdoor': 18.6,
        'water': 45.1
    },
    'presets': {
        'preset': 22.0
    },
    'status': {
        'mode': 'auto',
        'device_status': 'on'
    }
}

@app.route('/sensors', methods=['GET'])
def get_sensors():
    sensor_type = request.args.get('type')
    sensors = device_state['sensors']
    if sensor_type:
        value = sensors.get(sensor_type)
        if value is None:
            return jsonify({'error': f'Unknown sensor type: {sensor_type}'}), 400
        return jsonify({sensor_type: value})
    return jsonify(sensors)

@app.route('/status', methods=['GET'])
def get_status():
    return jsonify(device_state['status'])

@app.route('/presets', methods=['GET'])
def get_presets():
    return jsonify(device_state['presets'])

@app.route('/commands/mode', methods=['POST'])
def set_mode():
    if not request.is_json:
        abort(400, 'Request body must be JSON')
    data = request.get_json()
    mode = data.get('mode')
    if mode not in ['cooling', 'heating', 'auto', 'off']:
        return jsonify({'error': 'Invalid mode'}), 400
    device_state['status']['mode'] = mode
    return jsonify({'result': 'Mode updated', 'mode': mode})

@app.route('/commands/preset', methods=['POST'])
def set_preset():
    if not request.is_json:
        abort(400, 'Request body must be JSON')
    data = request.get_json()
    preset = data.get('preset')
    try:
        preset = float(preset)
    except (TypeError, ValueError):
        return jsonify({'error': 'Invalid preset value'}), 400
    device_state['presets']['preset'] = preset
    return jsonify({'result': 'Preset updated', 'preset': preset})

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT)