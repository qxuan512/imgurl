import os
import json
from flask import Flask, request, jsonify
from bacpypes.core import run, stop
from bacpypes.app import BIPSimpleApplication
from bacpypes.pdu import Address
from bacpypes.apdu import ReadPropertyRequest, WritePropertyRequest, ConfirmedRequestSequence, Error, SimpleAckPDU
from bacpypes.object import get_datatype
from bacpypes.primitivedata import Real, Integer, Unsigned, Boolean, CharacterString
from bacpypes.local.device import LocalDeviceObject
from bacpypes.iocb import IOCB
from threading import Thread, Event

# ========== Environment Configuration ==========
BACNET_DEVICE_IP = os.environ.get("BACNET_DEVICE_IP", "192.168.2.165")
BACNET_DEVICE_PORT = int(os.environ.get("BACNET_DEVICE_PORT", "49665"))
BACNET_DEVICE_ID = int(os.environ.get("BACNET_DEVICE_ID", "2523161"))
LOCAL_BACNET_IP = os.environ.get("LOCAL_BACNET_IP", "192.168.2.100")
LOCAL_BACNET_PORT = int(os.environ.get("LOCAL_BACNET_PORT", "47808"))
HTTP_SERVER_HOST = os.environ.get("HTTP_SERVER_HOST", "0.0.0.0")
HTTP_SERVER_PORT = int(os.environ.get("HTTP_SERVER_PORT", "8080"))

# ========== BACnet Client Utilities ==========

class BACnetClient:
    def __init__(self, device_id, address, local_ip, local_port):
        self.device_id = device_id
        self.device_address = Address(f"{address}:{BACNET_DEVICE_PORT}")
        self.local_device = LocalDeviceObject(
            objectName="ShifuBACnetClient",
            objectIdentifier=599999,  # Use random unused BACnet Device ID
            maxApduLengthAccepted=1024,
            segmentationSupported="segmentedBoth",
            vendorIdentifier=15
        )
        self.app = BIPSimpleApplication(self.local_device, f"{local_ip}:{local_port}")
        self._response = None
        self._error = None
        self._event = Event()

    def _wait_for_response(self):
        self._event.wait(timeout=5)

    def _request_io(self, apdu):
        self._response = None
        self._error = None
        self._event.clear()
        iocb = IOCB(apdu)
        iocb.add_callback(self._response_callback)
        self.app.request_io(iocb)
        self._wait_for_response()
        if self._error:
            raise Exception(self._error)
        return self._response

    def _response_callback(self, iocb):
        if iocb.ioError:
            self._error = str(iocb.ioError)
        elif iocb.ioResponse:
            self._response = iocb.ioResponse
        self._event.set()

    def read_property(self, obj_type, instance, prop):
        apdu = ReadPropertyRequest(
            objectIdentifier=(obj_type, instance),
            propertyIdentifier=prop
        )
        apdu.pduDestination = self.device_address
        response = self._request_io(apdu)
        if hasattr(response, 'propertyValue'):
            datatype = get_datatype(obj_type, prop)
            if datatype:
                value = response.propertyValue.cast_out(datatype)
                return value
            else:
                return response.propertyValue.value
        else:
            raise Exception("No property value returned")

    def write_property(self, obj_type, instance, prop, value):
        datatype = get_datatype(obj_type, prop)
        if not datatype:
            raise Exception(f"Unknown datatype for {obj_type}.{prop}")
        apdu = WritePropertyRequest(
            objectIdentifier=(obj_type, instance),
            propertyIdentifier=prop,
            propertyValue=datatype(value)
        )
        apdu.pduDestination = self.device_address
        response = self._request_io(apdu)
        if isinstance(response, SimpleAckPDU):
            return True
        elif isinstance(response, Error):
            raise Exception(str(response))
        return False

    def close(self):
        stop()

# ========== BACnet-HVAC Data Map ==========
# This should be replaced with actual BACnet object types and instance/prop mapping
SENSOR_POINTS = [
    # Example: (object_type, instance_number, property_id, readable_name)
    ('analogInput', 1, 'presentValue', 'temperature'),
    ('analogInput', 2, 'presentValue', 'humidity'),
    ('analogInput', 3, 'presentValue', 'co2'),
    ('analogValue', 1, 'presentValue', 'setpoint'),
]
CONTROL_POINTS = [
    # Example: (object_type, instance_number, property_id, readable_name)
    ('analogValue', 1, 'presentValue', 'setpoint'),  # writeable setpoint
    ('binaryValue', 1, 'presentValue', 'fan'),       # on/off, 1/0
    # Add more as needed
]

# ========== Flask HTTP API Server ==========

app = Flask(__name__)

def get_bacnet_client():
    # For every request, create a fresh client to avoid BACnet core loop problems.
    client = BACnetClient(
        device_id=BACNET_DEVICE_ID,
        address=BACNET_DEVICE_IP,
        local_ip=LOCAL_BACNET_IP,
        local_port=LOCAL_BACNET_PORT
    )
    return client

@app.route("/api/data", methods=["GET"])
def api_data():
    client = get_bacnet_client()
    results = {}
    try:
        for obj_type, instance, prop, name in SENSOR_POINTS:
            try:
                value = client.read_property(obj_type, instance, prop)
                results[name] = value
            except Exception as e:
                results[name] = f"error: {str(e)}"
        status = 200
    except Exception as e:
        results = {"error": str(e)}
        status = 500
    finally:
        client.close()
    return jsonify(results), status

@app.route("/api/control", methods=["POST"])
def api_control():
    payload = request.get_json(force=True)
    client = get_bacnet_client()
    result = {}
    try:
        # Example: {"setpoint": 22.5, "fan": 1}
        for obj_type, instance, prop, name in CONTROL_POINTS:
            if name in payload:
                try:
                    value = payload[name]
                    client.write_property(obj_type, instance, prop, value)
                    result[name] = "ok"
                except Exception as e:
                    result[name] = f"error: {str(e)}"
        status = 200
    except Exception as e:
        result = {"error": str(e)}
        status = 500
    finally:
        client.close()
    return jsonify(result), status

# ========== HTTP Server Entrypoint ==========
if __name__ == "__main__":
    app.run(host=HTTP_SERVER_HOST, port=HTTP_SERVER_PORT, threaded=True)