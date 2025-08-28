import os
import threading
import time
from flask import Flask, jsonify, request, abort
from bacpypes.core import run, stop, enable_sleeping
from bacpypes.app import BIPSimpleApplication
from bacpypes.object import (
    AnalogInputObject, AnalogValueObject,
    CharacterStringValueObject
)
from bacpypes.local.device import LocalDeviceObject
from bacpypes.pdu import Address
from bacpypes.apdu import (
    ReadPropertyRequest, WritePropertyRequest, SimpleAckPDU,
    Error, AbortPDU, RejectPDU
)
from bacpypes.primitivedata import Real, CharacterString, Boolean, Unsigned
from bacpypes.constructeddata import Array, Any
from bacpypes.basetypes import PropertyIdentifier
from bacpypes.service.device import WhoIsIAmServices

# ========== ENVIRONMENT VARIABLES ==========
DEVICE_IP = os.environ.get("DEVICE_IP", "192.168.1.20")
DEVICE_BACNET_ID = int(os.environ.get("DEVICE_BACNET_ID", "8001"))
DEVICE_BACNET_PORT = int(os.environ.get("DEVICE_BACNET_PORT", "47808"))
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

# ========== BACNET DEVICES/OBJECTS SETUP ==========
class HVACApplication(BIPSimpleApplication, WhoIsIAmServices):
    pass

# Device Info
DEVICE_NAME = "HVAC Room Controller"
DEVICE_MODEL = "SE8000 series, VT8300"
DEVICE_MANUFACTURER = "Schneider Electric, Viconics"
DEVICE_TYPE = "BACnet HVAC room controller"

# BACnet Device Object
bacnet_device = LocalDeviceObject(
    objectName=DEVICE_NAME,
    objectIdentifier=("device", DEVICE_BACNET_ID),
    maxApduLengthAccepted=1024,
    segmentationSupported="segmentedBoth",
    vendorIdentifier=15,  # 15 = Schneider Electric
    modelName=DEVICE_MODEL
)

# BACnet Objects
indoor_temp_ai = AnalogInputObject(
    objectIdentifier=("analogInput", 1),
    objectName="Indoor Temperature",
    presentValue=22.5,
    units=62,  # degreesCelsius
)
outdoor_temp_ai = AnalogInputObject(
    objectIdentifier=("analogInput", 2),
    objectName="Outdoor Temperature",
    presentValue=10.0,
    units=62,
)
water_temp_ai = AnalogInputObject(
    objectIdentifier=("analogInput", 3),
    objectName="Hot/Water Temperature",
    presentValue=45.7,
    units=62,
)

setpoint1_av = AnalogValueObject(
    objectIdentifier=("analogValue", 1),
    objectName="Preset Setpoint 1",
    presentValue=19.0,
)
setpoint2_av = AnalogValueObject(
    objectIdentifier=("analogValue", 2),
    objectName="Preset Setpoint 2",
    presentValue=22.0,
)
setpoint3_av = AnalogValueObject(
    objectIdentifier=("analogValue", 3),
    objectName="Preset Setpoint 3",
    presentValue=25.0,
)

setpoint1_desc = CharacterStringValueObject(
    objectIdentifier=("characterStringValue", 1),
    objectName="Setpoint 1 Description",
    presentValue="Eco Mode"
)
setpoint2_desc = CharacterStringValueObject(
    objectIdentifier=("characterStringValue", 2),
    objectName="Setpoint 2 Description",
    presentValue="Comfort Mode"
)
setpoint3_desc = CharacterStringValueObject(
    objectIdentifier=("characterStringValue", 3),
    objectName="Setpoint 3 Description",
    presentValue="Boost Mode"
)

active_setpoint_av = AnalogValueObject(
    objectIdentifier=("analogValue", 0),
    objectName="Current Active Setpoint Index",
    presentValue=1,  # 1, 2, or 3
)

# Out_Of_Service for override simulation
active_setpoint_av.outOfService = False

# Add objects to device
bacnet_device.objectList = [
    indoor_temp_ai, outdoor_temp_ai, water_temp_ai,
    setpoint1_av, setpoint2_av, setpoint3_av,
    setpoint1_desc, setpoint2_desc, setpoint3_desc,
    active_setpoint_av
]

bacnet_app = HVACApplication(bacnet_device, DEVICE_IP + "/" + str(DEVICE_BACNET_PORT))

# ========== BACNET STATE MANAGEMENT ==========
def simulate_sensor_updates():
    while True:
        # Simulate indoor, outdoor, and water temperature changes
        indoor_temp_ai.presentValue += (0.05 if time.time() % 10 < 5 else -0.05)
        outdoor_temp_ai.presentValue += (0.02 if time.time() % 17 < 8 else -0.02)
        water_temp_ai.presentValue += (0.03 if time.time() % 13 < 6 else -0.03)
        time.sleep(2)

def bacnet_run_loop():
    enable_sleeping()
    run()

# ========== HTTP SERVER ==========
app = Flask(__name__)

# ========== API ENDPOINTS ==========

@app.route('/api/info', methods=['GET'])
def api_info():
    return jsonify({
        "device_name": DEVICE_NAME,
        "device_model": DEVICE_MODEL,
        "manufacturer": DEVICE_MANUFACTURER,
        "device_type": DEVICE_TYPE,
        "bacnet_device_id": DEVICE_BACNET_ID,
        "bacnet_ip": DEVICE_IP,
        "bacnet_port": DEVICE_BACNET_PORT
    })

@app.route('/api/status', methods=['GET'])
def api_status():
    # Out_Of_Service and control state
    out_of_service = bool(active_setpoint_av.outOfService)
    return jsonify({
        "out_of_service": out_of_service,
        "active_setpoint_index": int(active_setpoint_av.presentValue),
        "control_state": "remote_override" if out_of_service else "normal",
        "network_status": "online"
    })

@app.route('/api/temperature', methods=['GET'])
def api_temperature():
    unit = request.args.get('unit', 'C').upper()
    def convert(temp):
        if unit == "F":
            return round(temp * 9.0 / 5.0 + 32, 2)
        return round(temp, 2)
    return jsonify({
        "unit": "Fahrenheit" if unit == "F" else "Celsius",
        "indoor": convert(indoor_temp_ai.presentValue),
        "outdoor": convert(outdoor_temp_ai.presentValue),
        "hot_water": convert(water_temp_ai.presentValue)
    })

@app.route('/api/temperature/setpoints', methods=['GET'])
def api_temperature_setpoints():
    setpoints = [
        {
            "index": 1,
            "value": setpoint1_av.presentValue,
            "description": setpoint1_desc.presentValue
        },
        {
            "index": 2,
            "value": setpoint2_av.presentValue,
            "description": setpoint2_desc.presentValue
        },
        {
            "index": 3,
            "value": setpoint3_av.presentValue,
            "description": setpoint3_desc.presentValue
        }
    ]
    current_active = int(active_setpoint_av.presentValue)
    return jsonify({
        "setpoints": setpoints,
        "active_setpoint_index": current_active
    })

@app.route('/api/data', methods=['GET'])
def api_data():
    unit = request.args.get('unit', 'C').upper()
    def convert(temp):
        if unit == "F":
            return round(temp * 9.0 / 5.0 + 32, 2)
        return round(temp, 2)
    setpoints = [
        {
            "index": 1,
            "value": setpoint1_av.presentValue,
            "description": setpoint1_desc.presentValue
        },
        {
            "index": 2,
            "value": setpoint2_av.presentValue,
            "description": setpoint2_desc.presentValue
        },
        {
            "index": 3,
            "value": setpoint3_av.presentValue,
            "description": setpoint3_desc.presentValue
        }
    ]
    current_active = int(active_setpoint_av.presentValue)
    return jsonify({
        "device_info": {
            "device_name": DEVICE_NAME,
            "device_model": DEVICE_MODEL,
            "manufacturer": DEVICE_MANUFACTURER,
            "device_type": DEVICE_TYPE,
            "bacnet_device_id": DEVICE_BACNET_ID,
            "bacnet_ip": DEVICE_IP,
            "bacnet_port": DEVICE_BACNET_PORT
        },
        "temperatures": {
            "unit": "Fahrenheit" if unit == "F" else "Celsius",
            "indoor": convert(indoor_temp_ai.presentValue),
            "outdoor": convert(outdoor_temp_ai.presentValue),
            "hot_water": convert(water_temp_ai.presentValue)
        },
        "setpoints": setpoints,
        "active_setpoint_index": current_active,
        "out_of_service": bool(active_setpoint_av.outOfService)
    })

@app.route('/api/control', methods=['POST'])
def api_control():
    payload = request.get_json(force=True)
    resp = {}
    # Override setpoint selection
    if "active_setpoint_index" in payload:
        idx = int(payload["active_setpoint_index"])
        if idx in [1,2,3]:
            active_setpoint_av.presentValue = idx
            resp["active_setpoint_index"] = idx
        else:
            abort(400, "active_setpoint_index must be 1, 2, or 3")
    # Override Out_Of_Service
    if "out_of_service" in payload:
        out_of_service = bool(payload["out_of_service"])
        active_setpoint_av.outOfService = out_of_service
        resp["out_of_service"] = out_of_service
    # Simulate button press
    if "lcd_button" in payload:
        btn = payload["lcd_button"]
        # For demo: "up" increases setpoint idx, "down" decreases
        idx = int(active_setpoint_av.presentValue)
        if btn == "up" and idx < 3:
            active_setpoint_av.presentValue = idx + 1
        elif btn == "down" and idx > 1:
            active_setpoint_av.presentValue = idx - 1
        resp["active_setpoint_index"] = int(active_setpoint_av.presentValue)
    # Set individual setpoints
    for i in [1,2,3]:
        key = f"setpoint{i}_value"
        if key in payload:
            val = float(payload[key])
            if i == 1: setpoint1_av.presentValue = val
            if i == 2: setpoint2_av.presentValue = val
            if i == 3: setpoint3_av.presentValue = val
            resp[key] = val
        key = f"setpoint{i}_description"
        if key in payload:
            desc = str(payload[key])
            if i == 1: setpoint1_desc.presentValue = desc
            if i == 2: setpoint2_desc.presentValue = desc
            if i == 3: setpoint3_desc.presentValue = desc
            resp[key] = desc
    return jsonify(resp)

# ========== STARTUP ==========
def start_bacnet():
    threading.Thread(target=bacnet_run_loop, daemon=True).start()
    threading.Thread(target=simulate_sensor_updates, daemon=True).start()

if __name__ == '__main__':
    start_bacnet()
    app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)