import os
import time
import threading
import yaml
import json
import traceback
from kubernetes import client, config
from kubernetes.client.rest import ApiException
import paho.mqtt.client as mqtt

# Constants for EdgeDevice CRD
CRD_GROUP = "shifu.edgenesis.io"
CRD_VERSION = "v1alpha1"
CRD_PLURAL = "edgedevices"
CRD_KIND = "EdgeDevice"

# Device phase states
PHASE_PENDING = "Pending"
PHASE_RUNNING = "Running"
PHASE_FAILED = "Failed"
PHASE_UNKNOWN = "Unknown"

# Load EdgeDevice name and namespace from env
EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE")
MQTT_BROKER_ADDRESS = os.environ.get("MQTT_BROKER_ADDRESS")

if not EDGEDEVICE_NAME or not EDGEDEVICE_NAMESPACE or not MQTT_BROKER_ADDRESS:
    raise Exception("EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE, and MQTT_BROKER_ADDRESS envs are required.")

# Load instruction config
CONFIG_INSTRUCTIONS_PATH = "/etc/edgedevice/config/instructions"
try:
    with open(CONFIG_INSTRUCTIONS_PATH, "r") as f:
        instruction_config = yaml.safe_load(f)
except Exception:
    instruction_config = {}

def get_api_settings(api_path):
    # Returns the settings for given api_path from instruction_config
    return instruction_config.get(api_path, {}).get("protocolPropertyList", {})

# Kubernetes API setup
config.load_incluster_config()
api = client.CustomObjectsApi()

def get_device_cr():
    return api.get_namespaced_custom_object(
        group=CRD_GROUP,
        version=CRD_VERSION,
        namespace=EDGEDEVICE_NAMESPACE,
        plural=CRD_PLURAL,
        name=EDGEDEVICE_NAME,
    )

def update_device_phase(phase):
    # Patch only the .status.edgeDevicePhase
    body = {"status": {"edgeDevicePhase": phase}}
    try:
        api.patch_namespaced_custom_object_status(
            group=CRD_GROUP,
            version=CRD_VERSION,
            namespace=EDGEDEVICE_NAMESPACE,
            plural=CRD_PLURAL,
            name=EDGEDEVICE_NAME,
            body=body,
        )
    except ApiException as e:
        pass  # Ignore patching errors to avoid crash loop

def get_device_address():
    try:
        device = get_device_cr()
        return device.get("spec", {}).get("address", None)
    except Exception:
        return None

# MQTT Topics
MQTT_TOPICS = [
    {
        "path": "wheeltecros/move/forward",
        "description": "Move forward",
        "api": "wheeltecros/move/forward",
    },
    {
        "path": "wheeltecros/move/backward",
        "description": "Move backward",
        "api": "wheeltecros/move/backward",
    },
    {
        "path": "wheeltecros/turn/left",
        "description": "Turn left",
        "api": "wheeltecros/turn/left",
    },
    {
        "path": "wheeltecros/turn/right",
        "description": "Turn right",
        "api": "wheeltecros/turn/right",
    },
    {
        "path": "wheeltecros/stop",
        "description": "Stop",
        "api": "wheeltecros/stop",
    },
]

# MQTT Client
class DeviceShifuMQTTClient:
    def __init__(self, broker_address, topics):
        self.broker_address, self.broker_port = self.parse_broker_address(broker_address)
        self.topics = topics
        self.mqtt_client = mqtt.Client()
        self.connected = False
        self.should_stop = threading.Event()
        self.status_lock = threading.Lock()
        self.status_phase = PHASE_PENDING
        self.last_status_update = 0

        # Register callbacks
        self.mqtt_client.on_connect = self.on_connect
        self.mqtt_client.on_disconnect = self.on_disconnect
        self.mqtt_client.on_message = self.on_message

    @staticmethod
    def parse_broker_address(addr):
        # Accepts address:port or address
        if ':' in addr:
            host, port = addr.split(':')
            return host.strip(), int(port.strip())
        else:
            return addr.strip(), 1883

    def start(self):
        threading.Thread(target=self._run_forever, daemon=True).start()
        threading.Thread(target=self._status_monitor_loop, daemon=True).start()

    def stop(self):
        self.should_stop.set()
        self.mqtt_client.disconnect()

    def _run_forever(self):
        while not self.should_stop.is_set():
            try:
                self.mqtt_client.connect(self.broker_address, self.broker_port, keepalive=60)
                self.mqtt_client.loop_forever()
            except Exception:
                self.set_status(PHASE_FAILED)
                time.sleep(5)  # Retry
            else:
                break

    def _status_monitor_loop(self):
        # Periodically update the device phase
        while not self.should_stop.is_set():
            with self.status_lock:
                phase = self.status_phase
            update_device_phase(phase)
            time.sleep(5)

    def set_status(self, phase):
        with self.status_lock:
            self.status_phase = phase

    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.set_status(PHASE_RUNNING)
            # Subscribe to all control topics
            for topic in self.topics:
                api_settings = get_api_settings(topic["api"])
                qos = api_settings.get("qos", 1)
                client.subscribe(topic["path"], qos)
        else:
            self.set_status(PHASE_FAILED)

    def on_disconnect(self, client, userdata, rc):
        self.set_status(PHASE_PENDING)

    def on_message(self, client, userdata, msg):
        # Dispatch to the appropriate handler
        try:
            payload = msg.payload.decode("utf-8")
            try:
                data = json.loads(payload) if payload else {}
            except Exception:
                data = {}
            topic = msg.topic
            if topic == "wheeltecros/move/forward":
                self.handle_move_forward(data)
            elif topic == "wheeltecros/move/backward":
                self.handle_move_backward(data)
            elif topic == "wheeltecros/turn/left":
                self.handle_turn_left(data)
            elif topic == "wheeltecros/turn/right":
                self.handle_turn_right(data)
            elif topic == "wheeltecros/stop":
                self.handle_stop(data)
        except Exception:
            traceback.print_exc()

    def handle_move_forward(self, params):
        # Implement movement logic here
        # Example: send command to ROS node or device over local IPC
        print("[Action] Move Forward:", params)

    def handle_move_backward(self, params):
        print("[Action] Move Backward:", params)

    def handle_turn_left(self, params):
        print("[Action] Turn Left:", params)

    def handle_turn_right(self, params):
        print("[Action] Turn Right:", params)

    def handle_stop(self, params):
        print("[Action] Stop:", params)

def main():
    device_address = get_device_address()
    # The device_address is not used for MQTT but may be useful for local integrations

    mqtt_topics = MQTT_TOPICS
    client = DeviceShifuMQTTClient(MQTT_BROKER_ADDRESS, mqtt_topics)
    client.start()

    while True:
        time.sleep(60)

if __name__ == "__main__":
    main()