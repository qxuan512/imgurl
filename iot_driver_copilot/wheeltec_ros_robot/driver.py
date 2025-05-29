import os
import sys
import time
import threading
import yaml
import json
from queue import Queue
from typing import Any, Dict, Callable, Optional

import paho.mqtt.client as mqtt
from kubernetes import client as k8s_client, config as k8s_config, utils as k8s_utils
from kubernetes.client.rest import ApiException

# ---------------------- Config & Constants ----------------------
EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE")
MQTT_BROKER_ADDRESS = os.environ.get("MQTT_BROKER_ADDRESS")

if not EDGEDEVICE_NAME or not EDGEDEVICE_NAMESPACE or not MQTT_BROKER_ADDRESS:
    sys.stderr.write("Required environment variables: EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE, MQTT_BROKER_ADDRESS\n")
    sys.exit(1)

CONFIG_PATH = "/etc/edgedevice/config/instructions"

EDGEDEVICE_CRD_GROUP = "shifu.edgenesis.io"
EDGEDEVICE_CRD_VERSION = "v1alpha1"
EDGEDEVICE_CRD_PLURAL = "edgedevices"

PHASE_PENDING = "Pending"
PHASE_RUNNING = "Running"
PHASE_FAILED = "Failed"
PHASE_UNKNOWN = "Unknown"

# ---------------------- Kubernetes CRD Client ----------------------

class EdgeDeviceCRD:
    def __init__(self, name: str, namespace: str):
        k8s_config.load_incluster_config()
        self.api = k8s_client.CustomObjectsApi()
        self.name = name
        self.namespace = namespace

    def get(self) -> Optional[Dict[str, Any]]:
        try:
            return self.api.get_namespaced_custom_object(
                group=EDGEDEVICE_CRD_GROUP,
                version=EDGEDEVICE_CRD_VERSION,
                namespace=self.namespace,
                plural=EDGEDEVICE_CRD_PLURAL,
                name=self.name
            )
        except ApiException as e:
            return None

    def update_status_phase(self, phase: str):
        for _ in range(3):
            try:
                body = {"status": {"edgeDevicePhase": phase}}
                self.api.patch_namespaced_custom_object_status(
                    group=EDGEDEVICE_CRD_GROUP,
                    version=EDGEDEVICE_CRD_VERSION,
                    namespace=self.namespace,
                    plural=EDGEDEVICE_CRD_PLURAL,
                    name=self.name,
                    body=body
                )
                return True
            except ApiException:
                time.sleep(1)
        return False

    def get_device_address(self) -> Optional[str]:
        obj = self.get()
        if obj and "spec" in obj and "address" in obj["spec"]:
            return obj["spec"]["address"]
        return None

# ---------------------- ConfigMap Loader ----------------------

def load_instructions_config(path: str) -> Dict[str, Any]:
    try:
        with open(path, "r") as f:
            return yaml.safe_load(f)
    except Exception:
        return {}

# ---------------------- MQTT Client Handler ----------------------

class MQTTClientHandler:
    def __init__(self, broker_address: str, config: Dict[str, Any], phase_callback: Callable[[str], None]):
        self.broker_address = broker_address
        self.config = config
        self.phase_callback = phase_callback
        self.client = None
        self.connected = threading.Event()
        self.subscriptions = {}
        self.subscriber_threads = {}
        self.incoming_queues = {}
        self.lock = threading.Lock()

    def connect(self):
        host, port = self._parse_broker(self.broker_address)
        self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        try:
            self.client.connect(host, port, 60)
            threading.Thread(target=self.client.loop_forever, daemon=True).start()
            if not self.connected.wait(timeout=10):
                self.phase_callback(PHASE_FAILED)
                return False
            self.phase_callback(PHASE_RUNNING)
            return True
        except Exception:
            self.phase_callback(PHASE_FAILED)
            return False

    def disconnect(self):
        if self.client:
            self.client.disconnect()
            self.phase_callback(PHASE_PENDING)

    def _parse_broker(self, address: str):
        if ":" in address:
            host, port = address.split(":", 1)
            return host, int(port)
        else:
            return address, 1883

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected.set()
        else:
            self.phase_callback(PHASE_FAILED)

    def _on_disconnect(self, client, userdata, rc):
        self.connected.clear()
        if rc != 0:
            self.phase_callback(PHASE_FAILED)
        else:
            self.phase_callback(PHASE_PENDING)

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        with self.lock:
            if topic in self.incoming_queues:
                self.incoming_queues[topic].put(msg.payload)

    def subscribe(self, topic: str, qos: int = 1):
        with self.lock:
            if topic not in self.subscriptions:
                self.client.subscribe(topic, qos=qos)
                self.incoming_queues[topic] = Queue()
                self.subscriptions[topic] = qos

    def unsubscribe(self, topic: str):
        with self.lock:
            if topic in self.subscriptions:
                self.client.unsubscribe(topic)
                del self.subscriptions[topic]
                del self.incoming_queues[topic]

    def publish(self, topic: str, payload: Any, qos: int = 1):
        self.client.publish(topic, json.dumps(payload), qos=qos)

    def get_message(self, topic: str, timeout: float = 2.0) -> Optional[Any]:
        queue = self.incoming_queues.get(topic)
        if queue:
            try:
                payload = queue.get(timeout=timeout)
                return json.loads(payload)
            except Exception:
                return None
        return None

# ---------------------- DeviceShifu API ----------------------

class DeviceShifu:
    def __init__(self, crd: EdgeDeviceCRD, mqtt_handler: MQTTClientHandler, config: Dict[str, Any]):
        self.crd = crd
        self.mqtt = mqtt_handler
        self.config = config
        self.api_map = self._build_api_map()

    def _build_api_map(self) -> Dict[str, Dict[str, Any]]:
        # Map API path to their properties from config
        return self.config or {}

    # --- SUBSCRIBE APIs ---
    def subscribe_imu(self, callback: Callable[[Dict], None]):
        topic = "device/telemetry/imu"
        qos = self._get_qos(topic)
        self.mqtt.subscribe(topic, qos)
        self._start_subscriber_thread(topic, callback)

    def subscribe_odom(self, callback: Callable[[Dict], None]):
        topic = "device/sensors/odom"
        qos = self._get_qos(topic)
        self.mqtt.subscribe(topic, qos)
        self._start_subscriber_thread(topic, callback)

    def subscribe_scan(self, callback: Callable[[Dict], None]):
        topic = "device/sensors/scan"
        qos = self._get_qos(topic)
        self.mqtt.subscribe(topic, qos)
        self._start_subscriber_thread(topic, callback)

    def subscribe_camera(self, callback: Callable[[Dict], None]):
        topic = "device/sensors/camera"
        qos = self._get_qos(topic)
        self.mqtt.subscribe(topic, qos)
        self._start_subscriber_thread(topic, callback)

    def subscribe_power(self, callback: Callable[[Dict], None]):
        topic = "device/sensors/power"
        qos = self._get_qos(topic)
        self.mqtt.subscribe(topic, qos)
        self._start_subscriber_thread(topic, callback)

    def subscribe_telemetry_imu(self, callback: Callable[[Dict], None]):
        topic = "device/telemetry/imu"
        qos = self._get_qos(topic)
        self.mqtt.subscribe(topic, qos)
        self._start_subscriber_thread(topic, callback)

    def subscribe_telemetry_odom(self, callback: Callable[[Dict], None]):
        topic = "device/telemetry/odom"
        qos = self._get_qos(topic)
        self.mqtt.subscribe(topic, qos)
        self._start_subscriber_thread(topic, callback)

    # --- PUBLISH APIs ---
    def publish_cmd_vel(self, payload: Dict):
        topic = "device/commands/cmd_vel"
        qos = self._get_qos(topic)
        self.mqtt.publish(topic, payload, qos)

    def publish_led(self, payload: Dict):
        topic = "device/commands/led"
        qos = self._get_qos(topic)
        self.mqtt.publish(topic, payload, qos)

    # --- Helpers ---
    def _get_qos(self, topic: str) -> int:
        for api, props in self.api_map.items():
            if api == topic or api.split('/')[-1] == topic.split('/')[-1]:
                protocol_props = props.get('protocolPropertyList', {})
                return int(protocol_props.get('qos', 1))
        return 1

    def _start_subscriber_thread(self, topic: str, callback: Callable[[Dict], None]):
        def run():
            while True:
                msg = self.mqtt.get_message(topic, timeout=10)
                if msg:
                    callback(msg)
        t = threading.Thread(target=run, daemon=True)
        t.start()
        self.mqtt.subscriber_threads[topic] = t

# ---------------------- Status Updater Thread ----------------------

def status_updater_loop(crd: EdgeDeviceCRD, mqtt_handler: MQTTClientHandler):
    prev_status = None
    while True:
        if mqtt_handler.connected.is_set():
            phase = PHASE_RUNNING
        else:
            phase = PHASE_PENDING
        if prev_status != phase:
            crd.update_status_phase(phase)
            prev_status = phase
        time.sleep(5)

# ---------------------- Main Entrypoint ----------------------

def main():
    # Prepare CRD & config
    crd = EdgeDeviceCRD(EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE)
    config = load_instructions_config(CONFIG_PATH)

    # Get device address from CRD (used only for K8s status, not MQTT)
    device_address = crd.get_device_address()

    # Setup MQTT
    mqtt_handler = MQTTClientHandler(
        broker_address=MQTT_BROKER_ADDRESS,
        config=config,
        phase_callback=lambda phase: crd.update_status_phase(phase)
    )

    # Start status updater thread
    threading.Thread(target=status_updater_loop, args=(crd, mqtt_handler), daemon=True).start()

    # Connect to MQTT Broker
    mqtt_handler.connect()

    # Init DeviceShifu
    shifu = DeviceShifu(crd, mqtt_handler, config)

    # Example usage for API subscription (replace with real callbacks/logic as needed)
    def imu_callback(data):
        print("IMU Data:", data)
    shifu.subscribe_imu(imu_callback)

    def odom_callback(data):
        print("Odometry Data:", data)
    shifu.subscribe_odom(odom_callback)

    def scan_callback(data):
        print("LIDAR Scan Data:", data)
    shifu.subscribe_scan(scan_callback)

    def camera_callback(data):
        print("Camera Data:", data)
    shifu.subscribe_camera(camera_callback)

    def power_callback(data):
        print("Power Data:", data)
    shifu.subscribe_power(power_callback)

    # Example publish usage
    # shifu.publish_cmd_vel({"linear": {"x":1.0, "y":0, "z":0}, "angular": {"x":0, "y":0, "z":0.5}})
    # shifu.publish_led({"status": "on"})

    while True:
        time.sleep(60)

if __name__ == "__main__":
    main()