import os
import json
from flask import Flask, request, jsonify
from threading import Lock

app = Flask(__name__)

# Configuration from environment variables
SERVER_HOST = os.environ.get('SHIFU_HTTP_HOST', '0.0.0.0')
SERVER_PORT = int(os.environ.get('SHIFU_HTTP_PORT', '8080'))
DEVICE_NAME = os.environ.get('SHIFU_DEVICE_NAME', 'BACnet HVAC Room Controller')
DEVICE_MODEL = os.environ.get('SHIFU_DEVICE_MODEL', 'SE8000, VT8300')
DEVICE_MANUFACTURER = os.environ.get('SHIFU_DEVICE_MANUFACTURER', 'Schneider Electric, Viconics')
DEVICE_TYPE = os.environ.get('SHIFU_DEVICE_TYPE', 'HVAC room controller')

# Simulated BACnet objects (with thread safety)
class SimulatedBACnet:
    def __init__(self):
        self.lock = Lock()
        # Analog Inputs: temperature values in Celsius
        self.analog_inputs = {
            0: 22.0,  # Indoor temperature
            1: 12.0,  # Outdoor temperature
            2: 45.0   # Hot/warm water temperature
        }
        # Analog Values: setpoints (presets)
        self.analog_values = {
            0: 22.0,  # Active setpoint index (float: 1.0, 2.0, 3.0)
            1: 20.0,  # Setpoint 1 value
            2: 22.0,  # Setpoint 2 value
            3: 24.0   # Setpoint 3 value
        }
        # Character Strings: setpoint descriptions
        self.character_strings = {
            1: "Eco",
            2: "Comfort",
            3: "Boost"
        }
        # Multistate Value: operating mode (1=Stop, 2=Heating, 3=Cooling)
        self.multistate_value = {
            0: 2
        }
        # Out-Of-Service for setpoints
        self.out_of_service = {
            0: False
        }
        # Simulated statuses
        self.status = {
            'ventilation_level': 1,  # 1, 2, 3
            'heater_status': False,
            'chiller_status': False
        }
        self._update_ventilation_and_status()

    def _update_ventilation_and_status(self):
        indoor = self.analog_inputs[0]
        outdoor = self.analog_inputs[1]
        mode = self.multistate_value[0]
        if mode == 2:  # Heating
            diff = indoor - outdoor
            if diff < 4:
                self.status['ventilation_level'] = 1
            elif diff < 8:
                self.status['ventilation_level'] = 2
            else:
                self.status['ventilation_level'] = 3
            self.status['heater_status'] = True
            self.status['chiller_status'] = False
        elif mode == 3:  # Cooling
            diff = indoor - outdoor
            if diff > -10:
                self.status['ventilation_level'] = 1
            elif diff > -20:
                self.status['ventilation_level'] = 2
            else:
                self.status['ventilation_level'] = 3
            self.status['heater_status'] = False
            self.status['chiller_status'] = True
        else:  # Stop
            self.status['ventilation_level'] = 1
            self.status['heater_status'] = False
            self.status['chiller_status'] = False

    def get_temperatures(self):
        with self.lock:
            return {
                'indoor': self.analog_inputs[0],
                'outdoor': self.analog_inputs[1],
                'hot_water': self.analog_inputs[2]
            }

    def get_temperatures_with_unit(self, unit='C'):
        temps = self.get_temperatures()
        if unit.upper() == 'F':
            return {k: round(v * 9/5 + 32, 2) for k, v in temps.items()}
        return {k: round(v, 2) for k, v in temps.items()}

    def get_setpoints(self):
        with self.lock:
            presets = []
            for i in range(1, 4):
                presets.append({
                    'index': i,
                    'value': round(self.analog_values[i], 2),
                    'description': self.character_strings[i]
                })
            active_idx = int(round(self.analog_values[0]))
            active = {'index': active_idx, 'value': round(self.analog_values.get(active_idx, 0.0), 2),
                      'description': self.character_strings.get(active_idx, "")}
            return {
                'presets': presets,
                'active': active,
                'out_of_service': self.out_of_service[0]
            }

    def get_status(self):
        with self.lock:
            self._update_ventilation_and_status()
            mode_map = {1: 'Stop', 2: 'Heating', 3: 'Cooling'}
            return {
                'operating_mode': mode_map.get(self.multistate_value[0], 'Unknown'),
                'ventilation_level': self.status['ventilation_level'],
                'heater_status': self.status['heater_status'],
                'chiller_status': self.status['chiller_status']
            }

    def set_control(self, data):
        with self.lock:
            if 'operating_mode' in data:
                mode_map_rev = {'Stop': 1, 'Heating': 2, 'Cooling': 3}
                mode = data['operating_mode']
                self.multistate_value[0] = mode_map_rev.get(mode, self.multistate_value[0])
            if 'active_setpoint_index' in data:
                idx = int(data['active_setpoint_index'])
                if idx in (1, 2, 3):
                    self.analog_values[0] = float(idx)
            if 'setpoints' in data:
                for s in data['setpoints']:
                    idx = int(s['index'])
                    val = float(s['value'])
                    desc = str(s.get('description', self.character_strings.get(idx, "")))
                    if idx in (1, 2, 3):
                        self.analog_values[idx] = val
                        self.character_strings[idx] = desc
            if 'out_of_service' in data:
                self.out_of_service[0] = bool(data['out_of_service'])
            self._update_ventilation_and_status()

    def get_info(self):
        return {
            'device_name': DEVICE_NAME,
            'device_model': DEVICE_MODEL,
            'manufacturer': DEVICE_MANUFACTURER,
            'device_type': DEVICE_TYPE
        }

    def get_full_data(self):
        return {
            'temperatures': self.get_temperatures(),
            'setpoints': self.get_setpoints(),
            'status': self.get_status(),
            'info': self.get_info()
        }

bacnet = SimulatedBACnet()

@app.route('/api/info', methods=['GET'])
@app.route('/info', methods=['GET'])
def api_info():
    return jsonify(bacnet.get_info())

@app.route('/api/temperature', methods=['GET'])
@app.route('/temp', methods=['GET'])
def api_temperature():
    unit = request.args.get('unit', 'C')
    return jsonify(bacnet.get_temperatures_with_unit(unit))

@app.route('/api/temperature/setpoints', methods=['GET'])
@app.route('/setp', methods=['GET'])
def api_setpoints():
    return jsonify(bacnet.get_setpoints())

@app.route('/api/status', methods=['GET'])
@app.route('/status', methods=['GET'])
def api_status():
    return jsonify(bacnet.get_status())

@app.route('/api/data', methods=['GET'])
def api_data():
    return jsonify(bacnet.get_full_data())

@app.route('/api/control', methods=['POST'])
@app.route('/control', methods=['POST'])
def api_control():
    try:
        data = request.get_json(force=True)
    except Exception:
        return jsonify({'error': 'Invalid JSON payload'}), 400
    bacnet.set_control(data)
    return jsonify({'result': 'success'})

if __name__ == '__main__':
    app.run(host=SERVER_HOST, port=SERVER_PORT)