#!/usr/bin/env python3
"""
HVAC DeviceShifu - Flask-based BACnet device control API
"""

from flask import Flask, request, jsonify
import asyncio
import threading
import time
import os
import BAC0

# Configure BAC0
BAC0.log_level('error')
os.environ.update({
    'BAC0_POLLING_ENABLED': '0',
    'BAC0_COV_ENABLED': '0',
    'BAC0_AUTO_START': '0'
})

class HVACController:
    """HVAC Controller"""

    def __init__(self, local_ip="192.168.2.109/24", device_address="192.168.2.165:49665", device_id=2523161):
        self.local_ip = local_ip
        self.device_address = device_address
        self.device_id = device_id
        self.bacnet = None
        self.device = None

    async def connect(self):
        """Connect to BACnet network and device"""
        try:
            from BAC0.scripts.Lite import Lite
            self.bacnet = await Lite(ip=self.local_ip, port=47808, deviceId=0)
            self.device = await BAC0.device(self.device_address, self.device_id, self.bacnet, poll=False)
            return True
        except Exception:
            return False

    async def _read_value(self, obj_type, obj_id):
        """Helper method to read BACnet value"""
        coro = self.bacnet.read(f"{self.device_address} {obj_type} {obj_id} presentValue")
        value = await coro if hasattr(coro, '__await__') else coro
        return f"{value:.2f}" if isinstance(value, (int, float)) else str(value)

    async def read_all_data(self):
        """Read all device data"""
        try:
            data = {
                'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
                'device_info': {
                    'address': self.device_address,
                    'id': self.device_id,
                    'name': 'RoomController.Simulator'
                },
                'temperatures': {
                    'analogInput 0': await self._read_value('analogInput', 0),
                    'analogInput 1': await self._read_value('analogInput', 1),
                    'analogInput 2': await self._read_value('analogInput', 2)
                },
                'setpoints': {
                    'analogValue 0': await self._read_value('analogValue', 0),
                    'analogValue 1': await self._read_value('analogValue', 1),
                    'analogValue 2': await self._read_value('analogValue', 2),
                    'analogValue 3': await self._read_value('analogValue', 3)
                },
                'control_states': {
                    'controller_state': await self._read_value('multiStateValue', 0),
                    'ventilation_level': await self._read_value('multiStateValue', 1),
                    'heater_state': await self._read_value('binaryValue', 0),
                    'cooler_state': await self._read_value('binaryValue', 1)
                }
            }

            coro = self.bacnet.read(f"{self.device_address} characterstringValue 1 presentValue")
            data['setpoint_texts'] = await coro if hasattr(coro, '__await__') else coro
            return data
        except Exception as e:
            return {'error': str(e)}

    async def read_device_info(self):
        """Read device information"""
        return {
            'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
            'device_info': {
                'address': self.device_address,
                'id': self.device_id,
                'name': 'RoomController.Simulator'
            }
        }

    async def read_temperature_sensors(self):
        """Read temperature sensor data"""
        try:
            return {
                'analogInput 0': await self._read_value('analogInput', 0),
                'analogInput 1': await self._read_value('analogInput', 1),
                'analogInput 2': await self._read_value('analogInput', 2)
            }
        except Exception as e:
            return {'error': str(e)}

    async def read_temperature_setpoints(self):
        """Read temperature setpoint data"""
        try:
            return {
                'analogValue 0': await self._read_value('analogValue', 0),
                'analogValue 1': await self._read_value('analogValue', 1),
                'analogValue 2': await self._read_value('analogValue', 2),
                'analogValue 3': await self._read_value('analogValue', 3)
            }
        except Exception as e:
            return {'error': str(e)}

    async def read_control_status(self):
        """Read control status data"""
        try:
            return {
                'controller_state': await self._read_value('multiStateValue', 0),
                'ventilation_level': await self._read_value('multiStateValue', 1),
                'heater_state': await self._read_value('binaryValue', 0),
                'cooler_state': await self._read_value('binaryValue', 1)
            }
        except Exception as e:
            return {'error': str(e)}

    async def _write_value(self, obj_type, obj_id, value):
        """Helper method to write BACnet value"""
        coro = self.bacnet.write(f"{self.device_address} {obj_type} {obj_id} presentValue {value}")
        if hasattr(coro, '__await__'):
            await coro

    async def control_device(self, command, value=None):
        """Control device"""
        try:
            if command == 'set_setpoint':
                await self._write_value('analogValue', 0, value)
            elif command == 'set_mode' and value in [1, 2, 3]:
                await self._write_value('multiStateValue', 0, value)
            elif command == 'set_ventilation' and value in [1, 2, 3, 4]:
                await self._write_value('multiStateValue', 1, value)
            elif command == 'override_setpoint':
                coro = self.bacnet.write(f"{self.device_address} analogValue 0 outOfService true")
                if hasattr(coro, '__await__'):
                    await coro
                await self._write_value('analogValue', 0, value)
            return True
        except Exception:
            return False

    async def disconnect(self):
        """Disconnect"""
        if self.bacnet:
            coro = self.bacnet.disconnect()
            if hasattr(coro, '__await__'):
                await coro

app = Flask(__name__)

# Configure Flask JSON encoding
app.config['JSON_AS_ASCII'] = False
app.json.ensure_ascii = False

# Global variables
hvac_controller = None
connection_status = {"connected": False, "message": "Not connected"}
loop = None
loop_thread = None

def run_event_loop():
    """Run event loop in a separate thread"""
    global loop
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_forever()



def run_async_task(coro):
    """Run async task in event loop"""
    if loop is None or not loop.is_running():
        return {"error": "Event loop not running"}

    future = asyncio.run_coroutine_threadsafe(coro, loop)
    try:
        return future.result(timeout=30)
    except Exception as e:
        return {"error": str(e)}

def check_connection():
    """Check if device is connected"""
    if not hvac_controller or not connection_status["connected"]:
        return False
    return True



@app.route('/api/data', methods=['GET'])
def read_data():
    """Read device data"""
    if not check_connection():
        return jsonify({"success": False, "error": "Not connected"}), 400

    result = run_async_task(hvac_controller.read_all_data())
    if isinstance(result, dict) and "error" in result:
        return jsonify({"success": False, "error": result["error"]}), 500
    return jsonify({"success": True, "data": result})

@app.route('/api/info', methods=['GET'])
def read_device_info():
    """Read device information"""
    if not check_connection():
        return jsonify({"success": False, "error": "Not connected"}), 400

    result = run_async_task(hvac_controller.read_device_info())
    return jsonify({"success": True, "data": result})

@app.route('/api/temperature', methods=['GET'])
def read_temperature_sensors():
    """Read temperature sensor data"""
    if not check_connection():
        return jsonify({"success": False, "error": "Not connected"}), 400

    result = run_async_task(hvac_controller.read_temperature_sensors())
    if isinstance(result, dict) and "error" in result:
        return jsonify({"success": False, "error": result["error"]}), 500
    return jsonify({"success": True, "data": result})

@app.route('/api/temperature/setpoints', methods=['GET'])
def read_temperature_setpoints():
    """Read temperature setpoint data"""
    if not check_connection():
        return jsonify({"success": False, "error": "Not connected"}), 400

    result = run_async_task(hvac_controller.read_temperature_setpoints())
    if isinstance(result, dict) and "error" in result:
        return jsonify({"success": False, "error": result["error"]}), 500
    return jsonify({"success": True, "data": result})

@app.route('/api/status', methods=['GET'])
def read_control_status():
    """Read control status data"""
    if not check_connection():
        return jsonify({"success": False, "error": "Not connected"}), 400

    result = run_async_task(hvac_controller.read_control_status())
    if isinstance(result, dict) and "error" in result:
        return jsonify({"success": False, "error": result["error"]}), 500
    return jsonify({"success": True, "data": result})

@app.route('/api/control', methods=['POST'])
def control_device():
    """Control device"""
    if not check_connection():
        return jsonify({"success": False, "error": "Not connected"}), 400

    data = request.get_json()
    if not data or 'command' not in data:
        return jsonify({"success": False, "error": "Missing command"}), 400

    command = data['command']
    value = data.get('value')

    # Validate command
    valid_commands = ['set_setpoint', 'set_mode', 'set_ventilation', 'override_setpoint']
    if command not in valid_commands:
        return jsonify({"success": False, "error": "Invalid command"}), 400

    # Validate value
    if command in ['set_setpoint', 'override_setpoint']:
        try:
            float(value)
        except (ValueError, TypeError):
            return jsonify({"success": False, "error": "Invalid temperature value"}), 400
    elif command == 'set_mode' and value not in [1, 2, 3]:
        return jsonify({"success": False, "error": "Invalid mode value"}), 400
    elif command == 'set_ventilation' and value not in [1, 2, 3, 4]:
        return jsonify({"success": False, "error": "Invalid ventilation value"}), 400

    result = run_async_task(hvac_controller.control_device(command, value))
    if result:
        return jsonify({"success": True, "message": f"Command {command} executed"})
    return jsonify({"success": False, "error": "Command failed"}), 500

@app.errorhandler(404)
def not_found(error):
    return jsonify({"success": False, "error": "Endpoint not found"}), 404

@app.errorhandler(405)
def method_not_allowed(error):
    return jsonify({"success": False, "error": "Method not allowed"}), 405

@app.errorhandler(500)
def internal_error(error):
    return jsonify({"success": False, "error": "Internal server error"}), 500

if __name__ == '__main__':
    print("HVAC DeviceShifu API Server starting...")

    # Start event loop thread
    loop_thread = threading.Thread(target=run_event_loop, daemon=True)
    loop_thread.start()
    time.sleep(0.5)

    try:
        app.run(host='0.0.0.0', port=8081, debug=False, threaded=True)
    except KeyboardInterrupt:
        if loop:
            loop.call_soon_threadsafe(loop.stop)
        print("Server stopped")
