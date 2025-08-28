import os
import json
from typing import Optional
from fastapi import FastAPI, Query, Request, Response, HTTPException, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from bacpypes.core import run, stop, enable_sleeping, deferred
from bacpypes.app import BIPSimpleApplication
from bacpypes.local.device import LocalDeviceObject
from bacpypes.pdu import Address
from bacpypes.object import AnalogInputObject, AnalogValueObject, CharacterStringValueObject
from bacpypes.service.device import WhoIsIAmServices
from bacpypes.basetypes import EngineeringUnits
from threading import Thread, Event

# ------- Configuration from Environment -------
BACNET_DEVICE_IP = os.environ.get("BACNET_DEVICE_IP", "192.168.1.10")
BACNET_DEVICE_ID = int(os.environ.get("BACNET_DEVICE_ID", 800001))
BACNET_DEVICE_PORT = int(os.environ.get("BACNET_DEVICE_PORT", 47808))
BACNET_BROADCAST = os.environ.get("BACNET_BROADCAST", "192.168.1.255")
HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", 8000))
DEVICE_NAME = os.environ.get("DEVICE_NAME", "BACnet HVAC Room Controller")
DEVICE_MODEL = os.environ.get("DEVICE_MODEL", "SE8000 series, VT8300")
DEVICE_MANUFACTURER = os.environ.get("DEVICE_MANUFACTURER", "Schneider Electric, Viconics")
DEVICE_TYPE = os.environ.get("DEVICE_TYPE", "Room Temperature Controller")

# ------- BACnet Local Device Setup -------
this_device = LocalDeviceObject(
    objectName=DEVICE_NAME,
    objectIdentifier=int(BACNET_DEVICE_ID),
    maxApduLengthAccepted=1024,
    segmentationSupported="segmentedBoth",
    vendorIdentifier=15
)
bacnet_application = None

# ------- BACnet Objects -------
indoor_temp = AnalogInputObject(
    objectIdentifier=('analogInput', 1),
    objectName='Indoor Temperature',
    presentValue=22.5,
    units=EngineeringUnits('DEGREES_CELSIUS')
)
outdoor_temp = AnalogInputObject(
    objectIdentifier=('analogInput', 2),
    objectName='Outdoor Temperature',
    presentValue=17.0,
    units=EngineeringUnits('DEGREES_CELSIUS')
)
hotwater_temp = AnalogInputObject(
    objectIdentifier=('analogInput', 3),
    objectName='Hot Water Temperature',
    presentValue=40.5,
    units=EngineeringUnits('DEGREES_CELSIUS')
)

setpoint1 = AnalogValueObject(
    objectIdentifier=('analogValue', 1),
    objectName='Setpoint 1',
    presentValue=20.0,
)
setpoint2 = AnalogValueObject(
    objectIdentifier=('analogValue', 2),
    objectName='Setpoint 2',
    presentValue=22.0,
)
setpoint3 = AnalogValueObject(
    objectIdentifier=('analogValue', 3),
    objectName='Setpoint 3',
    presentValue=24.0,
)
setpoint_desc1 = CharacterStringValueObject(
    objectIdentifier=('characterStringValue', 1),
    objectName='Setpoint 1 Description',
    presentValue="Eco"
)
setpoint_desc2 = CharacterStringValueObject(
    objectIdentifier=('characterStringValue', 2),
    objectName='Setpoint 2 Description',
    presentValue="Comfort"
)
setpoint_desc3 = CharacterStringValueObject(
    objectIdentifier=('characterStringValue', 3),
    objectName='Setpoint 3 Description',
    presentValue="Boost"
)
active_setpoint = AnalogValueObject(
    objectIdentifier=('analogValue', 0),
    objectName='Active Setpoint',
    presentValue=1.0,  # index: 1, 2, or 3
    outOfService=False
)

# BACnet object registry for lookups
bacnet_objects = {
    'indoor_temp': indoor_temp,
    'outdoor_temp': outdoor_temp,
    'hotwater_temp': hotwater_temp,
    'setpoint1': setpoint1,
    'setpoint2': setpoint2,
    'setpoint3': setpoint3,
    'setpoint_desc1': setpoint_desc1,
    'setpoint_desc2': setpoint_desc2,
    'setpoint_desc3': setpoint_desc3,
    'active_setpoint': active_setpoint,
}

# ------- BACnet App Initialization -------
def bacnet_app_init():
    global bacnet_application
    this_device.protocolServicesSupported = WhoIsIAmServices.supported_services
    bacnet_application = BIPSimpleApplication(this_device, f"{BACNET_DEVICE_IP}/{BACNET_DEVICE_PORT}")
    # Add objects to the application
    for obj in [
        indoor_temp, outdoor_temp, hotwater_temp,
        setpoint1, setpoint2, setpoint3,
        setpoint_desc1, setpoint_desc2, setpoint_desc3,
        active_setpoint
    ]:
        bacnet_application.add_object(obj)

# ------- BACnet Thread Control -------
bacnet_thread_event = Event()

def start_bacnet_stack():
    enable_sleeping()
    deferred(bacnet_thread_event.wait)
    run()

def stop_bacnet_stack():
    bacnet_thread_event.set()
    stop()

bacnet_thread = Thread(target=start_bacnet_stack, daemon=True)
bacnet_app_init()
bacnet_thread.start()

# ------- FastAPI Models -------
class ControlCommand(BaseModel):
    setpoint_index: Optional[int] = None  # 1,2,3
    setpoint_value: Optional[float] = None
    out_of_service: Optional[bool] = None

# ------- FastAPI Application -------
app = FastAPI()

@app.get("/api/info")
def api_info():
    return {
        "device_name": DEVICE_NAME,
        "device_model": DEVICE_MODEL,
        "manufacturer": DEVICE_MANUFACTURER,
        "device_type": DEVICE_TYPE
    }

@app.get("/api/temp")
def api_temp(unit: str = Query("C", pattern="^[CF]$", description="Temperature unit: C or F")):
    def convert(temp_c):
        if unit.upper() == "F":
            return round(temp_c * 9 / 5 + 32, 2)
        return round(temp_c, 2)
    data = {
        "indoor": convert(indoor_temp.presentValue),
        "outdoor": convert(outdoor_temp.presentValue),
        "hotwater": convert(hotwater_temp.presentValue),
        "unit": "Fahrenheit" if unit.upper() == "F" else "Celsius"
    }
    return data

@app.get("/api/setpt")
def api_setpt():
    idx = int(active_setpoint.presentValue)
    setpoints = [
        {
            "index": 1,
            "value": setpoint1.presentValue,
            "description": setpoint_desc1.presentValue
        },
        {
            "index": 2,
            "value": setpoint2.presentValue,
            "description": setpoint_desc2.presentValue
        },
        {
            "index": 3,
            "value": setpoint3.presentValue,
            "description": setpoint_desc3.presentValue
        }
    ]
    return {
        "preset_setpoints": setpoints,
        "active_setpoint_index": idx,
        "active_setpoint_value": setpoints[idx - 1]["value"] if 1 <= idx <= 3 else None
    }

@app.get("/api/status")
def api_status():
    idx = int(active_setpoint.presentValue)
    setpoint_value = None
    if idx == 1:
        setpoint_value = setpoint1.presentValue
    elif idx == 2:
        setpoint_value = setpoint2.presentValue
    elif idx == 3:
        setpoint_value = setpoint3.presentValue
    return {
        "active_setpoint_index": idx,
        "active_setpoint_value": setpoint_value,
        "out_of_service": active_setpoint.outOfService,
    }

@app.get("/api/data")
def api_data():
    # Full snapshot
    idx = int(active_setpoint.presentValue)
    setpoints = [
        {
            "index": 1,
            "value": setpoint1.presentValue,
            "description": setpoint_desc1.presentValue
        },
        {
            "index": 2,
            "value": setpoint2.presentValue,
            "description": setpoint_desc2.presentValue
        },
        {
            "index": 3,
            "value": setpoint3.presentValue,
            "description": setpoint_desc3.presentValue
        }
    ]
    setpoint_value = None
    if idx == 1:
        setpoint_value = setpoint1.presentValue
    elif idx == 2:
        setpoint_value = setpoint2.presentValue
    elif idx == 3:
        setpoint_value = setpoint3.presentValue
    return {
        "device_info": {
            "device_name": DEVICE_NAME,
            "device_model": DEVICE_MODEL,
            "manufacturer": DEVICE_MANUFACTURER,
            "device_type": DEVICE_TYPE
        },
        "temperature": {
            "indoor": round(indoor_temp.presentValue, 2),
            "outdoor": round(outdoor_temp.presentValue, 2),
            "hotwater": round(hotwater_temp.presentValue, 2),
            "unit": "Celsius"
        },
        "setpoints": setpoints,
        "active_setpoint_index": idx,
        "active_setpoint_value": setpoint_value,
        "out_of_service": active_setpoint.outOfService
    }

@app.post("/api/control")
def api_control(cmd: ControlCommand):
    if cmd.setpoint_index is not None:
        if cmd.setpoint_index not in [1,2,3]:
            raise HTTPException(status_code=400, detail="Invalid setpoint_index (must be 1,2,3)")
        active_setpoint.presentValue = cmd.setpoint_index
    if cmd.setpoint_value is not None and cmd.setpoint_index is not None:
        if cmd.setpoint_index == 1:
            setpoint1.presentValue = cmd.setpoint_value
        elif cmd.setpoint_index == 2:
            setpoint2.presentValue = cmd.setpoint_value
        elif cmd.setpoint_index == 3:
            setpoint3.presentValue = cmd.setpoint_value
    if cmd.out_of_service is not None:
        active_setpoint.outOfService = bool(cmd.out_of_service)
    return {"status": "ok"}

# ------- Shutdown Handler -------
import signal

def shutdown_handler(signum, frame):
    stop_bacnet_stack()

signal.signal(signal.SIGTERM, shutdown_handler)
signal.signal(signal.SIGINT, shutdown_handler)

# ------- Uvicorn Entrypoint -------
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT)