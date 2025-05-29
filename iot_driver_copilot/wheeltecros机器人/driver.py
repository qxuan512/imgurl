import os
import sys
import time
import threading
import yaml
import json
import traceback
from queue import Queue, Empty
from typing import Callable, Dict, Any

import paho.mqtt.client as mqtt
from kubernetes import client, config, watch
from kubernetes.client.rest import ApiException

INSTRUCTION_CONFIG_PATH = "/etc/edgedevice/config/instructions"
EDGEDEVICE_CRD_GROUP = "shifu.edgenesis.io"
EDGEDEVICE_CRD_VERSION = "v1alpha1"
EDGEDEVICE_CRD_PLURAL = "edgedevices"
EDGEDEVICE_CRD_KIND = "EdgeDevice"

# Environment Variables
EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE")
MQTT_BROKER_ADDRESS = os.environ.get("MQTT_BROKER_ADDRESS")

if not EDGEDEVICE_NAME or not EDGEDEVICE_NAMESPACE or not MQTT_BROKER_ADDRESS:
    sys.stderr.write("Missing required environment variables: EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE, MQTT_BROKER_ADDRESS\n")
    sys.exit(1)

# --- K8s CRD Client ---
class EdgeDeviceStatusManager:
    def __init__(self, name: str, namespace: str):
        config.load_incluster_config()
        self.api = client.CustomObjectsApi()
        self.name = name
        self.namespace = namespace

    def get_edgedevice(self) -> Dict[str, Any]:
        return self.api.get_namespaced_custom_object(
            group=EDGEDEVICE_CRD_GROUP,
            version=EDGEDEVICE_CRD_VERSION,
            namespace=self.namespace,
            plural=EDGEDEVICE_CRD_PLURAL,
            name=self.name
        )

    def update_phase(self, phase: str):
        body = {"status": {"edgeDevicePhase": phase}}
        try:
            self.api.patch_namespaced_custom_object_status(
                group=EDGEDEVICE_CRD_GROUP,
                version=EDGEDEVICE_CRD_VERSION,
                namespace=self.namespace,
                plural=EDGEDEVICE_CRD_PLURAL,
                name=self.name,
                body=body
            )
        except ApiException as e:
            sys.stderr.write(f"Failed to update EdgeDevice phase: {e}\n")

    def get_address(self) -> str:
        ed = self.get_edgedevice()
        return ed.get("spec", {}).get("address", "")

# --- MQTT Handler ---
class MQTTDriver:
    def __init__(self, broker_address: str, instruction_config: dict, prefix: str):
        self.broker_address, self.broker_port = self._parse_broker_address(broker_address)
        self.instruction_config = instruction_config
        self.prefix = prefix
        self.client_id = f"deviceshifu-{os.getpid()}"
        self.mqttc = mqtt.Client(client_id=self.client_id, clean_session=True)
        self._setup_from_env()
        self.subscriptions: Dict[str, Callable[[str], None]] = {}
        self.sub_queues: Dict[str, Queue] = {}
        self._connect_event = threading.Event()
        self._disconnect_event = threading.Event()
        self.mqttc.on_connect = self._on_connect
        self.mqttc.on_disconnect = self._on_disconnect
        self.mqttc.on_message = self._on_message

    def _setup_from_env(self):
        # Optional username/password
        user = os.environ.get("MQTT_USERNAME", None)
        pw = os.environ.get("MQTT_PASSWORD", None)
        if user and pw:
            self.mqttc.username_pw_set(user, pw)
        # TLS?
        if os.environ.get("MQTT_TLS", "false").lower() == "true":
            self.mqttc.tls_set()

    def _parse_broker_address(self, addr: str):
        if ":" in addr:
            host, port = addr.split(":")
            return host, int(port)
        return addr, 1883

    def connect(self, timeout=10):
        self.mqttc.loop_start()
        try:
            self.mqttc.connect(self.broker_address, self.broker_port, keepalive=60)
        except Exception as e:
            raise RuntimeError(f"MQTT Connect failed: {e}")
        if not self._connect_event.wait(timeout):
            raise TimeoutError("MQTT connection timed out")

    def disconnect(self):
        self.mqttc.disconnect()
        self.mqttc.loop_stop()
        self._disconnect_event.wait(timeout=5)

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._connect_event.set()
        else:
            sys.stderr.write(f"MQTT connect failed: {rc}\n")

    def _on_disconnect(self, client, userdata, rc):
        self._disconnect_event.set()

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        payload = msg.payload.decode("utf-8")
        if topic in self.sub_queues:
            self.sub_queues[topic].put(payload)

    def publish(self, topic: str, payload: dict, qos: int = 1):
        self.mqttc.publish(topic, json.dumps(payload), qos=qos)

    def subscribe(self, topic: str, qos: int = 1):
        if topic not in self.sub_queues:
            self.sub_queues[topic] = Queue()
            self.mqttc.subscribe(topic, qos)

    def get_message(self, topic: str, timeout: float = 2.0):
        if topic not in self.sub_queues:
            return None
        try:
            return self.sub_queues[topic].get(timeout=timeout)
        except Empty:
            return None

# --- Instruction Config Loader ---
def load_instruction_config(path: str) -> dict:
    if not os.path.exists(path):
        return {}
    with open(path, "r") as f:
        return yaml.safe_load(f)

# --- API Implementation ---
class DeviceShifuAPI:
    def __init__(self, mqtt_driver: MQTTDriver, instruction_config: dict):
        self.mqtt_driver = mqtt_driver
        self.instruction_config = instruction_config
        self.prefix = mqtt_driver.prefix

    # --- PUBLISH APIs ---
    def move_forward(self, json_payload: dict = None):
        topic = f"{self.prefix}/move/forward"
        settings = self._get_settings("move/forward")
        payload = json_payload or {}
        payload.update(settings)
        self.mqtt_driver.publish(topic, payload, qos=1)
        return {"result": "ok"}

    def move_backward(self, json_payload: dict = None):
        topic = f"{self.prefix}/move/backward"
        settings = self._get_settings("move/backward")
        payload = json_payload or {}
        payload.update(settings)
        self.mqtt_driver.publish(topic, payload, qos=1)
        return {"result": "ok"}

    def turn_left(self, json_payload: dict = None):
        topic = f"{self.prefix}/turn/left"
        settings = self._get_settings("turn/left")
        payload = json_payload or {}
        payload.update(settings)
        self.mqtt_driver.publish(topic, payload, qos=1)
        return {"result": "ok"}

    def turn_right(self, json_payload: dict = None):
        topic = f"{self.prefix}/turn/right"
        settings = self._get_settings("turn/right")
        payload = json_payload or {}
        payload.update(settings)
        self.mqtt_driver.publish(topic, payload, qos=1)
        return {"result": "ok"}

    def stop(self):
        topic = f"{self.prefix}/stop"
        settings = self._get_settings("stop")
        payload = settings
        self.mqtt_driver.publish(topic, payload, qos=1)
        return {"result": "ok"}

    # --- SUBSCRIBE APIs ---
    def get_odom(self, timeout=2.0):
        topic = f"{self.prefix}/odom"
        self.mqtt_driver.subscribe(topic, qos=1)
        msg = self.mqtt_driver.get_message(topic, timeout=timeout)
        return json.loads(msg) if msg else None

    def get_scan(self, timeout=2.0):
        topic = f"{self.prefix}/scan"
        self.mqtt_driver.subscribe(topic, qos=1)
        msg = self.mqtt_driver.get_message(topic, timeout=timeout)
        return json.loads(msg) if msg else None

    def get_joint_states(self, timeout=2.0):
        topic = f"{self.prefix}/joint_states"
        self.mqtt_driver.subscribe(topic, qos=1)
        msg = self.mqtt_driver.get_message(topic, timeout=timeout)
        return json.loads(msg) if msg else None

    def get_point_cloud(self, timeout=2.0):
        topic = f"{self.prefix}/point_cloud"
        self.mqtt_driver.subscribe(topic, qos=1)
        msg = self.mqtt_driver.get_message(topic, timeout=timeout)
        return json.loads(msg) if msg else None

    def get_status(self, timeout=2.0):
        topic = f"{self.prefix}/status"
        self.mqtt_driver.subscribe(topic, qos=1)
        msg = self.mqtt_driver.get_message(topic, timeout=timeout)
        return json.loads(msg) if msg else None

    def _get_settings(self, api_name: str) -> dict:
        entry = self.instruction_config.get(api_name, {})
        return entry.get("protocolPropertyList", {}) if entry else {}

# --- Main Service Loop ---
def main():
    phase_mgr = EdgeDeviceStatusManager(EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE)

    # Initial phase is Pending
    phase_mgr.update_phase("Pending")
    device_addr = phase_mgr.get_address()
    prefix = "deviceshifu-car-mqtt"
    instruction_config = load_instruction_config(INSTRUCTION_CONFIG_PATH)

    mqtt_driver = MQTTDriver(
        broker_address=MQTT_BROKER_ADDRESS,
        instruction_config=instruction_config,
        prefix=prefix
    )

    try:
        mqtt_driver.connect(timeout=10)
        phase_mgr.update_phase("Running")
    except Exception as e:
        phase_mgr.update_phase("Failed")
        sys.stderr.write(f"MQTT connect error: {e}\n")
        sys.exit(1)

    api = DeviceShifuAPI(mqtt_driver, instruction_config)

    # Simple HTTP interface for demonstration (Flask-like, but minimal)
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class Handler(BaseHTTPRequestHandler):
        def _parse_json(self):
            content_length = int(self.headers.get('Content-Length', 0))
            if content_length == 0:
                return {}
            body = self.rfile.read(content_length)
            try:
                return json.loads(body.decode("utf-8"))
            except Exception:
                return {}

        def _send_json(self, data, code=200):
            resp = json.dumps(data).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

        def do_POST(self):
            try:
                if self.path.endswith("/move/forward"):
                    payload = self._parse_json()
                    result = api.move_forward(payload)
                    self._send_json(result)
                elif self.path.endswith("/move/backward"):
                    payload = self._parse_json()
                    result = api.move_backward(payload)
                    self._send_json(result)
                elif self.path.endswith("/turn/left"):
                    payload = self._parse_json()
                    result = api.turn_left(payload)
                    self._send_json(result)
                elif self.path.endswith("/turn/right"):
                    payload = self._parse_json()
                    result = api.turn_right(payload)
                    self._send_json(result)
                elif self.path.endswith("/stop"):
                    result = api.stop()
                    self._send_json(result)
                else:
                    self.send_error(404)
            except Exception:
                self.send_error(500)

        def do_GET(self):
            try:
                if self.path.endswith("/odom"):
                    data = api.get_odom()
                    self._send_json(data or {})
                elif self.path.endswith("/scan"):
                    data = api.get_scan()
                    self._send_json(data or {})
                elif self.path.endswith("/joint_states"):
                    data = api.get_joint_states()
                    self._send_json(data or {})
                elif self.path.endswith("/point_cloud"):
                    data = api.get_point_cloud()
                    self._send_json(data or {})
                elif self.path.endswith("/status"):
                    data = api.get_status()
                    self._send_json(data or {})
                else:
                    self.send_error(404)
            except Exception:
                self.send_error(500)

    http_port = int(os.environ.get("SHIFU_HTTP_PORT", "8080"))
    server = HTTPServer(("", http_port), Handler)

    def phase_monitor():
        prev_phase = "Running"
        while True:
            try:
                if not mqtt_driver._connect_event.is_set():
                    phase_mgr.update_phase("Failed")
                    prev_phase = "Failed"
                else:
                    if prev_phase != "Running":
                        phase_mgr.update_phase("Running")
                        prev_phase = "Running"
            except Exception:
                phase_mgr.update_phase("Unknown")
                prev_phase = "Unknown"
            time.sleep(10)

    threading.Thread(target=phase_monitor, daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        mqtt_driver.disconnect()
        phase_mgr.update_phase("Unknown")

if __name__ == "__main__":
    main()