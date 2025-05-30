import os
import sys
import signal
import time
import yaml
import threading
import json

from kubernetes import client, config, watch
from kubernetes.client.rest import ApiException
import paho.mqtt.client as mqtt

INSTRUCTION_PATH = "/etc/edgedevice/config/instructions"

EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE")
MQTT_BROKER_ADDRESS = os.environ.get("MQTT_BROKER_ADDRESS")

if not EDGEDEVICE_NAME or not EDGEDEVICE_NAMESPACE or not MQTT_BROKER_ADDRESS:
    sys.stderr.write("Required environment variables not set: EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE, MQTT_BROKER_ADDRESS\n")
    sys.exit(1)

# DeviceShifu status phases
EDGEDEVICE_PHASE_PENDING = "Pending"
EDGEDEVICE_PHASE_RUNNING = "Running"
EDGEDEVICE_PHASE_FAILED = "Failed"
EDGEDEVICE_PHASE_UNKNOWN = "Unknown"

# API Topics/Settings
API_TOPICS = {
    "SUBSCRIBE_current_position": "device/telemetry/current_position",
    "PUBLISH_navigation": "device/commands/navigation",
    "PUBLISH_cmd_vel": "device/commands/cmd_vel"
}
API_CONFIG = {}

# Initialize Kubernetes in-cluster config and API client
config.load_incluster_config()
crd_api = client.CustomObjectsApi()

def get_edgedevice():
    try:
        return crd_api.get_namespaced_custom_object(
            group="shifu.edgenesis.io",
            version="v1alpha1",
            namespace=EDGEDEVICE_NAMESPACE,
            plural="edgedevices",
            name=EDGEDEVICE_NAME,
        )
    except ApiException as e:
        return None

def update_edgeDevice_phase(phase):
    for _ in range(3):
        try:
            body = {"status": {"edgeDevicePhase": phase}}
            crd_api.patch_namespaced_custom_object_status(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=EDGEDEVICE_NAMESPACE,
                plural="edgedevices",
                name=EDGEDEVICE_NAME,
                body=body
            )
            return True
        except ApiException:
            time.sleep(1)
    return False

def load_api_config():
    global API_CONFIG
    try:
        with open(INSTRUCTION_PATH, "r") as f:
            API_CONFIG = yaml.safe_load(f)
    except Exception:
        API_CONFIG = {}

class DeviceShifuMQTTClient:
    def __init__(self, broker_addr):
        self.broker_host, self.broker_port = self._split_addr(broker_addr)
        self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        # Used for tracking subscribe status
        self.connected = threading.Event()
        self.failed = threading.Event()
        self.subscribed_topics = {}
        self.lock = threading.Lock()

    def _split_addr(self, addr):
        if ':' not in addr:
            return addr, 1883
        host, port = addr.rsplit(':', 1)
        return host, int(port)

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected.set()
            update_edgeDevice_phase(EDGEDEVICE_PHASE_RUNNING)
        else:
            self.failed.set()
            update_edgeDevice_phase(EDGEDEVICE_PHASE_FAILED)
    
    def _on_disconnect(self, client, userdata, rc):
        self.connected.clear()
        update_edgeDevice_phase(EDGEDEVICE_PHASE_PENDING)

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        payload = msg.payload.decode()
        with self.lock:
            if topic in self.subscribed_topics:
                cb = self.subscribed_topics[topic]
                try:
                    cb(topic, payload)
                except Exception:
                    pass

    def connect(self):
        try:
            self.client.connect(self.broker_host, self.broker_port, keepalive=60)
            self.client.loop_start()
            if not self.connected.wait(timeout=8):
                update_edgeDevice_phase(EDGEDEVICE_PHASE_FAILED)
                return False
            return True
        except Exception:
            self.failed.set()
            update_edgeDevice_phase(EDGEDEVICE_PHASE_FAILED)
            return False

    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()
        self.connected.clear()
        update_edgeDevice_phase(EDGEDEVICE_PHASE_PENDING)

    def subscribe(self, topic, qos, callback):
        with self.lock:
            self.subscribed_topics[topic] = callback
        self.client.subscribe(topic, qos=qos)

    def publish(self, topic, payload, qos=1):
        result = self.client.publish(topic, payload=payload, qos=qos)
        return result.rc == mqtt.MQTT_ERR_SUCCESS

    def stop(self):
        self.disconnect()

# DeviceShifu API logic

class DeviceShifu:
    def __init__(self, mqtt_client):
        self.mqtt_client = mqtt_client
        self.stop_event = threading.Event()
        self.sub_thread = None

    def api_subscribe_current_position(self, callback=None):
        api_name = "api1"
        topic = API_TOPICS["SUBSCRIBE_current_position"]
        api_conf = API_CONFIG.get(api_name, {})
        qos = int(api_conf.get("protocolPropertyList", {}).get("qos", 1))
        def _default_cb(topic, payload):
            # Placeholder: handle incoming position update (payload is JSON string)
            print(payload)
        cb = callback or _default_cb
        self.mqtt_client.subscribe(topic, qos, cb)

    def api_publish_navigation(self, navigation_goal_json):
        api_name = "api2"
        topic = API_TOPICS["PUBLISH_navigation"]
        api_conf = API_CONFIG.get(api_name, {})
        qos = int(api_conf.get("protocolPropertyList", {}).get("qos", 1))
        payload = (
            json.dumps(navigation_goal_json)
            if not isinstance(navigation_goal_json, str)
            else navigation_goal_json
        )
        return self.mqtt_client.publish(topic, payload, qos=qos)

    def api_publish_cmd_vel(self, cmd_vel_json):
        api_name = "api3"
        topic = API_TOPICS["PUBLISH_cmd_vel"]
        api_conf = API_CONFIG.get(api_name, {})
        qos = int(api_conf.get("protocolPropertyList", {}).get("qos", 1))
        payload = (
            json.dumps(cmd_vel_json)
            if not isinstance(cmd_vel_json, str)
            else cmd_vel_json
        )
        return self.mqtt_client.publish(topic, payload, qos=qos)

    def stop(self):
        self.stop_event.set()
        self.mqtt_client.stop()


def main():
    # Set initial phase
    update_edgeDevice_phase(EDGEDEVICE_PHASE_PENDING)

    # Load instruction config
    load_api_config()
    device_spec = get_edgedevice()
    if not device_spec:
        update_edgeDevice_phase(EDGEDEVICE_PHASE_UNKNOWN)
        sys.exit(1)

    address = device_spec.get("spec", {}).get("address", "")
    if not address:
        update_edgeDevice_phase(EDGEDEVICE_PHASE_UNKNOWN)
        sys.exit(1)

    mqtt_client = DeviceShifuMQTTClient(MQTT_BROKER_ADDRESS)

    def signal_handler(sig, frame):
        mqtt_client.stop()
        update_edgeDevice_phase(EDGEDEVICE_PHASE_PENDING)
        sys.exit(0)

    signal.signal(signal.SIGTERM, signal_handler)
    signal.signal(signal.SIGINT, signal_handler)

    if not mqtt_client.connect():
        update_edgeDevice_phase(EDGEDEVICE_PHASE_FAILED)
        sys.exit(1)

    shifu = DeviceShifu(mqtt_client)

    # Example: subscribe to position updates and print them
    def print_position(topic, payload):
        try:
            data = json.loads(payload)
            print("Received Position:", data)
        except Exception:
            print("Malformed position payload:", payload)
    shifu.api_subscribe_current_position(print_position)

    # Main loop: keep running, handle instructions if needed
    try:
        while True:
            time.sleep(2)
    except KeyboardInterrupt:
        pass
    finally:
        shifu.stop()
        update_edgeDevice_phase(EDGEDEVICE_PHASE_PENDING)

if __name__ == "__main__":
    main()