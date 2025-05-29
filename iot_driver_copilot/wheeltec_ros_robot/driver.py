import os
import sys
import yaml
import json
import threading
import time
import signal
from typing import Callable, Dict, Any

import paho.mqtt.client as mqtt
from kubernetes import client as k8s_client, config as k8s_config
from kubernetes.client.rest import ApiException

CONFIG_INSTRUCTION_PATH = "/etc/edgedevice/config/instructions"
EDGEDEVICE_CRD_GROUP = "shifu.edgenesis.io"
EDGEDEVICE_CRD_VERSION = "v1alpha1"
EDGEDEVICE_CRD_PLURAL = "edgedevices"

# DeviceShifu MQTT topic prefix
MQTT_PREFIX = "deviceshifu-car-mqtt"

# DeviceShifu API to MQTT topic mapping and type ('pub' or 'sub')
DEVICE_API = [
    {"method": "PUBLISH", "topic": f"{MQTT_PREFIX}/point_cloud", "api": "point_cloud"},
    {"method": "SUBSCRIBE", "topic": f"{MQTT_PREFIX}/stop", "api": "stop"},
    {"method": "PUBLISH", "topic": f"{MQTT_PREFIX}/joint_states", "api": "joint_states"},
    {"method": "PUBLISH", "topic": f"{MQTT_PREFIX}/scan", "api": "scan"},
    {"method": "PUBLISH", "topic": f"{MQTT_PREFIX}/odom", "api": "odom"},
    {"method": "SUBSCRIBE", "topic": f"{MQTT_PREFIX}/move/forward", "api": "move_forward"},
    {"method": "SUBSCRIBE", "topic": f"{MQTT_PREFIX}/move/backward", "api": "move_backward"},
    {"method": "PUBLISH", "topic": f"{MQTT_PREFIX}/status", "api": "status"},
    {"method": "SUBSCRIBE", "topic": f"{MQTT_PREFIX}/turn/right", "api": "turn_right"},
    {"method": "SUBSCRIBE", "topic": f"{MQTT_PREFIX}/turn/left", "api": "turn_left"},
]

# EdgeDevice Phase
EDGEDEVICE_PHASE_PENDING = "Pending"
EDGEDEVICE_PHASE_RUNNING = "Running"
EDGEDEVICE_PHASE_FAILED = "Failed"
EDGEDEVICE_PHASE_UNKNOWN = "Unknown"

def load_k8s_config():
    try:
        k8s_config.load_incluster_config()
    except Exception as e:
        print("Failed to load in-cluster config:", e)
        sys.exit(1)

def get_env_or_exit(envname: str) -> str:
    v = os.getenv(envname)
    if v is None or v == "":
        print(f"Required environment variable {envname} not set.")
        sys.exit(1)
    return v

class EdgeDeviceCRDClient:
    def __init__(self, name: str, namespace: str):
        self.name = name
        self.namespace = namespace
        self.api = k8s_client.CustomObjectsApi()
        self.resource = EDGEDEVICE_CRD_PLURAL
        self.group = EDGEDEVICE_CRD_GROUP
        self.version = EDGEDEVICE_CRD_VERSION

    def get(self) -> Dict[str, Any]:
        try:
            return self.api.get_namespaced_custom_object(
                group=self.group,
                version=self.version,
                namespace=self.namespace,
                plural=self.resource,
                name=self.name,
            )
        except ApiException as e:
            print("Exception when retrieving EdgeDevice:", e)
            return None

    def patch_status(self, phase: str):
        # Patch only the status.edgeDevicePhase field
        body = {
            "status": {
                "edgeDevicePhase": phase
            }
        }
        try:
            self.api.patch_namespaced_custom_object_status(
                group=self.group,
                version=self.version,
                namespace=self.namespace,
                plural=self.resource,
                name=self.name,
                body=body
            )
        except ApiException as e:
            print(f"Failed to update edgeDevicePhase to {phase}: {e}")

    def get_address(self) -> str:
        obj = self.get()
        if obj is None:
            return ""
        try:
            return obj["spec"]["address"]
        except KeyError:
            return ""

    def update_phase(self, phase: str):
        self.patch_status(phase)

class ConfigInstructionLoader:
    def __init__(self, path: str):
        self.path = path
        self.config = {}
        self.load()

    def load(self):
        try:
            with open(self.path, "r") as f:
                self.config = yaml.safe_load(f)
        except Exception as e:
            print(f"Error loading config instructions: {e}")
            self.config = {}

    def get_api_settings(self, api: str) -> Dict[str, Any]:
        if api in self.config:
            return self.config[api].get("protocolPropertyList", {})
        return {}

class DeviceShifuMQTTClient:
    def __init__(self, broker_addr: str, device_crd: EdgeDeviceCRDClient, config_loader: ConfigInstructionLoader):
        self.broker_addr = broker_addr
        self.device_crd = device_crd
        self.config_loader = config_loader
        self.client_id = f"shifu-mqtt-{os.urandom(4).hex()}"
        self.mqttc = mqtt.Client(client_id=self.client_id)
        self.pub_topics = {}
        self.sub_topics = {}
        self.sub_handlers = {}
        self.device_status = EDGEDEVICE_PHASE_PENDING
        self._stop_event = threading.Event()
        self._reconnect_delay = 5

        # Setup connect/disconnect callbacks
        self.mqttc.on_connect = self._on_connect
        self.mqttc.on_disconnect = self._on_disconnect
        self.mqttc.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.device_status = EDGEDEVICE_PHASE_RUNNING
            self.device_crd.update_phase(EDGEDEVICE_PHASE_RUNNING)
            # Subscribe to required topics
            for topic, qos in self.sub_topics.values():
                client.subscribe(topic, qos)
        else:
            self.device_status = EDGEDEVICE_PHASE_FAILED
            self.device_crd.update_phase(EDGEDEVICE_PHASE_FAILED)

    def _on_disconnect(self, client, userdata, rc):
        # If was running, now unknown
        self.device_status = EDGEDEVICE_PHASE_UNKNOWN
        self.device_crd.update_phase(EDGEDEVICE_PHASE_UNKNOWN)

    def _on_message(self, client, userdata, msg):
        for api, (topic, qos) in self.sub_topics.items():
            if msg.topic == topic:
                handler = self.sub_handlers.get(api)
                if handler:
                    handler(msg.payload)
                break

    def register_publish_api(self, api: str, topic: str, qos: int):
        self.pub_topics[api] = (topic, qos)

    def register_subscribe_api(self, api: str, topic: str, qos: int, handler: Callable[[bytes], None]):
        self.sub_topics[api] = (topic, qos)
        self.sub_handlers[api] = handler

    def start(self):
        # Connect and start loop in background
        broker_host, broker_port = self.broker_addr.split(":")
        self.mqttc.connect(broker_host, int(broker_port), keepalive=60)
        t = threading.Thread(target=self._loop_forever)
        t.daemon = True
        t.start()

    def _loop_forever(self):
        while not self._stop_event.is_set():
            try:
                self.mqttc.loop_forever()
            except Exception as e:
                print(f"MQTT Loop exception: {e}")
                self.device_status = EDGEDEVICE_PHASE_FAILED
                self.device_crd.update_phase(EDGEDEVICE_PHASE_FAILED)
                time.sleep(self._reconnect_delay)
                try:
                    broker_host, broker_port = self.broker_addr.split(":")
                    self.mqttc.reconnect()
                except Exception:
                    time.sleep(self._reconnect_delay)

    def stop(self):
        self._stop_event.set()
        try:
            self.mqttc.disconnect()
        except Exception:
            pass

    def publish(self, api: str, payload: Any):
        if api in self.pub_topics:
            topic, qos = self.pub_topics[api]
            self.mqttc.publish(topic, json.dumps(payload), qos=qos)

    # Handler registration for sub apis is done via register_subscribe_api

def main():
    # ENV VARS
    edgedevice_name = get_env_or_exit("EDGEDEVICE_NAME")
    edgedevice_namespace = get_env_or_exit("EDGEDEVICE_NAMESPACE")
    mqtt_broker_address = get_env_or_exit("MQTT_BROKER_ADDRESS")

    # Load k8s config
    load_k8s_config()
    device_crd = EdgeDeviceCRDClient(edgedevice_name, edgedevice_namespace)

    # Mark as pending at startup
    device_crd.update_phase(EDGEDEVICE_PHASE_PENDING)

    # Get CRD address (not used for MQTT, just for compliance)
    _ = device_crd.get_address()

    # Load configmap instructions
    config_loader = ConfigInstructionLoader(CONFIG_INSTRUCTION_PATH)

    # Construct MQTT client
    mqtt_client = DeviceShifuMQTTClient(mqtt_broker_address, device_crd, config_loader)

    # Register publish APIs
    for api_def in DEVICE_API:
        api = api_def["api"]
        method = api_def["method"]
        topic = api_def["topic"]
        qos = api_def.get("qos", 1)
        settings = config_loader.get_api_settings(api)
        if method == "PUBLISH":
            mqtt_client.register_publish_api(api, topic, qos)
        elif method == "SUBSCRIBE":
            # Provide a default handler for each sub
            def make_handler(api_name):
                def handler(payload):
                    print(f"Received command {api_name}: {payload.decode('utf-8')}")
                return handler
            mqtt_client.register_subscribe_api(api, topic, qos, make_handler(api))

    # Start MQTT
    mqtt_client.start()

    # Handle SIGTERM/SIGINT for graceful shutdown
    def signal_handler(sig, frame):
        print("Shutting down DeviceShifu MQTT driver...")
        mqtt_client.stop()
        device_crd.update_phase(EDGEDEVICE_PHASE_UNKNOWN)
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Example: periodic publishing simulation (mock data)
    def periodic_publish():
        while True:
            # Odometry
            mqtt_client.publish("odom", {"x": 1, "y": 2, "theta": 0.5, "timestamp": time.time()})
            # Scan
            mqtt_client.publish("scan", {"ranges": [1,2,3,4], "angle_min": 0, "angle_max": 3.14, "timestamp": time.time()})
            # Joint States
            mqtt_client.publish("joint_states", {"joint_1": 0.1, "joint_2": 0.2, "timestamp": time.time()})
            # Point Cloud
            mqtt_client.publish("point_cloud", {"points": [[1,2,3],[4,5,6]], "timestamp": time.time()})
            # Status
            mqtt_client.publish("status", {"status": "running", "timestamp": time.time()})
            time.sleep(5)

    pub_thread = threading.Thread(target=periodic_publish)
    pub_thread.daemon = True
    pub_thread.start()

    # Main thread just waits
    while True:
        time.sleep(60)

if __name__ == "__main__":
    main()