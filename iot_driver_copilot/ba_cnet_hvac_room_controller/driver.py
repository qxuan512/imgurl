import os
import json
from typing import Dict, Any
from fastapi import FastAPI, Request, HTTPException, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel

# Environment variable config
DEVICE_NAME = os.getenv("DEVICE_NAME", "BACnet HVAC Room Controller")
DEVICE_MODEL = os.getenv("DEVICE_MODEL", "SE8000 series, VT8300")
DEVICE_MANUFACTURER = os.getenv("DEVICE_MANUFACTURER", "Schneider Electric, Viconics")
DEVICE_TYPE = os.getenv("DEVICE_TYPE", "Room Temperature Controller")

SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.getenv("SERVER_PORT", "8080"))

# Simulation: device state
class BACnetHVACSim:
    def __init__(self):
        # Temperatures in Celsius by default
        self.temperature_unit = "C"
        self.indoor_temp = 22.0
        self.outdoor_temp = 10.0
        self.water_temp = 45.0
        # Preset setpoints
        self.setpoints = [
            {"id": 1, "value": 20.0, "desc": "Eco"},
            {"id": 2, "value": 22.0, "desc": "Comfort"},
            {"id": 3, "value": 24.0, "desc": "Boost"},
        ]
        self.active_setpoint_index = 1  # 0-based: default "Comfort"
        self.operating_mode = 1  # 0: Stop, 1: Heating, 2: Cooling
        self.ventilation_level = 1  # 0: Low, 1: Medium, 2: High
        self.controller_status = "Normal"
        self.heater_status = False
        self.chiller_status = False

    def get_temperature_data(self):
        return {
            "indoor_temperature": self._convert_temp(self.indoor_temp),
            "outdoor_temperature": self._convert_temp(self.outdoor_temp),
            "water_temperature": self._convert_temp(self.water_temp),
            "temperature_unit": self.temperature_unit
        }

    def get_setpoints_data(self):
        setpoints = [
            {
                "id": s["id"],
                "value": self._convert_temp(s["value"]),
                "desc": s["desc"]
            }
            for s in self.setpoints
        ]
        return {
            "setpoints": setpoints,
            "active_setpoint": {
                "id": self.setpoints[self.active_setpoint_index]["id"],
                "value": self._convert_temp(self.setpoints[self.active_setpoint_index]["value"]),
                "desc": self.setpoints[self.active_setpoint_index]["desc"]
            }
        }

    def get_status(self):
        return {
            "operating_mode": ["Stop", "Heating", "Cooling"][self.operating_mode],
            "ventilation_level": ["Low", "Medium", "High"][self.ventilation_level],
            "controller_status": self.controller_status,
            "heater_status": self.heater_status,
            "chiller_status": self.chiller_status
        }

    def get_comprehensive_data(self):
        data = {}
        data.update(self.get_temperature_data())
        data.update(self.get_setpoints_data())
        data.update(self.get_status())
        return data

    def set_control(self, payload: Dict[str, Any]):
        # Supported keys: operating_mode, setpoint_id, setpoint_value, ventilation_level, temperature_unit
        if "operating_mode" in payload:
            value = payload["operating_mode"]
            if value in [0, 1, 2]:
                self.operating_mode = value
            elif isinstance(value, str) and value.lower() in ["stop", "heating", "cooling"]:
                self.operating_mode = ["stop", "heating", "cooling"].index(value.lower())
            else:
                raise ValueError("Invalid operating_mode")
        if "setpoint_id" in payload:
            idx = next((i for i, s in enumerate(self.setpoints) if s["id"] == payload["setpoint_id"]), None)
            if idx is None:
                raise ValueError("Invalid setpoint_id")
            self.active_setpoint_index = idx
        if "setpoint_value" in payload:
            idx = self.active_setpoint_index
            val = payload["setpoint_value"]
            if self.temperature_unit == "F":
                # store internally as Celsius
                val = (val - 32) * 5.0 / 9.0
            self.setpoints[idx]["value"] = float(val)
        if "ventilation_level" in payload:
            val = payload["ventilation_level"]
            if val in [0, 1, 2]:
                self.ventilation_level = val
            elif isinstance(val, str) and val.lower() in ["low", "medium", "high"]:
                self.ventilation_level = ["low", "medium", "high"].index(val.lower())
            else:
                raise ValueError("Invalid ventilation_level")
        if "temperature_unit" in payload:
            val = payload["temperature_unit"]
            if val.upper() in ["C", "F"]:
                self.set_temperature_unit(val.upper())
            else:
                raise ValueError("Invalid temperature_unit")
        # Simulate heater/chiller status
        self.update_heater_chiller_status()
        return True

    def set_temperature_unit(self, unit: str):
        prev_unit = self.temperature_unit
        if unit == prev_unit:
            return
        # Convert all stored setpoints and temps to new unit
        if unit == "F":
            self.indoor_temp = self._c2f(self.indoor_temp)
            self.outdoor_temp = self._c2f(self.outdoor_temp)
            self.water_temp = self._c2f(self.water_temp)
            for s in self.setpoints:
                s["value"] = self._c2f(s["value"])
        elif unit == "C":
            self.indoor_temp = self._f2c(self.indoor_temp)
            self.outdoor_temp = self._f2c(self.outdoor_temp)
            self.water_temp = self._f2c(self.water_temp)
            for s in self.setpoints:
                s["value"] = self._f2c(s["value"])
        self.temperature_unit = unit

    def update_heater_chiller_status(self):
        if self.operating_mode == 1:  # Heating
            self.heater_status = True
            self.chiller_status = False
        elif self.operating_mode == 2:  # Cooling
            self.heater_status = False
            self.chiller_status = True
        else:
            self.heater_status = False
            self.chiller_status = False

    def _convert_temp(self, value: float):
        if self.temperature_unit == "C":
            return round(value, 1)
        else:
            return round(self._c2f(value), 1)

    @staticmethod
    def _c2f(c):
        return c * 9.0 / 5.0 + 32.0

    @staticmethod
    def _f2c(f):
        return (f - 32.0) * 5.0 / 9.0

hvac = BACnetHVACSim()
app = FastAPI(title="BACnet HVAC Room Controller DeviceShifu Driver")

# API Models
class ControlCommand(BaseModel):
    operating_mode: int | str | None = None
    setpoint_id: int | None = None
    setpoint_value: float | None = None
    ventilation_level: int | str | None = None
    temperature_unit: str | None = None

@app.get("/info")
def get_info():
    return {
        "device_name": DEVICE_NAME,
        "device_model": DEVICE_MODEL,
        "manufacturer": DEVICE_MANUFACTURER,
        "device_type": DEVICE_TYPE
    }

@app.get("/temperature")
def get_temperature():
    return hvac.get_temperature_data()

@app.get("/temperature/setpoints")
def get_setpoints():
    return hvac.get_setpoints_data()

@app.get("/status")
def get_status():
    return hvac.get_status()

@app.get("/data")
def get_comprehensive_data():
    return hvac.get_comprehensive_data()

@app.post("/control")
async def post_control(cmd: ControlCommand):
    try:
        d = cmd.dict(exclude_none=True)
        if not d:
            raise HTTPException(status_code=400, detail="No command data provided")
        hvac.set_control(d)
        return {"result": "success", "updated_state": hvac.get_comprehensive_data()}
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))

# Entrypoint for Uvicorn/gunicorn
def start():
    import uvicorn
    uvicorn.run(app, host=SERVER_HOST, port=SERVER_PORT)

if __name__ == "__main__":
    start()