import os
import sys
import time
import json
import yaml
import signal
import logging
import threading
from typing import Dict, Any, Optional

from flask import Flask, jsonify
import paho.mqtt.client as mqtt

from kubernetes import client as k8s_client
from kubernetes import config as k8s_config
from kubernetes.client.rest import ApiException

# ===== Logging Configuration =====
LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=LOG_LEVEL,
    format="%(asctime)s [%(levelname)s] %(threadName)s - %(message)s",
)
logger = logging.getLogger(__name__)

# ===== Environment Variables =====
EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME", "deviceshifu-decoder")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE", "devices")
CONFIG_MOUNT_PATH = os.environ.get("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
MQTT_BROKER = os.environ.get("MQTT_BROKER", None)
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", 1883))
MQTT_BROKER_USERNAME = os.environ.get("MQTT_BROKER_USERNAME", "")
MQTT_BROKER_PASSWORD = os.environ.get("MQTT_BROKER_PASSWORD", "")
MQTT_TOPIC_PREFIX = os.environ.get("MQTT_TOPIC_PREFIX", "shifu")
HTTP_HOST = os.environ.get("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("HTTP_PORT", 8080))

# ===== Constants =====
INSTRUCTION_FILE = os.path.join(CONFIG_MOUNT_PATH, "instructions")
DRIVER_PROPERTIES_FILE = os.path.join(CONFIG_MOUNT_PATH, "driverProperties")
DEFAULT_PUBLISH_INTERVAL_MS = 5000

# MQTT Topics
TOPIC_MANAGE = "device/decoder/manage"
TOPIC_CONFIG = "device/decoder/config"
TOPIC_STATUS = "device/decoder/status"
TOPIC_DECODE = "device/decoder/decode"

# ======= ShifuClient Class =======
class ShifuClient:
    def __init__(self):
        self.edge_device_name = EDGEDEVICE_NAME
        self.edge_device_namespace = EDGEDEVICE_NAMESPACE
        self.config_mount_path = CONFIG_MOUNT_PATH
        self.k8s_api = None
        self._init_k8s_client()

    def _init_k8s_client(self):
        try:
            k8s_config.load_incluster_config()
            logger.info("Loaded in-cluster Kubernetes config.")
        except Exception as e:
            try:
                k8s_config.load_kube_config()
                logger.info("Loaded local kubeconfig for Kubernetes.")
            except Exception as ex:
                logger.error(f"Failed to load Kubernetes config: {ex}")
                self.k8s_api = None
                return
        self.k8s_api = k8s_client.CustomObjectsApi()

    def get_edge_device(self) -> Optional[Dict[str, Any]]:
        if not self.k8s_api:
            logger.warning("Kubernetes API is not initialized.")
            return None
        try:
            edge_device = self.k8s_api.get_namespaced_custom_object(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.edge_device_namespace,
                plural="edgedevices",
                name=self.edge_device_name,
            )
            logger.debug("EdgeDevice resource fetched from Kubernetes.")
            return edge_device
        except ApiException as e:
            logger.error(f"Failed to get EdgeDevice: {e}")
            return None

    def get_device_address(self) -> Optional[str]:
        edge_device = self.get_edge_device()
        if not edge_device:
            return None
        try:
            return edge_device["spec"]["address"]
        except Exception as e:
            logger.error(f"Failed to extract device address: {e}")
            return None

    def update_device_status(self, status: Dict[str, Any]) -> None:
        if not self.k8s_api:
            logger.warning("Kubernetes API is not initialized, cannot update status.")
            return
        body = {"status": status}
        try:
            self.k8s_api.patch_namespaced_custom_object_status(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.edge_device_namespace,
                plural="edgedevices",
                name=self.edge_device_name,
                body=body,
            )
            logger.info("EdgeDevice status updated in Kubernetes.")
        except ApiException as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Optional[Any]:
        try:
            with open(filename, "r") as f:
                config_data = yaml.safe_load(f)
            logger.debug(f"Config file {filename} loaded.")
            return config_data
        except Exception as e:
            logger.error(f"Failed to read config file {filename}: {e}")
            return None

    def get_instruction_config(self) -> Dict[str, Any]:
        instructions = self.read_mounted_config_file(INSTRUCTION_FILE)
        if not instructions:
            logger.warning("No instructions found in config file.")
            return {}
        return instructions

    def get_driver_properties(self) -> Dict[str, Any]:
        properties = self.read_mounted_config_file(DRIVER_PROPERTIES_FILE)
        if not properties:
            logger.warning("No driverProperties found in config file.")
            return {}
        return properties

# ======= DeviceShifuMQTTDriver Class =======
class DeviceShifuMQTTDriver:
    def __init__(self):
        # Device, MQTT, and config state
        self.shifu_client = ShifuClient()
        self.device_address = self.shifu_client.get_device_address()
        self.driver_properties = self.shifu_client.get_driver_properties()
        self.instruction_config = self.shifu_client.get_instruction_config()
        self.latest_data: Dict[str, Any] = {}
        self.publish_intervals: Dict[str, int] = {}
        self.mqtt_client = None
        self.mqtt_connected = False
        self.client_id = f"{EDGEDEVICE_NAME}-{int(time.time())}"
        self.shutdown_flag = threading.Event()
        self.threads = []
        self.flask_app = Flask(__name__)
        self._parse_publish_intervals()
        self._setup_routes()
        self.status_lock = threading.Lock()
        self.status_report = {
            "device": EDGEDEVICE_NAME,
            "status": "INIT",
            "mqtt": "DISCONNECTED",
            "device_address": self.device_address,
            "timestamp": int(time.time()),
        }

    # ========== Configuration Parsing ==========
    def _parse_publish_intervals(self):
        """
        Parse publishIntervalMS for each instruction from config.
        """
        try:
            instructions = self.instruction_config.get("instructions", {})
            for name, instr in instructions.items():
                publish_ms = int(instr.get("publishIntervalMS", DEFAULT_PUBLISH_INTERVAL_MS))
                self.publish_intervals[name] = publish_ms
            logger.info(f"Parsed publish intervals: {self.publish_intervals}")
        except Exception as e:
            logger.error(f"Error parsing publish intervals: {e}")

    # ========== Threaded Publishers ==========
    def _start_scheduled_publishers(self):
        """
        Start threads for each publisher instruction for periodic publishing.
        """
        instructions = self.instruction_config.get("instructions", {})
        for name, instr in instructions.items():
            mode = instr.get("mode", "publisher").lower()
            if mode == "publisher":
                t = threading.Thread(
                    target=self._publish_topic_periodically,
                    args=(name, instr),
                    name=f"Publisher-{name}",
                    daemon=True,
                )
                t.start()
                self.threads.append(t)
                logger.info(f"Started periodic publisher thread for instruction: {name}")

    def _publish_topic_periodically(self, instruction_name: str, instruction_cfg: Dict[str, Any]):
        """
        Periodically publish data for a given instruction.
        """
        topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{instruction_name}"
        publish_interval = int(instruction_cfg.get("publishIntervalMS", DEFAULT_PUBLISH_INTERVAL_MS)) / 1000.0
        qos = int(instruction_cfg.get("qos", 1))
        logger.debug(f"Publisher thread for {instruction_name} on topic {topic} every {publish_interval}s")
        while not self.shutdown_flag.is_set():
            try:
                # Simulate device data retrieval (replace with real SDK/RTSP calls)
                data = self.retrieve_device_data(instruction_name)
                self.latest_data[instruction_name] = data
                payload = json.dumps(data, default=str)
                if self.mqtt_connected:
                    self.mqtt_client.publish(topic, payload, qos=qos)
                    logger.info(f"Published {instruction_name} to {topic}: {payload}")
                else:
                    logger.warning(f"MQTT disconnected, not publishing {instruction_name}")
            except Exception as e:
                logger.error(f"Error publishing {instruction_name}: {e}")
            time.sleep(publish_interval)

    # ===== Device Data Retrieval Abstraction (Replace with actual SDK/RTSP calls) =====
    def retrieve_device_data(self, instruction_name: str) -> Dict[str, Any]:
        """
        Simulated device data retrieval for each instruction.
        Replace with actual interactions with the Hikvision Decoder device.
        """
        # Simulated data for demonstration purpose
        timestamp = int(time.time())
        if instruction_name == "status":
            return {
                "channels": [{"id": 1, "state": "active"}, {"id": 2, "state": "idle"}],
                "device_state": "online",
                "sdk_version": "5.3.0",
                "alarm": False,
                "timestamp": timestamp,
            }
        elif instruction_name == "decode":
            return {"decode_state": "stopped", "timestamp": timestamp}
        elif instruction_name == "manage":
            return {"last_action": "none", "result": "ok", "timestamp": timestamp}
        elif instruction_name == "config":
            return {"display_mode": "fullscreen", "window_count": 4, "timestamp": timestamp}
        else:
            return {"info": f"Unknown instruction {instruction_name}", "timestamp": timestamp}

    # ========== MQTT Connection ==========
    def connect_mqtt(self):
        """
        Initialize and connect the MQTT client.
        """
        if not MQTT_BROKER:
            logger.error("MQTT_BROKER not set. Exiting.")
            sys.exit(1)
        self.mqtt_client = mqtt.Client(client_id=self.client_id, clean_session=True)
        if MQTT_BROKER_USERNAME:
            self.mqtt_client.username_pw_set(MQTT_BROKER_USERNAME, MQTT_BROKER_PASSWORD)
        self.mqtt_client.on_connect = self.on_connect
        self.mqtt_client.on_message = self.on_message
        self.mqtt_client.on_disconnect = self.on_disconnect
        # Optional: Configure TLS here if needed

        # Start MQTT network loop in background thread
        try:
            self.mqtt_client.connect(MQTT_BROKER, MQTT_BROKER_PORT, keepalive=60)
            t = threading.Thread(target=self.mqtt_client.loop_forever, name="MQTTLoop", daemon=True)
            t.start()
            self.threads.append(t)
            logger.info(f"MQTT client {self.client_id} connecting to {MQTT_BROKER}:{MQTT_BROKER_PORT}")
        except Exception as e:
            logger.error(f"Failed to connect to MQTT broker: {e}")
            self.mqtt_connected = False

    def on_connect(self, client, userdata, flags, rc):
        """
        MQTT on_connect callback.
        """
        if rc == 0:
            self.mqtt_connected = True
            logger.info("Connected to MQTT broker.")
            # Subscribe to control topics
            control_topics = [
                f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/control/+",
                f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{TOPIC_MANAGE}",
                f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{TOPIC_CONFIG}",
                f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{TOPIC_DECODE}",
            ]
            for topic in control_topics:
                self.mqtt_client.subscribe(topic, qos=1)
                logger.info(f"Subscribed to MQTT topic {topic}")
            # Subscribe to status topic for SUBSCRIBE instructions
            self.mqtt_client.subscribe(f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{TOPIC_STATUS}", qos=1)
            logger.info(f"Subscribed to device status topic {TOPIC_STATUS}")

            # Update status
            with self.status_lock:
                self.status_report["status"] = "RUNNING"
                self.status_report["mqtt"] = "CONNECTED"
                self.status_report["timestamp"] = int(time.time())
        else:
            logger.error(f"MQTT connection failed with code {rc}")
            with self.status_lock:
                self.status_report["status"] = "ERROR"
                self.status_report["mqtt"] = "CONNECTION_FAILED"
                self.status_report["timestamp"] = int(time.time())

    def on_disconnect(self, client, userdata, rc):
        """
        MQTT on_disconnect callback.
        """
        self.mqtt_connected = False
        logger.warning(f"Disconnected from MQTT broker (rc={rc}).")
        with self.status_lock:
            self.status_report["status"] = "DISCONNECTED"
            self.status_report["mqtt"] = "DISCONNECTED"
            self.status_report["timestamp"] = int(time.time())
        # Reconnect logic
        if not self.shutdown_flag.is_set():
            logger.info("Attempting MQTT reconnection in 5 seconds...")
            time.sleep(5)
            try:
                self.mqtt_client.reconnect()
            except Exception as e:
                logger.error(f"MQTT reconnection failed: {e}")

    def on_message(self, client, userdata, msg):
        """
        MQTT on_message callback.
        """
        try:
            topic = msg.topic
            payload = msg.payload.decode("utf-8")
            logger.info(f"Received MQTT message on {topic}: {payload}")
            # Parse topic for control commands
            if topic.endswith("/control/manage") or topic.endswith(f"/{TOPIC_MANAGE}"):
                self.handle_manage_command(payload)
            elif topic.endswith("/control/config") or topic.endswith(f"/{TOPIC_CONFIG}"):
                self.handle_config_command(payload)
            elif topic.endswith("/control/decode") or topic.endswith(f"/{TOPIC_DECODE}"):
                self.handle_decode_command(payload)
            else:
                logger.warning(f"Received message on unhandled topic: {topic}")
        except Exception as e:
            logger.error(f"Error handling MQTT message: {e}")

    # ====== Control Command Handlers ======
    def handle_manage_command(self, payload: str):
        """
        Handle device management commands.
        """
        try:
            cmd = json.loads(payload)
            # Simulate handling management command (e.g., reboot, shutdown)
            result = {"action": cmd.get("action", "unknown"), "result": "success", "timestamp": int(time.time())}
            self.latest_data["manage"] = result
            # Publish response to status topic
            status_topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{TOPIC_STATUS}"
            self.publish_status_update(result)
            logger.info(f"Handled manage command: {cmd}")
        except Exception as e:
            logger.error(f"Failed to handle manage command: {e}")

    def handle_config_command(self, payload: str):
        """
        Handle configuration commands.
        """
        try:
            cfg = json.loads(payload)
            # Simulate applying configuration
            result = {"config": cfg, "result": "applied", "timestamp": int(time.time())}
            self.latest_data["config"] = result
            self.publish_status_update(result)
            logger.info(f"Handled config command: {cfg}")
        except Exception as e:
            logger.error(f"Failed to handle config command: {e}")

    def handle_decode_command(self, payload: str):
        """
        Handle decode commands (start/stop).
        """
        try:
            cmd = json.loads(payload)
            # Simulate decode action
            result = {"decode_action": cmd.get("action", "unknown"), "result": "executed", "timestamp": int(time.time())}
            self.latest_data["decode"] = result
            self.publish_status_update(result)
            logger.info(f"Handled decode command: {cmd}")
        except Exception as e:
            logger.error(f"Failed to handle decode command: {e}")

    # ====== Status Publishing ======
    def publish_status_update(self, data: Dict[str, Any]):
        """
        Publish status update to the status topic.
        """
        if not self.mqtt_connected:
            logger.warning("Cannot publish status update: MQTT disconnected.")
            return
        try:
            topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{TOPIC_STATUS}"
            payload = json.dumps(data, default=str)
            self.mqtt_client.publish(topic, payload, qos=1)
            logger.info(f"Published status update to {topic}: {payload}")
        except Exception as e:
            logger.error(f"Failed to publish status update: {e}")

    # ========== HTTP Server ==========
    def _setup_routes(self):
        @self.flask_app.route("/health", methods=["GET"])
        def health():
            return jsonify({"status": "healthy"}), 200

        @self.flask_app.route("/status", methods=["GET"])
        def status():
            with self.status_lock:
                status_copy = dict(self.status_report)
            return jsonify(status_copy), 200

    def _start_http_server(self):
        """
        Start Flask HTTP server in a background thread.
        """
        t = threading.Thread(
            target=lambda: self.flask_app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True, use_reloader=False),
            name="HTTPServer",
            daemon=True,
        )
        t.start()
        self.threads.append(t)
        logger.info(f"HTTP server started on {HTTP_HOST}:{HTTP_PORT}")

    # ========== Signal & Shutdown Handler ==========
    def signal_handler(self, signum, frame):
        logger.info(f"Received signal {signum}. Initiating graceful shutdown.")
        self.shutdown()

    def shutdown(self):
        """
        Graceful shutdown logic.
        """
        if self.shutdown_flag.is_set():
            return
        logger.info("Shutting down DeviceShifuMQTTDriver...")
        self.shutdown_flag.set()
        # MQTT disconnect
        if self.mqtt_client and self.mqtt_connected:
            try:
                self.mqtt_client.disconnect()
                logger.info("MQTT client disconnected.")
            except Exception as e:
                logger.error(f"Failed to disconnect MQTT client: {e}")
        # Wait for all threads to finish
        for t in self.threads:
            if t.is_alive():
                t.join(timeout=5)
        logger.info("All background threads terminated. Shutdown complete.")
        sys.exit(0)

    # ========== Main Run ==========
    def run(self):
        # Signal registration
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        # Start HTTP server for health checks
        self._start_http_server()

        # Connect MQTT
        self.connect_mqtt()

        # Start periodic publishers
        self._start_scheduled_publishers()

        # Device status update loop (every 30s)
        try:
            while not self.shutdown_flag.is_set():
                # Update EdgeDevice status in Kubernetes
                with self.status_lock:
                    self.status_report["timestamp"] = int(time.time())
                self.shifu_client.update_device_status(self.status_report)
                time.sleep(30)
        except Exception as e:
            logger.error(f"Error in main loop: {e}")
        finally:
            self.shutdown()

# ========== Main Entrypoint ==========
if __name__ == "__main__":
    driver = DeviceShifuMQTTDriver()
    driver.run()