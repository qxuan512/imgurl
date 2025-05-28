import os
import sys
import time
import json
import yaml
import signal
import threading
import logging
from typing import Dict, Any, Optional

from flask import Flask, jsonify
from kubernetes import client as k8s_client, config as k8s_config
import paho.mqtt.client as mqtt

# --------------------- Logging Configuration ---------------------
LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL),
    format='[%(asctime)s] %(levelname)s - %(threadName)s - %(message)s',
    stream=sys.stdout
)
logger = logging.getLogger("deviceshifu-mqtt-driver")

# --------------------- Global Shutdown Flag ---------------------
shutdown_flag = threading.Event()

# --------------------- ShifuClient Definition ---------------------
class ShifuClient:
    def __init__(self):
        self.device_name = os.getenv("EDGEDEVICE_NAME", "deviceshifu-video-decoder")
        self.namespace = os.getenv("EDGEDEVICE_NAMESPACE", "devices")
        self.config_mount_path = os.getenv("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
        self._init_k8s_client()

    def _init_k8s_client(self):
        try:
            if os.path.exists('/var/run/secrets/kubernetes.io/serviceaccount/token'):
                k8s_config.load_incluster_config()
                logger.info("Loaded in-cluster Kubernetes config.")
            else:
                k8s_config.load_kube_config()
                logger.info("Loaded local Kubernetes config.")
            self.k8s_api = k8s_client.CustomObjectsApi()
        except Exception as e:
            logger.error(f"Failed to initialize Kubernetes client: {e}")
            self.k8s_api = None

    def get_edge_device(self) -> Optional[Dict[str, Any]]:
        if not self.k8s_api:
            logger.warning("Kubernetes API not initialized.")
            return None
        try:
            return self.k8s_api.get_namespaced_custom_object(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.device_name
            )
        except Exception as e:
            logger.error(f"Error getting EdgeDevice CR: {e}")
            return None

    def get_device_address(self) -> Optional[str]:
        device = self.get_edge_device()
        if device and "spec" in device and "address" in device["spec"]:
            return device["spec"]["address"]
        logger.warning("Device address not found in EdgeDevice CR.")
        return None

    def update_device_status(self, status: str) -> None:
        if not self.k8s_api:
            return
        body = {"status": {"devicePhase": status}}
        try:
            self.k8s_api.patch_namespaced_custom_object_status(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.device_name,
                body=body
            )
            logger.info(f"Updated EdgeDevice status to {status}")
        except Exception as e:
            logger.warning(f"Failed to update EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Optional[str]:
        file_path = os.path.join(self.config_mount_path, filename)
        try:
            with open(file_path, "r") as f:
                content = f.read()
            logger.debug(f"Read config file: {file_path}")
            return content
        except Exception as e:
            logger.error(f"Failed to read config file {file_path}: {e}")
            return None

    def get_instruction_config(self) -> Dict[str, Any]:
        instructions_yaml = self.read_mounted_config_file("instructions")
        if instructions_yaml is None:
            return {}
        try:
            return yaml.safe_load(instructions_yaml) or {}
        except Exception as e:
            logger.error(f"Failed to parse instruction config YAML: {e}")
            return {}

# --------------------- Device Protocol Abstraction ---------------------
class HikvisionDecoderDevice:
    """
    Stub implementation for Hikvision Decoder device protocol.
    In production, replace methods below with real SDK/driver calls.
    """
    def __init__(self, address: Optional[str]):
        self.address = address
        self.connected = False

    def connect(self) -> bool:
        # Simulate successful device connection
        logger.info(f"Connecting to Hikvision Decoder at {self.address} ...")
        time.sleep(1)
        self.connected = True
        logger.info("Connected to Hikvision Decoder.")
        return True

    def disconnect(self) -> None:
        logger.info("Disconnecting from Hikvision Decoder.")
        self.connected = False

    def get_status(self) -> Dict[str, Any]:
        # Simulate status response
        return {
            "device_model": "DS-6300D(-JX/-T)",
            "manufacturer": "Hikvision",
            "timestamp": int(time.time()),
            "decoder_channel_status": "normal",
            "remote_playback_status": "stopped",
            "display_configuration": {
                "wall": "A",
                "window": "1x4"
            },
            "local_network_info": {
                "ip": self.address,
                "mac": "11:22:33:44:55:66"
            }
        }

    def get_alarm(self) -> Dict[str, Any]:
        # Simulate alarm data
        return {
            "alarm_status": "clear",
            "alarm_code": 0,
            "description": "No alarm",
            "timestamp": int(time.time())
        }

    def send_command(self, command: str, params: Dict[str, Any]) -> Dict[str, Any]:
        logger.info(f"Executing command '{command}' with params {params}")
        # Simulate command execution
        if command == "reboot":
            return {"result": "success", "timestamp": int(time.time())}
        elif command == "control":
            return {"result": "success", "detail": "operation performed", "timestamp": int(time.time())}
        else:
            return {"result": "unknown command", "timestamp": int(time.time())}

# --------------------- Main Driver ---------------------
class DeviceShifuMQTTDriver:
    def __init__(self):
        # Environment variables/config
        self.device_name = os.getenv("EDGEDEVICE_NAME", "deviceshifu-video-decoder")
        self.namespace = os.getenv("EDGEDEVICE_NAMESPACE", "devices")
        self.config_mount_path = os.getenv("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
        self.mqtt_broker = os.getenv("MQTT_BROKER", "localhost")
        self.mqtt_port = int(os.getenv("MQTT_BROKER_PORT", 1883))
        self.mqtt_username = os.getenv("MQTT_BROKER_USERNAME")
        self.mqtt_password = os.getenv("MQTT_BROKER_PASSWORD")
        self.mqtt_topic_prefix = os.getenv("MQTT_TOPIC_PREFIX", "shifu")
        self.http_host = os.getenv("HTTP_HOST", "0.0.0.0")
        self.http_port = int(os.getenv("HTTP_PORT", 8080))
        self.log_level = os.getenv("LOG_LEVEL", "INFO").upper()

        # Shifu, Device, MQTT and Data
        self.shifu_client = ShifuClient()
        self.instruction_config = self.shifu_client.get_instruction_config()
        self.device_address = self.shifu_client.get_device_address()
        self.device = HikvisionDecoderDevice(self.device_address)
        self.latest_data: Dict[str, Any] = {}
        self.publish_intervals: Dict[str, int] = {}
        self.publish_threads: Dict[str, threading.Thread] = {}
        self.mqtt_client: Optional[mqtt.Client] = None
        self.mqtt_connected = threading.Event()
        self.mqtt_reconnect_backoff = 2  # seconds
        self.mqtt_client_id = f"{self.device_name}-{int(time.time())}"
        self.status_lock = threading.Lock()

        # HTTP (Flask) Server
        self.app = Flask(__name__)
        self.http_thread: Optional[threading.Thread] = None

        # Device connection status
        self.device_connected = threading.Event()

        # Thread management
        self.threads: list = []

    # ------------------ MQTT Setup ------------------
    def connect_mqtt(self):
        client = mqtt.Client(client_id=self.mqtt_client_id, clean_session=True)
        if self.mqtt_username:
            client.username_pw_set(self.mqtt_username, self.mqtt_password)
        client.on_connect = self.on_mqtt_connect
        client.on_message = self.on_mqtt_message
        client.on_disconnect = self.on_mqtt_disconnect
        client.reconnect_delay_set(min_delay=2, max_delay=30)
        self.mqtt_client = client
        while not shutdown_flag.is_set():
            try:
                logger.info(f"Connecting to MQTT broker at {self.mqtt_broker}:{self.mqtt_port} ...")
                client.connect(self.mqtt_broker, self.mqtt_port, keepalive=60)
                client.loop_start()
                if self.mqtt_connected.wait(timeout=10):
                    logger.info("Connected to MQTT broker.")
                    break
                else:
                    logger.warning("MQTT connect timed out, retrying ...")
            except Exception as e:
                logger.error(f"MQTT connection failed: {e}")
            time.sleep(self.mqtt_reconnect_backoff)

    def on_mqtt_connect(self, client, userdata, flags, rc):
        if rc == 0:
            logger.info("MQTT connection established.")
            self.mqtt_connected.set()
            # Subscribe to relevant topics (control commands, etc.)
            subscribe_topics = [
                (f"{self.mqtt_topic_prefix}/{self.device_name}/control/reboot", 1),
                (f"{self.mqtt_topic_prefix}/{self.device_name}/control/control", 2),
            ]
            for topic, qos in subscribe_topics:
                try:
                    client.subscribe(topic, qos=qos)
                    logger.info(f"Subscribed to MQTT topic: {topic} (QoS={qos})")
                except Exception as e:
                    logger.error(f"Failed to subscribe to {topic}: {e}")
        else:
            logger.error(f"MQTT connection failed with RC={rc}")
            self.mqtt_connected.clear()

    def on_mqtt_disconnect(self, client, userdata, rc):
        logger.warning(f"MQTT disconnected (rc={rc}).")
        self.mqtt_connected.clear()
        if not shutdown_flag.is_set():
            logger.info("Attempting to reconnect to MQTT ...")
            self.connect_mqtt()

    def on_mqtt_message(self, client, userdata, msg):
        topic = msg.topic
        try:
            payload = json.loads(msg.payload.decode())
        except Exception:
            payload = msg.payload.decode()
        logger.info(f"Received MQTT message on {topic}: {payload}")
        # Handle device command topics
        if topic.endswith("/control/reboot"):
            resp = self.device.send_command("reboot", payload if isinstance(payload, dict) else {})
            self.publish_status_update()
        elif topic.endswith("/control/control"):
            resp = self.device.send_command("control", payload if isinstance(payload, dict) else {})
            self.publish_status_update()
        else:
            logger.warning(f"Unhandled MQTT topic: {topic}")

    def publish_mqtt(self, topic_suffix: str, data: Dict[str, Any], qos: int = 1, retain: bool = False):
        if not self.mqtt_client or not self.mqtt_connected.is_set():
            logger.warning("MQTT not connected; cannot publish.")
            return
        topic = f"{self.mqtt_topic_prefix}/{self.device_name}/{topic_suffix}"
        try:
            payload = json.dumps(data, default=str)
            self.mqtt_client.publish(topic, payload, qos=qos, retain=retain)
            logger.info(f"Published to {topic}: {payload}")
        except Exception as e:
            logger.error(f"Failed to publish to {topic}: {e}")

    # ------------------ Instruction/Config Parsing ------------------
    def _parse_publish_intervals(self):
        """
        Parse instruction config for publishIntervalMS and publishing mode.
        """
        for instr, cfg in self.instruction_config.get("instructions", {}).items():
            mode = cfg.get("protocolProperties", {}).get("mode", "").lower()
            if mode == "publisher":
                interval = cfg.get("protocolProperties", {}).get("publishIntervalMS", 1000)
                self.publish_intervals[instr] = int(interval)
                logger.info(f"Instruction '{instr}': mode=publisher, publishIntervalMS={interval}")

    # ------------------ Periodic Publishing ------------------
    def _start_scheduled_publishers(self):
        for instr, interval in self.publish_intervals.items():
            t = threading.Thread(target=self._publish_topic_periodically, args=(instr, interval), name=f"publisher-{instr}", daemon=True)
            self.publish_threads[instr] = t
            t.start()
            self.threads.append(t)

    def _publish_topic_periodically(self, instruction: str, interval_ms: int):
        topic_mapping = {
            "status_report": "telemetry/status",
            "alarm_status": "telemetry/alarm",
        }
        # Determine which data to pull based on instruction name
        while not shutdown_flag.is_set():
            try:
                data = None
                qos = 1
                if instruction == "status_report":
                    data = self.device.get_status()
                    topic = topic_mapping["status_report"]
                elif instruction == "alarm_status":
                    data = self.device.get_alarm()
                    topic = topic_mapping["alarm_status"]
                else:
                    logger.debug(f"Unknown publisher instruction: {instruction}")
                    time.sleep(interval_ms / 1000.0)
                    continue
                self.latest_data[instruction] = data
                self.publish_mqtt(topic, data, qos=qos)
            except Exception as e:
                logger.error(f"Error publishing {instruction}: {e}")
            time.sleep(interval_ms / 1000.0)

    def publish_status_update(self):
        # Publish device status to status topic
        try:
            status_data = self.device.get_status()
            self.latest_data["status_report"] = status_data
            self.publish_mqtt("status", status_data, qos=1)
        except Exception as e:
            logger.error(f"Error publishing status update: {e}")

    # ------------------ HTTP Server Setup ------------------
    def setup_routes(self):
        @self.app.route("/health", methods=["GET"])
        def health():
            return jsonify({"status": "ok", "mqtt_connected": self.mqtt_connected.is_set(), "device_connected": self.device_connected.is_set()}), 200

        @self.app.route("/status", methods=["GET"])
        def status():
            with self.status_lock:
                status = self.latest_data.get("status_report", {})
            return jsonify(status), 200

    def run_http_server(self):
        try:
            logger.info(f"Starting HTTP server on {self.http_host}:{self.http_port}")
            self.app.run(host=self.http_host, port=self.http_port, threaded=True, use_reloader=False)
        except Exception as e:
            logger.error(f"HTTP server exception: {e}")

    # ------------------ Lifecycle and Signal Handling ------------------
    def signal_handler(self, sig, frame):
        logger.info(f"Received signal {sig}, shutting down ...")
        shutdown_flag.set()
        self.shutdown()

    def shutdown(self):
        try:
            logger.info("Shutting down DeviceShifu MQTT driver ...")
            if self.mqtt_client:
                self.mqtt_client.loop_stop()
                self.mqtt_client.disconnect()
            if hasattr(self.device, "disconnect"):
                self.device.disconnect()
            # Wait for threads
            for t in self.threads:
                if t.is_alive():
                    t.join(timeout=2)
            logger.info("Shutdown complete.")
        except Exception as e:
            logger.error(f"Error during shutdown: {e}")

    # ------------------ Main Run ------------------
    def run(self):
        # Register signal handlers
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        # Setup HTTP routes
        self.setup_routes()
        self.http_thread = threading.Thread(target=self.run_http_server, name="HTTPServer", daemon=True)
        self.http_thread.start()
        self.threads.append(self.http_thread)

        # Device connection
        if self.device.connect():
            self.device_connected.set()
        else:
            logger.error("Failed to connect to device.")
            self.shifu_client.update_device_status("NotReady")
            return

        self.shifu_client.update_device_status("Ready")

        # Parse config
        self._parse_publish_intervals()

        # Start MQTT
        mqtt_thread = threading.Thread(target=self.connect_mqtt, name="MQTTConnector", daemon=True)
        mqtt_thread.start()
        self.threads.append(mqtt_thread)

        # Wait for MQTT connection before starting publishers
        if not self.mqtt_connected.wait(timeout=20):
            logger.error("MQTT connection could not be established. Exiting driver.")
            self.shifu_client.update_device_status("NotReady")
            shutdown_flag.set()
            self.shutdown()
            return
        self.shifu_client.update_device_status("Ready")

        # Start publishers
        self._start_scheduled_publishers()

        # Main loop: monitor threads and handle shutdown
        try:
            while not shutdown_flag.is_set():
                time.sleep(1)
        except Exception as e:
            logger.error(f"Main loop exception: {e}")
        finally:
            self.shutdown()

# --------------------- Entry Point ---------------------
if __name__ == "__main__":
    driver = DeviceShifuMQTTDriver()
    driver.run()