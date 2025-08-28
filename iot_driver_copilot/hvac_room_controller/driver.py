import os
import json
from flask import Flask, request, jsonify, abort

app = Flask(__name__)

# Environment Variables (all required)
BACNET_DEVICE_IP = os.environ.get("BACNET_DEVICE_IP", "127.0.0.1")
BACNET_DEVICE_PORT = int(os.environ.get("BACNET_DEVICE_PORT", 47808))
SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", 8080))

# Simulated Device Data
device_info = {
    "name": "HVAC Room Controller",
    "model": "SE8000 series, VT8300",
    "manufacturer": "Schneider Electric, Viconics",
    "type": "Room Temperature Controller"
}

# Initial Values (simulate BACnet objects)
# Temperature: Celsius by default
indoor_temp_c = 22.5
outdoor_temp_c = 16.0
water_temp_c = 45.0
temperature_unit = "C"  # "C" or "F"

# Setpoints and their descriptions
setpoints = [
    {"value": 20.0, "description": "Energy Saving"},
    {"value": 22.0, "description": "Comfort"},
    {"value": 24.0, "description": "Boost"}
]

active_setpoint_index = 1  # 0-based index

# Operating Modes
operating_modes = ["Stop", "Heating", "Cooling"]
operating_mode_index = 0  # Default: Stop

# Device statuses
ventilation_levels = ["Low", "Medium", "High"]
ventilation_level_index = 0

heater_status = False
chiller_status = False

# Helper Functions

def c_to_f(c):
    return round(c * 9.0 / 5.0 + 32.0, 1)

def f_to_c(f):
    return round((f - 32.0) * 5.0 / 9.0, 1)

def get_temperature_data(unit="C"):
    if unit == "F":
        return {
            "indoor": c_to_f(indoor_temp_c),
            "outdoor": c_to_f(outdoor_temp_c),
            "water": c_to_f(water_temp_c),
            "unit": "F"
        }
    else:
        return {
            "indoor": indoor_temp_c,
            "outdoor": outdoor_temp_c,
            "water": water_temp_c,
            "unit": "C"
        }

def get_setpoint_data():
    data = []
    for idx, sp in enumerate(setpoints):
        data.append({
            "index": idx,
            "value": sp["value"],
            "description": sp["description"],
            "active": idx == active_setpoint_index
        })
    return data

def update_ventilation_level():
    global ventilation_level_index
    # Heating: ventilation based on |indoor - outdoor| < 12C
    # Cooling: ventilation based on |indoor - outdoor| > 30C
    diff = abs(indoor_temp_c - outdoor_temp_c)
    if operating_mode_index == 1:  # Heating
        if diff < 4:
            ventilation_level_index = 0
        elif diff < 8:
            ventilation_level_index = 1
        else:
            ventilation_level_index = 2
    elif operating_mode_index == 2:  # Cooling
        if diff < 10:
            ventilation_level_index = 0
        elif diff < 20:
            ventilation_level_index = 1
        else:
            ventilation_level_index = 2
    else:  # Stop
        ventilation_level_index = 0

def update_heater_chiller_status():
    global heater_status, chiller_status
    if operating_mode_index == 1:  # Heating
        heater_status = True
        chiller_status = False
    elif operating_mode_index == 2:  # Cooling
        heater_status = False
        chiller_status = True
    else:
        heater_status = False
        chiller_status = False

def get_status_data():
    update_ventilation_level()
    update_heater_chiller_status()
    return {
        "operating_mode": operating_modes[operating_mode_index],
        "ventilation_level_status": ventilation_levels[ventilation_level_index],
        "heater_status": heater_status,
        "chiller_status": chiller_status
    }

def get_comprehensive_data(unit="C"):
    return {
        "temperature": get_temperature_data(unit),
        "setpoints": get_setpoint_data(),
        "active_setpoint": setpoints[active_setpoint_index],
        "status": get_status_data()
    }

# HTTP API Endpoints

@app.route("/api/info", methods=["GET"])
def api_info():
    return jsonify(device_info)

@app.route("/api/temperature", methods=["GET"])
def api_temperature():
    unit = request.args.get("unit", temperature_unit).upper()
    if unit not in ["C", "F"]:
        return jsonify({"error": "Invalid unit"}), 400
    return jsonify(get_temperature_data(unit))

@app.route("/api/temperature/setpoints", methods=["GET"])
def api_setpoints():
    # Optionally allow filtering, e.g., ?active=true
    active = request.args.get("active", None)
    data = get_setpoint_data()
    if active is not None:
        if active.lower() in ["true", "1", "yes"]:
            data = [sp for sp in data if sp["active"]]
        else:
            data = [sp for sp in data if not sp["active"]]
    return jsonify(data)

@app.route("/api/status", methods=["GET"])
def api_status():
    return jsonify(get_status_data())

@app.route("/api/data", methods=["GET"])
def api_data():
    # Supports pagination and filtering (simulate)
    unit = request.args.get("unit", temperature_unit).upper()
    page = int(request.args.get("page", 1))
    page_size = int(request.args.get("page_size", 100))
    data = get_comprehensive_data(unit)
    # For demonstration, simulate paginated setpoints
    setpoints_data = data["setpoints"]
    start = (page - 1) * page_size
    end = start + page_size
    data["setpoints"] = setpoints_data[start:end]
    return jsonify(data)

@app.route("/api/control", methods=["POST"])
def api_control():
    # Accepts JSON payload
    if not request.is_json:
        return jsonify({"error": "Expected JSON payload"}), 400
    payload = request.get_json()
    errors = []

    global active_setpoint_index, operating_mode_index, indoor_temp_c, outdoor_temp_c, water_temp_c

    # Set active setpoint (by index or value)
    if "active_setpoint_index" in payload:
        idx = payload["active_setpoint_index"]
        if not isinstance(idx, int) or not (0 <= idx < len(setpoints)):
            errors.append("active_setpoint_index out of range")
        else:
            active_setpoint_index = idx
    elif "active_setpoint_value" in payload:
        val = payload["active_setpoint_value"]
        idx = next((i for i, sp in enumerate(setpoints) if sp["value"] == val), None)
        if idx is None:
            errors.append("active_setpoint_value not found")
        else:
            active_setpoint_index = idx

    # Change operating mode
    if "operating_mode" in payload:
        mode = payload["operating_mode"]
        if isinstance(mode, int):
            if 0 <= mode < len(operating_modes):
                operating_mode_index = mode
            else:
                errors.append("operating_mode index out of range")
        elif isinstance(mode, str):
            mode = mode.capitalize()
            if mode in operating_modes:
                operating_mode_index = operating_modes.index(mode)
            else:
                errors.append("operating_mode not recognized")
        else:
            errors.append("operating_mode format invalid")

    # Override setpoint values
    if "setpoints" in payload and isinstance(payload["setpoints"], list):
        for i, sp in enumerate(payload["setpoints"]):
            if i >= len(setpoints):
                continue
            if "value" in sp and isinstance(sp["value"], (int, float)):
                setpoints[i]["value"] = float(sp["value"])
            if "description" in sp and isinstance(sp["description"], str):
                setpoints[i]["description"] = sp["description"]

    # Simulate temperature override (test hook)
    if "indoor_temp" in payload:
        try:
            indoor_temp_c = float(payload["indoor_temp"])
        except Exception:
            errors.append("Invalid indoor_temp")
    if "outdoor_temp" in payload:
        try:
            outdoor_temp_c = float(payload["outdoor_temp"])
        except Exception:
            errors.append("Invalid outdoor_temp")
    if "water_temp" in payload:
        try:
            water_temp_c = float(payload["water_temp"])
        except Exception:
            errors.append("Invalid water_temp")

    if errors:
        return jsonify({"success": False, "errors": errors}), 400
    return jsonify({"success": True, "active_setpoint_index": active_setpoint_index,
                    "operating_mode": operating_modes[operating_mode_index]})

if __name__ == "__main__":
    app.run(host=SERVER_HOST, port=SERVER_PORT)