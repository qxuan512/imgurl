import os
import json
from flask import Flask, request, jsonify
from bacpypes.core import run, stop
from bacpypes.app import BIPSimpleApplication
from bacpypes.local.device import LocalDeviceObject
from bacpypes.pdu import Address
from bacpypes.apdu import ReadPropertyRequest, WritePropertyRequest, SimpleAckPDU, Error, AbortPDU, RejectPDU
from bacpypes.object import get_datatype
from bacpypes.primitivedata import Real, Unsigned, Integer, Boolean, CharacterString
from bacpypes.constructeddata import Array, Any
from threading import Thread, Event
import queue

# --- Environment Variable Configuration ---
HTTP_SERVER_HOST = os.getenv("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.getenv("HTTP_SERVER_PORT", "8080"))
BACNET_DEVICE_IP = os.getenv("BACNET_DEVICE_IP", "192.168.2.165")
BACNET_DEVICE_PORT = int(os.getenv("BACNET_DEVICE_PORT", "49665"))
BACNET_DEVICE_ID = int(os.getenv("BACNET_DEVICE_ID", "2523161"))
BACNET_LOCAL_IP = os.getenv("BACNET_LOCAL_IP", "192.168.2.100")
BACNET_LOCAL_PORT = int(os.getenv("BACNET_LOCAL_PORT", "47808"))

# --- BACnet Configuration ---
BACNET_DEVICE_ADDR = f"{BACNET_DEVICE_IP}:{BACNET_DEVICE_PORT}"
BACNET_LOCAL_ADDR = f"{BACNET_LOCAL_IP}:{BACNET_LOCAL_PORT}"

# --- Flask HTTP Server ---
app = Flask(__name__)

# --- BACpypes App/Threading Utility ---
class BACnetThread(Thread):
    def __init__(self, app, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.app = app
        self.daemon = True
        self._stop_event = Event()

    def run(self):
        run()

    def stop(self):
        stop()
        self._stop_event.set()

# --- BACnet Application Layer ---
class BACnetApp(BIPSimpleApplication):
    def __init__(self, device, address):
        super().__init__(device, address)
        self.response_queue = queue.Queue()

    def indication(self, apdu):
        super().indication(apdu)

    def confirmation(self, apdu):
        self.response_queue.put(apdu)

# --- BACnet Initialization ---
local_device = LocalDeviceObject(
    objectName="BACnetHTTPProxy",
    objectIdentifier=int(BACNET_DEVICE_ID) + 100000,  # Do not overlap with device
    maxApduLengthAccepted=1024,
    segmentationSupported="noSegmentation",
    vendorIdentifier=15
)
bacnet_app = BACnetApp(local_device, Address(BACNET_LOCAL_ADDR))
bacnet_thread = BACnetThread(bacnet_app)
bacnet_thread.start()

# --- BACnet Utility Functions ---
def bacnet_read_property(address, obj_type, obj_inst, prop_id):
    request = ReadPropertyRequest(
        objectIdentifier=(obj_type, obj_inst),
        propertyIdentifier=prop_id
    )
    request.pduDestination = Address(address)
    bacnet_app.request(request)
    try:
        apdu = bacnet_app.response_queue.get(timeout=3.0)
        if isinstance(apdu, Error) or isinstance(apdu, AbortPDU) or isinstance(apdu, RejectPDU):
            return None
        datatype = get_datatype(apdu.objectIdentifier[0], apdu.propertyIdentifier)
        if not datatype:
            return None
        if issubclass(datatype, Array) and apdu.propertyArrayIndex is not None:
            value = apdu.propertyValue.cast_out(datatype.subtype)
        else:
            value = apdu.propertyValue.cast_out(datatype)
        return value
    except queue.Empty:
        return None

def bacnet_write_property(address, obj_type, obj_inst, prop_id, value):
    datatype = get_datatype(obj_type, prop_id)
    if not datatype:
        return False
    if datatype is Real:
        value = Real(float(value))
    elif datatype is Unsigned:
        value = Unsigned(int(value))
    elif datatype is Integer:
        value = Integer(int(value))
    elif datatype is Boolean:
        value = Boolean(bool(value))
    elif datatype is CharacterString:
        value = CharacterString(str(value))
    else:
        value = Any(value)
    request = WritePropertyRequest(
        objectIdentifier=(obj_type, obj_inst),
        propertyIdentifier=prop_id,
        propertyValue=value
    )
    request.pduDestination = Address(address)
    bacnet_app.request(request)
    try:
        apdu = bacnet_app.response_queue.get(timeout=3.0)
        if isinstance(apdu, SimpleAckPDU):
            return True
        return False
    except queue.Empty:
        return False

# --- BACnet Object/Property Map for This Device ---
HVAC_OBJECTS = [
    # Example object/prop map; customize as per your device
    # (obj_type, obj_inst, prop_id, json_field)
    (8, BACNET_DEVICE_ID, 'presentValue', 'temperature'),     # Analog Value (temperature)
    (8, BACNET_DEVICE_ID + 1, 'presentValue', 'humidity'),    # Analog Value (humidity)
    (2, BACNET_DEVICE_ID, 'presentValue', 'fan_status'),      # Binary Value (fan)
    (8, BACNET_DEVICE_ID + 2, 'presentValue', 'setpoint'),    # Analog Value (setpoint)
    # Add more as needed
]

# --- HTTP API Implementation ---

@app.route('/api/data', methods=['GET'])
def get_data():
    filter_fields = request.args.getlist('field')
    result = {}
    for obj_type, obj_inst, prop_id, field in HVAC_OBJECTS:
        if filter_fields and field not in filter_fields:
            continue
        value = bacnet_read_property(BACNET_DEVICE_ADDR, obj_type, obj_inst, prop_id)
        result[field] = value
    return jsonify(result)

@app.route('/api/control', methods=['POST'])
def post_control():
    try:
        command = request.get_json(force=True)
    except Exception:
        return jsonify({"error": "Invalid JSON"}), 400

    results = {}
    success = True
    for field, value in command.items():
        found = False
        for obj_type, obj_inst, prop_id, map_field in HVAC_OBJECTS:
            if field == map_field:
                ok = bacnet_write_property(BACNET_DEVICE_ADDR, obj_type, obj_inst, prop_id, value)
                results[field] = "ok" if ok else "error"
                if not ok:
                    success = False
                found = True
        if not found:
            results[field] = "unknown_field"
            success = False
    return jsonify({"result": results}), (200 if success else 400)

if __name__ == '__main__':
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT)