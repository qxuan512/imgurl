import os
import sys
import json
import time
import yaml
import signal
import logging
import threading
from typing import Dict, Any, Optional

import paho.mqtt.client as mqtt
from flask import Flask, jsonify

from kubernetes import client, config
from kubernetes.client.rest import ApiException

# === Logging Configuration ===
LOG_LEVEL = os.getenv('LOG_LEVEL', 'INFO').upper()
logging.basicConfig(
    level=LOG_LEVEL,
    format='%(asctime)s %(levelname)s %(threadName)s %(name)s: %(message)s'
)
logger = logging.getLogger("deviceshifu-mqtt-driver")

# === Environment Variables/Defaults ===
EDGEDEVICE_NAME = os.getenv('EDGEDEVICE_NAME', 'deviceshifu-network-video-decoder')
EDGEDEVICE_NAMESPACE = os.getenv('EDGEDEVICE_NAMESPACE', 'devices')
CONFIG_MOUNT_PATH = os.getenv('CONFIG_MOUNT_PATH', '/etc/edgedevice/config')
MQTT_BROKER = os.getenv('MQTT_BROKER', 'localhost')
MQTT_BROKER_PORT = int(os.getenv('MQTT_BROKER_PORT', '1883'))
MQTT_BROKER_USERNAME = os.getenv('MQTT_BROKER_USERNAME', '')
MQTT_BROKER_PASSWORD = os.getenv('MQTT_BROKER_PASSWORD', '')
MQTT_TOPIC_PREFIX = os.getenv('MQTT_TOPIC_PREFIX', 'shifu')
HTTP_HOST = os.getenv('HTTP_HOST', '0.0.0.0')
HTTP_PORT = int(os.getenv('HTTP_PORT', '8080'))

# === Flask App for HTTP API ===
app = Flask(__name__)

# === ShifuClient: K8s Integration Layer ===
class ShifuClient:
    def __init__(self):
        self.edge_device_name = EDGEDEVICE_NAME
        self.edge_device_namespace = EDGEDEVICE_NAMESPACE
        self.config_mount_path = CONFIG_MOUNT_PATH
        self.api = None
        self.custom_obj_api = None
        self._init_k8s_client()

    def _init_k8s_client(self) -> None:
        try:
            config.load_incluster_config()
            logger.info("Loaded in-cluster Kubernetes config")
        except Exception:
            try:
                config.load_kube_config()
                logger.info("Loaded local kubeconfig")
            except Exception as e:
                logger.error(f"Failed to load Kubernetes config: {e}")
                self.api = None
                self.custom_obj_api = None
                return
        self.api = client.CoreV1Api()
        self.custom_obj_api = client.CustomObjectsApi()

    def get_edge_device(self) -> Dict[str, Any]:
        if self.custom_obj_api is None:
            logger.warning("Kubernetes custom object API not initialized")
            return {}
        try:
            edge_device = self.custom_obj_api.get_namespaced_custom_object(
                group="deviceshifu.edgedevice.io",
                version="v1alpha1",
                namespace=self.edge_device_namespace,
                plural="edgedevices",
                name=self.edge_device_name
            )
            logger.debug(f"EdgeDevice fetched: {edge_device}")
            return edge_device
        except ApiException as e:
            logger.error(f"Error getting EdgeDevice: {e}")
            return {}

    def get_device_address(self) -> Optional[str]:
        edge_device = self.get_edge_device()
        try:
            address = edge_device['spec']['address']
            logger.debug(f"Device address: {address}")
            return address
        except (KeyError, TypeError) as e:
            logger.error(f"Error retrieving device address: {e}")
            return None

    def update_device_status(self, status: Dict[str, Any]) -> None:
        if self.custom_obj_api is None:
            logger.warning("Kubernetes custom object API not initialized")
            return
        try:
            body = {
                "status": status
            }
            self.custom_obj_api.patch_namespaced_custom_object_status(
                group="deviceshifu.edgedevice.io",
                version="v1alpha1",
                namespace=self.edge_device_namespace,
                plural="edgedevices",
                name=self.edge_device_name,
                body=body
            )
            logger.info(f"Updated EdgeDevice status: {status}")
        except ApiException as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Optional[Any]:
        path = os.path.join(self.config_mount_path, filename)
        try:
            with open(path, 'r') as f:
                if filename.endswith('.yaml') or filename.endswith('.yml'):
                    data = yaml.safe_load(f)
                else:
                    data = f.read()
            logger.debug(f"Read config file {filename}: {data}")
            return data
        except Exception as e:
            logger.error(f"Failed to read config file {filename}: {e}")
            return None

    def get_instruction_config(self) -> Dict[str, Any]:
        data = self.read_mounted_config_file('instructions')
        if not data:
            logger.warning("No instruction config loaded")
            return {}
        if isinstance(data, str):
            try:
                data = yaml.safe_load(data)
            except Exception as e:
                logger.error(f"Error parsing instruction config: {e}")
                return {}
        logger.debug(f"Instruction config: {data}")
        return data or {}

# === Device Protocol Abstraction Layer (Simulated for Proprietary SDK/RTSP) ===
class DeviceConnectionManager:
    def __init__(self, device_address: Optional[str]):
        self.device_address = device_address
        self.connected = False
        self.lock = threading.Lock()
        self.connect()

    def connect(self):
        # Simulated connection logic for proprietary SDK/RTSP
        try:
            if self.device_address:
                logger.info(f"Connecting to device at {self.device_address} (Simulated)")
                self.connected = True
            else:
                logger.warning("No device address provided, skipping connection")
                self.connected = False
        except Exception as e:
            logger.error(f"Device connection failed: {e}")
            self.connected = False

    def disconnect(self):
        logger.info("Disconnecting device (Simulated)")
        self.connected = False

    def is_connected(self) -> bool:
        return self.connected

    def get_status(self) -> Dict[str, Any]:
        # Simulate status retrieval
        # In real implementation, use SDK/RTSP to get real device status
        if not self.connected:
            logger.warning("Device not connected, status unavailable")
            return {"connected": False, "error": "Device not connected"}
        status = {
            "connected": True,
            "device_model": "DS-64XXHD-S",
            "channels": 4,
            "error_codes": [],
            "upgrade_progress": 100,
            "alarm_info": [],
            "timestamp": int(time.time())
        }
        logger.debug(f"Device status: {status}")
        return status

    def reboot(self) -> Dict[str, Any]:
        # Simulate reboot command
        if not self.connected:
            logger.warning("Cannot reboot, device not connected")
            return {"success": False, "error": "Device not connected"}
        logger.info("Rebooting device (Simulated)")
        time.sleep(2)
        return {"success": True, "action": "reboot"}

    def config(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        # Simulate get/set config
        if not self.connected:
            logger.warning("Cannot config, device not connected")
            return {"success": False, "error": "Device not connected"}
        action = payload.get('action', 'get')
        logger.info(f"Processing config command: {action} (Simulated)")
        if action == 'get':
            config = {
                "display_config": {"windows": 4, "resolution": "1920x1080"},
                "scene_config": {"scenes": ["scene1", "scene2"]},
            }
            return {"success": True, "config": config}
        elif action == 'set':
            # In reality, validate and apply config
            logger.info(f"Applying configuration: {payload.get('config', {})} (Simulated)")
            return {"success": True, "applied_config": payload.get('config', {})}
        else:
            logger.warning(f"Unknown config action: {action}")
            return {"success": False, "error": f"Unknown action {action}"}

# === Main DeviceShifu MQTT Driver ===
class DeviceShifuMQTTDriver:
    def __init__(self):
        self.shifu_client = ShifuClient()
        self.device_address = self.shifu_client.get_device_address()
        self.device_manager = DeviceConnectionManager(self.device_address)

        self.instruction_config = self.shifu_client.get_instruction_config()
        self.latest_data: Dict[str, Any] = {}
        self.shutdown_flag = threading.Event()
        self.threads = []

        # MQTT Client
        self.mqtt_client = None
        self.mqtt_connected = False
        self.mqtt_reconnect_interval = 5
        self.client_id = f"{EDGEDEVICE_NAME}-{int(time.time())}"

        # MQTT Topics (from driver API info)
        self.telemetry_topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/device/telemetry/status"
        self.reboot_topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/device/command/reboot"
        self.config_topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/device/command/config"
        self.status_topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/status"
        self.control_topics = [
            self.reboot_topic,
            self.config_topic
        ]

        # Telemetry publishing config (from API info)
        self.telemetry_qos = 1
        self.reboot_qos = 1
        self.config_qos = 1

        # Threaded publishers by instruction config
        self._parse_publish_intervals()

    def _parse_publish_intervals(self):
        # Parses instruction config for publish intervals (default: 5000ms for telemetry)
        self.publish_intervals = {}
        # Fallback default
        self.publish_intervals[self.telemetry_topic] = int(
            self.instruction_config.get("publishIntervalMS", 5000)
        )
        # Further config per-instruction if defined
        if isinstance(self.instruction_config, dict):
            for instr, val in self.instruction_config.items():
                if isinstance(val, dict) and "publishIntervalMS" in val:
                    topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{instr.replace('_', '/')}"
                    self.publish_intervals[topic] = int(val["publishIntervalMS"])

    def _start_scheduled_publishers(self):
        for topic, interval_ms in self.publish_intervals.items():
            t = threading.Thread(
                target=self._publish_topic_periodically,
                args=(topic, interval_ms),
                daemon=True
            )
            t.start()
            self.threads.append(t)

    def _publish_topic_periodically(self, topic: str, interval_ms: int):
        logger.info(f"Starting publisher for topic {topic} with interval {interval_ms}ms")
        while not self.shutdown_flag.is_set():
            try:
                if topic == self.telemetry_topic:
                    data = self.device_manager.get_status()
                else:
                    data = self.latest_data.get(topic, {})
                self.publish_mqtt(topic, data, qos=self.telemetry_qos)
                self.latest_data[topic] = data
            except Exception as e:
                logger.error(f"Periodic publish failed for {topic}: {e}")
            time.sleep(interval_ms / 1000.0)

    def connect_mqtt(self):
        self.mqtt_client = mqtt.Client(client_id=self.client_id, clean_session=True)
        if MQTT_BROKER_USERNAME:
            self.mqtt_client.username_pw_set(MQTT_BROKER_USERNAME, MQTT_BROKER_PASSWORD)
        self.mqtt_client.on_connect = self.on_connect
        self.mqtt_client.on_message = self.on_message
        self.mqtt_client.on_disconnect = self.on_disconnect
        self.mqtt_client.loop_start()
        try:
            logger.info(f"Connecting to MQTT broker {MQTT_BROKER}:{MQTT_BROKER_PORT}")
            self.mqtt_client.connect(MQTT_BROKER, MQTT_BROKER_PORT, keepalive=30)
        except Exception as e:
            logger.error(f"MQTT connect failed: {e}")
            self.mqtt_connected = False

    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            logger.info("Connected to MQTT broker")
            self.mqtt_connected = True
            for topic in self.control_topics:
                try:
                    client.subscribe(topic, qos=1)
                    logger.info(f"Subscribed to control topic: {topic}")
                except Exception as e:
                    logger.error(f"Failed to subscribe to {topic}: {e}")
            # Publish initial status
            self.publish_status()
        else:
            logger.error(f"MQTT connection failed with rc={rc}")
            self.mqtt_connected = False

    def on_disconnect(self, client, userdata, rc):
        logger.warning(f"MQTT disconnected (rc={rc})")
        self.mqtt_connected = False
        while not self.shutdown_flag.is_set():
            try:
                logger.info("Attempting MQTT reconnect...")
                client.reconnect()
                return
            except Exception as e:
                logger.error(f"MQTT reconnect failed: {e}")
                time.sleep(self.mqtt_reconnect_interval)

    def on_message(self, client, userdata, msg):
        try:
            payload = msg.payload.decode('utf-8')
            logger.info(f"Received MQTT message on {msg.topic}: {payload}")
            data = json.loads(payload)
        except Exception as e:
            logger.error(f"Malformed MQTT message: {e}")
            return

        if msg.topic == self.reboot_topic:
            resp = self.device_manager.reboot()
            self.publish_mqtt(self.status_topic, resp, qos=self.reboot_qos)
        elif msg.topic == self.config_topic:
            resp = self.device_manager.config(data)
            self.publish_mqtt(self.status_topic, resp, qos=self.config_qos)
        else:
            logger.warning(f"Unhandled MQTT topic: {msg.topic}")

    def publish_mqtt(self, topic: str, payload: Any, qos: int = 1):
        if not self.mqtt_connected:
            logger.warning("MQTT not connected, skipping publish")
            return
        try:
            payload_json = json.dumps(payload, default=str)
            self.mqtt_client.publish(topic, payload=payload_json, qos=qos)
            logger.debug(f"Published to {topic}: {payload_json}")
        except Exception as e:
            logger.error(f"MQTT publish to {topic} failed: {e}")

    def publish_status(self):
        status = self.device_manager.get_status()
        self.publish_mqtt(self.status_topic, status, qos=1)

    def setup_routes(self):
        # Health
        @app.route('/health', methods=['GET'])
        def health():
            return jsonify({"status": "ok"}), 200

        # Status (device + MQTT)
        @app.route('/status', methods=['GET'])
        def status():
            res = {
                "device": self.device_manager.get_status(),
                "mqtt_connected": self.mqtt_connected
            }
            return jsonify(res), 200

    def signal_handler(self, signum, frame):
        logger.info(f"Received signal {signum}, shutting down...")
        self.shutdown()

    def shutdown(self):
        self.shutdown_flag.set()
        logger.info("Waiting for threads to terminate...")
        for t in self.threads:
            t.join(timeout=2)
        try:
            if self.mqtt_client:
                self.mqtt_client.loop_stop()
                self.mqtt_client.disconnect()
        except Exception as e:
            logger.error(f"Error during MQTT disconnect: {e}")
        try:
            self.device_manager.disconnect()
        except Exception as e:
            logger.error(f"Error during device disconnect: {e}")
        logger.info("Shutdown complete")
        sys.exit(0)

    def run(self):
        logger.info("Starting DeviceShifu MQTT driver")
        # Signal handlers
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        self.connect_mqtt()
        self.setup_routes()
        self._start_scheduled_publishers()

        # Start Flask HTTP server in thread
        http_thread = threading.Thread(
            target=lambda: app.run(host=HTTP_HOST, port=HTTP_PORT, use_reloader=False),
            daemon=True,
            name="HTTPServer"
        )
        http_thread.start()
        self.threads.append(http_thread)

        # Main thread: keepalive
        try:
            while not self.shutdown_flag.is_set():
                time.sleep(1)
        except Exception as e:
            logger.error(f"Main loop exception: {e}")
            self.shutdown()

# === Main Entrypoint ===
if __name__ == "__main__":
    driver = DeviceShifuMQTTDriver()
    driver.run()