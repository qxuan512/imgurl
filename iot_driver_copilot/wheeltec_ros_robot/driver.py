import os
import sys
import time
import threading
import yaml
import json
from typing import Any, Dict, Callable
from kubernetes import client, config
from kubernetes.client.rest import ApiException
import paho.mqtt.client as mqtt

# --- Kubernetes CRD Handling ---

EDGEDEVICE_CRD_GROUP = "shifu.edgenesis.io"
EDGEDEVICE_CRD_VERSION = "v1alpha1"
EDGEDEVICE_CRD_PLURAL = "edgedevices"
EDGEDEVICE_STATUS_PHASES = ["Pending", "Running", "Failed", "Unknown"]

def load_incluster_kube_config():
    config.load_incluster_config()

def get_k8s_custom_api():
    return client.CustomObjectsApi()

class EdgeDeviceStatusManager:
    def __init__(self, name: str, namespace: str):
        load_incluster_kube_config()
        self.api = get_k8s_custom_api()
        self.name = name
        self.namespace = namespace

    def get(self) -> Dict[str, Any]:
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

    def get_address(self) -> str:
        obj = self.get()
        if obj and 'spec' in obj and 'address' in obj['spec']:
            return obj['spec']['address']
        return None

    def update_phase(self, phase: str):
        if phase not in EDGEDEVICE_STATUS_PHASES:
            phase = "Unknown"
        status = {'edgeDevicePhase': phase}
        body = {'status': status}
        for _ in range(3):
            try:
                self.api.patch_namespaced_custom_object_status(
                    group=EDGEDEVICE_CRD_GROUP,
                    version=EDGEDEVICE_CRD_VERSION,
                    namespace=self.namespace,
                    plural=EDGEDEVICE_CRD_PLURAL,
                    name=self.name,
                    body=body
                )
                return
            except ApiException:
                time.sleep(1)
        # Give up silently if fails

# --- Instruction Config Loading ---

def load_instruction_config(path: str) -> Dict[str, Any]:
    if not os.path.exists(path):
        return {}
    with open(path, "r") as f:
        return yaml.safe_load(f) or {}

# --- MQTT Client Management ---

class MQTTClientManager:
    def __init__(self, broker_address: str):
        self.broker_address = broker_address
        self.mqtt_client = mqtt.Client()
        self.connected = threading.Event()
        self.subscriptions = {}
        self.lock = threading.Lock()
        # Callbacks
        self.mqtt_client.on_connect = self._on_connect
        self.mqtt_client.on_disconnect = self._on_disconnect
        self.mqtt_client.on_message = self._on_message
        self.message_callbacks = {}

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected.set()
        else:
            self.connected.clear()

    def _on_disconnect(self, client, userdata, rc):
        self.connected.clear()

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        payload = msg.payload.decode('utf-8')
        with self.lock:
            callbacks = self.message_callbacks.get(topic, [])
        for cb in callbacks:
            try:
                cb(topic, payload)
            except Exception:
                pass

    def connect(self, timeout=5) -> bool:
        addr, port = self._parse_broker_addr()
        try:
            self.mqtt_client.connect(addr, port, keepalive=30)
        except Exception:
            return False
        t = threading.Thread(target=self.mqtt_client.loop_start)
        t.daemon = True
        t.start()
        return self.connected.wait(timeout)

    def disconnect(self):
        try:
            self.mqtt_client.disconnect()
        except Exception:
            pass
        self.connected.clear()

    def _parse_broker_addr(self):
        if ':' in self.broker_address:
            addr, port = self.broker_address.rsplit(':', 1)
            return addr, int(port)
        return self.broker_address, 1883

    def subscribe(self, topic: str, qos: int, callback: Callable[[str, str], None]):
        with self.lock:
            if topic not in self.message_callbacks:
                self.message_callbacks[topic] = []
            self.message_callbacks[topic].append(callback)
        self.mqtt_client.subscribe(topic, qos=qos)

    def unsubscribe(self, topic: str):
        with self.lock:
            self.message_callbacks.pop(topic, None)
        self.mqtt_client.unsubscribe(topic)

    def publish(self, topic: str, payload: str, qos: int = 1):
        return self.mqtt_client.publish(topic, payload, qos=qos)

# --- DeviceShifu API Implementation ---

class DeviceShifu:
    def __init__(self):
        # Env required
        self.edge_device_name = os.environ.get("EDGEDEVICE_NAME")
        self.edge_device_namespace = os.environ.get("EDGEDEVICE_NAMESPACE")
        self.mqtt_broker_address = os.environ.get("MQTT_BROKER_ADDRESS")
        if not self.edge_device_name or not self.edge_device_namespace or not self.mqtt_broker_address:
            sys.exit(1)

        self.crd_manager = EdgeDeviceStatusManager(self.edge_device_name, self.edge_device_namespace)
        self.device_address = self.crd_manager.get_address()
        self.config_path = "/etc/edgedevice/config/instructions"
        self.api_settings = load_instruction_config(self.config_path)
        self.mqtt_manager = MQTTClientManager(self.mqtt_broker_address)
        self.status_thread = threading.Thread(target=self._status_maintainer, daemon=True)
        self.status_thread.start()

    def _status_maintainer(self):
        last_phase = None
        while True:
            phase = self._determine_phase()
            if phase != last_phase:
                self.crd_manager.update_phase(phase)
                last_phase = phase
            time.sleep(5)

    def _determine_phase(self) -> str:
        if not self.device_address:
            return "Unknown"
        if not self.mqtt_manager.connected.is_set():
            # Try to connect
            if self.mqtt_manager.connect(timeout=2):
                return "Running"
            else:
                return "Pending"
        return "Running"

    def _get_api_setting(self, api_name: str, key: str, default=None):
        obj = self.api_settings.get(api_name, {})
        plist = obj.get('protocolPropertyList', {})
        return plist.get(key, default)

    # --- API Methods ---

    # SUBSCRIBE: device/telemetry/imu, device/sensors/odom, device/sensors/scan, device/sensors/camera, device/sensors/imu, device/sensors/power, device/telemetry/odom
    # Each returns a generator of JSON messages

    def subscribe_device_telemetry_imu(self):
        return self._subscribe_stream("device/telemetry/imu", qos=1)

    def subscribe_device_sensors_odom(self):
        return self._subscribe_stream("device/sensors/odom", qos=1)

    def subscribe_device_sensors_scan(self):
        return self._subscribe_stream("device/sensors/scan", qos=1)

    def subscribe_device_sensors_camera(self):
        return self._subscribe_stream("device/sensors/camera", qos=1)

    def subscribe_device_sensors_imu(self):
        return self._subscribe_stream("device/sensors/imu", qos=1)

    def subscribe_device_sensors_power(self):
        return self._subscribe_stream("device/sensors/power", qos=1)

    def subscribe_device_telemetry_odom(self):
        return self._subscribe_stream("device/telemetry/odom", qos=1)

    # PUBLISH: device/commands/cmd_vel, device/commands/led
    def publish_device_commands_cmd_vel(self, payload: dict):
        topic = "device/commands/cmd_vel"
        qos = 1
        payload_str = json.dumps(payload)
        return self.mqtt_manager.publish(topic, payload_str, qos=qos)

    def publish_device_commands_led(self, payload: dict):
        topic = "device/commands/led"
        qos = 1
        payload_str = json.dumps(payload)
        return self.mqtt_manager.publish(topic, payload_str, qos=qos)

    # Internal subscribe stream helper
    def _subscribe_stream(self, topic: str, qos: int = 1):
        queue = []
        cond = threading.Condition()

        def cb(t, payload):
            with cond:
                queue.append(payload)
                cond.notify()

        self.mqtt_manager.subscribe(topic, qos, cb)
        try:
            while True:
                with cond:
                    while not queue:
                        cond.wait()
                    message = queue.pop(0)
                yield message
        finally:
            self.mqtt_manager.unsubscribe(topic)

# --- Entry Point for Testing (not for production use) ---

if __name__ == "__main__":
    ds = DeviceShifu()
    # Example usage: subscribe to IMU
    for msg in ds.subscribe_device_telemetry_imu():
        print("Received IMU:", msg)