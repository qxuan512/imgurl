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

from kubernetes import client as k8s_client, config as k8s_config
from kubernetes.client.rest import ApiException

# =========================
# Logging Configuration
# =========================

LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL, logging.INFO),
    format="%(asctime)s [%(levelname)s] %(threadName)s: %(message)s",
)
logger = logging.getLogger(__name__)

# =========================
# ShifuClient Class
# =========================

class ShifuClient:
    def __init__(self):
        self.edge_device_name = os.getenv("EDGEDEVICE_NAME", "deviceshifu-networkvideodecoder")
        self.edge_device_namespace = os.getenv("EDGEDEVICE_NAMESPACE", "devices")
        self.config_mount_path = os.getenv("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
        self._init_k8s_client()

    def _init_k8s_client(self) -> None:
        try:
            if os.path.exists("/var/run/secrets/kubernetes.io/serviceaccount/token"):
                k8s_config.load_incluster_config()
                logger.info("Loaded in-cluster Kubernetes config.")
            else:
                k8s_config.load_kube_config()
                logger.info("Loaded local kubeconfig.")
            self.api = k8s_client.CustomObjectsApi()
        except Exception as e:
            logger.error(f"Kubernetes client initialization failed: {e}")
            self.api = None

    def get_edge_device(self) -> Optional[Dict[str, Any]]:
        if not self.api:
            logger.error("Kubernetes API not initialized.")
            return None
        try:
            ed = self.api.get_namespaced_custom_object(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.edge_device_namespace,
                plural="edgedevices",
                name=self.edge_device_name
            )
            logger.debug(f"Fetched EdgeDevice resource: {ed}")
            return ed
        except ApiException as e:
            logger.error(f"Failed to get EdgeDevice: {e}")
            return None

    def get_device_address(self) -> Optional[str]:
        ed = self.get_edge_device()
        if ed:
            try:
                return ed.get("spec", {}).get("address", None)
            except Exception as e:
                logger.error(f"Error extracting device address: {e}")
        return None

    def update_device_status(self, new_status: Dict[str, Any]) -> None:
        if not self.api:
            logger.error("Kubernetes API not initialized, cannot update status.")
            return
        try:
            self.api.patch_namespaced_custom_object_status(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.edge_device_namespace,
                plural="edgedevices",
                name=self.edge_device_name,
                body={"status": new_status}
            )
            logger.debug("EdgeDevice status updated.")
        except ApiException as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Optional[Any]:
        path = os.path.join(self.config_mount_path, filename)
        try:
            with open(path, "r") as f:
                if filename.endswith(".yaml") or filename.endswith(".yml"):
                    data = yaml.safe_load(f)
                else:
                    data = f.read()
                logger.debug(f"Read config file {filename}: {data}")
                return data
        except Exception as e:
            logger.error(f"Error reading config file {filename}: {e}")
            return None

    def get_instruction_config(self) -> Dict[str, Any]:
        data = self.read_mounted_config_file("instructions")
        if data is None:
            logger.warning("No instruction config found.")
            return {}
        return data

    def get_driver_properties(self) -> Dict[str, Any]:
        data = self.read_mounted_config_file("driverProperties")
        if data is None:
            logger.warning("No driverProperties config found.")
            return {}
        return data

# =========================
# DeviceShifuMQTTDriver Class
# =========================

class DeviceShifuMQTTDriver:
    def __init__(self):
        # Environment Variables
        self.device_type = "networkvideodecoder"
        self.device_name = os.getenv("EDGEDEVICE_NAME", "deviceshifu-networkvideodecoder")
        self.namespace = os.getenv("EDGEDEVICE_NAMESPACE", "devices")
        self.mqtt_broker = os.getenv("MQTT_BROKER", "localhost")
        self.mqtt_port = int(os.getenv("MQTT_BROKER_PORT", 1883))
        self.mqtt_user = os.getenv("MQTT_BROKER_USERNAME")
        self.mqtt_pass = os.getenv("MQTT_BROKER_PASSWORD")
        self.mqtt_topic_prefix = os.getenv("MQTT_TOPIC_PREFIX", "shifu")
        self.http_host = os.getenv("HTTP_HOST", "0.0.0.0")
        self.http_port = int(os.getenv("HTTP_PORT", 8080))

        # ShifuClient
        self.shifu_client = ShifuClient()

        # MQTT
        self.mqtt_client: Optional[mqtt.Client] = None
        self.mqtt_connected = threading.Event()
        self.mqtt_client_id = f"{self.device_name}-{int(time.time())}"
        self.mqtt_qos = 1

        # Device Data
        self.latest_data: Dict[str, Any] = {}
        self.shutdown_flag = threading.Event()
        self.threads: list = []
        self.instruction_config: Dict[str, Any] = {}
        self.publish_intervals: Dict[str, int] = {}

        # HTTP Server
        self.app = Flask(__name__)
        self.http_thread = None

        # Device connection (stub - replace with actual device integration as needed)
        self.device_connected = True

        self._init_driver()

    def _init_driver(self) -> None:
        self.instruction_config = self.shifu_client.get_instruction_config()
        self.driver_properties = self.shifu_client.get_driver_properties()
        self._parse_publish_intervals()
        self.setup_routes()

    def _parse_publish_intervals(self) -> None:
        self.publish_intervals = {}
        if not self.instruction_config:
            logger.warning("No instruction configuration for publish intervals.")
            return
        try:
            for instr_name, instr_cfg in self.instruction_config.items():
                if isinstance(instr_cfg, dict):
                    mode = instr_cfg.get("protocolProperty", {}).get("mode", "").lower()
                    interval = instr_cfg.get("protocolProperty", {}).get("publishIntervalMS", 0)
                    if mode == "publisher" and interval:
                        self.publish_intervals[instr_name] = int(interval)
            logger.info(f"Parsed publish intervals: {self.publish_intervals}")
        except Exception as e:
            logger.error(f"Error parsing publish intervals: {e}")

    def _start_scheduled_publishers(self):
        for instr_name, interval in self.publish_intervals.items():
            t = threading.Thread(
                target=self._publish_topic_periodically,
                args=(instr_name, interval),
                name=f"Publisher-{instr_name}",
                daemon=True
            )
            t.start()
            self.threads.append(t)

    def _publish_topic_periodically(self, instr_name: str, interval_ms: int) -> None:
        topic = f"{self.mqtt_topic_prefix}/{self.device_name}/{instr_name}"
        while not self.shutdown_flag.is_set():
            try:
                # Simulate device data retrieval
                data = self._get_device_data(instr_name)
                payload = json.dumps(data, default=str)
                result = self.mqtt_client.publish(topic, payload, qos=self.mqtt_qos)
                logger.info(
                    f"Published to {topic}: {payload} (result: {result.rc})"
                )
                self.latest_data[instr_name] = data
            except Exception as e:
                logger.error(f"Error in periodic publishing for {instr_name}: {e}")
            time.sleep(interval_ms / 1000.0)

    def _get_device_data(self, instr_name: str) -> Dict[str, Any]:
        # Simulate device-specific data; in production, integrate with device SDK/RTSP/XML
        now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        if instr_name == "status_report" or instr_name == "device/telemetry/status":
            return {
                "timestamp": now,
                "device_status": "OK" if self.device_connected else "DISCONNECTED",
                "channels": 4,
                "decoding": True,
                "error_code": 0,
                "upgrade_progress": 0,
                "alarm": False,
                "sdk_version": "5.3.8",
            }
        elif instr_name == "cmd_vel_echo":
            return {
                "timestamp": now,
                "echo": "cmd_vel received"
            }
        else:
            # Default stub data for other instructions
            return {
                "timestamp": now,
                "instruction": instr_name,
                "status": "success"
            }

    def connect_mqtt(self):
        self.mqtt_client = mqtt.Client(client_id=self.mqtt_client_id, clean_session=True)
        if self.mqtt_user:
            self.mqtt_client.username_pw_set(self.mqtt_user, self.mqtt_pass)
        self.mqtt_client.on_connect = self.on_connect
        self.mqtt_client.on_disconnect = self.on_disconnect
        self.mqtt_client.on_message = self.on_message
        self.mqtt_client.on_subscribe = self.on_subscribe

        while not self.shutdown_flag.is_set():
            try:
                logger.info(f"Connecting to MQTT {self.mqtt_broker}:{self.mqtt_port} as {self.mqtt_client_id}")
                self.mqtt_client.connect(self.mqtt_broker, self.mqtt_port, keepalive=60)
                self.mqtt_client.loop_start()
                if self.mqtt_connected.wait(timeout=10):
                    logger.info("MQTT connected.")
                    break
                else:
                    logger.warning("MQTT connect timeout, retrying in 5s.")
            except Exception as e:
                logger.error(f"MQTT connection error: {e}, retrying in 5s.")
            time.sleep(5)

        # Subscribe to control topics (e.g., reboot, config)
        control_topics = [
            f"{self.mqtt_topic_prefix}/{self.device_name}/control/reboot",
            f"{self.mqtt_topic_prefix}/{self.device_name}/control/config"
        ]
        for topic in control_topics:
            try:
                self.mqtt_client.subscribe(topic, qos=self.mqtt_qos)
                logger.info(f"Subscribed to control topic: {topic}")
            except Exception as e:
                logger.error(f"Failed to subscribe to topic {topic}: {e}")

        # Subscribe to telemetry status topic if required
        telemetry_topic = f"{self.mqtt_topic_prefix}/{self.device_name}/device/telemetry/status"
        try:
            self.mqtt_client.subscribe(telemetry_topic, qos=self.mqtt_qos)
            logger.info(f"Subscribed to telemetry topic: {telemetry_topic}")
        except Exception as e:
            logger.error(f"Failed to subscribe to telemetry topic {telemetry_topic}: {e}")

    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.mqtt_connected.set()
            logger.info("Connected to MQTT broker.")
            self._publish_status("online")
        else:
            logger.error(f"MQTT connection failed with code {rc}")

    def on_disconnect(self, client, userdata, rc):
        self.mqtt_connected.clear()
        logger.warning(f"Disconnected from MQTT broker with code {rc}")
        if not self.shutdown_flag.is_set():
            logger.info("Attempting MQTT reconnect in 5s...")
            time.sleep(5)
            self.connect_mqtt()

    def on_subscribe(self, client, userdata, mid, granted_qos):
        logger.debug(f"Subscribed (mid={mid}) with QoS {granted_qos}")

    def on_message(self, client, userdata, msg):
        logger.info(f"Received MQTT message on {msg.topic}: {msg.payload}")
        try:
            self.handle_mqtt_message(msg.topic, msg.payload)
        except Exception as e:
            logger.error(f"Exception in message handler: {e}")

    def handle_mqtt_message(self, topic: str, payload: bytes):
        try:
            payload_json = json.loads(payload.decode())
        except Exception:
            payload_json = {"raw": payload.decode(errors="ignore")}
        # Command handling
        if topic.endswith("/control/reboot"):
            self._handle_reboot_command(payload_json)
        elif topic.endswith("/control/config"):
            self._handle_config_command(payload_json)
        elif topic.endswith("device/telemetry/status"):
            self._handle_status_subscribe(payload_json)
        else:
            logger.debug(f"No handler for topic {topic}")

    def _handle_reboot_command(self, payload: Dict[str, Any]) -> None:
        logger.info(f"Handling reboot command: {payload}")
        # Simulate reboot: Mark device as rebooting, then online
        self._publish_status("rebooting")
        time.sleep(2)
        self._publish_status("online")
        logger.info("Device rebooted.")

    def _handle_config_command(self, payload: Dict[str, Any]) -> None:
        logger.info(f"Handling config command: {payload}")
        # Simulate config actions
        action = payload.get("action")
        response = {"result": "no_action"}
        if action == "get":
            response = {
                "status": "success",
                "config": self.driver_properties
            }
        elif action == "set":
            # Example: Update driverProperties (stub, no writeback)
            response = {"status": "success", "msg": "Config updated (stub)"}
        else:
            response = {"status": "failed", "msg": "Unknown action"}
        topic = f"{self.mqtt_topic_prefix}/{self.device_name}/control/config/response"
        try:
            self.mqtt_client.publish(topic, json.dumps(response), qos=self.mqtt_qos)
            logger.info(f"Published config response to {topic}: {response}")
        except Exception as e:
            logger.error(f"Failed to publish config response: {e}")

    def _handle_status_subscribe(self, payload: Dict[str, Any]) -> None:
        logger.info(f"Received status subscribe payload: {payload}")
        # Optionally send latest status
        status = self._get_device_data("device/telemetry/status")
        topic = f"{self.mqtt_topic_prefix}/{self.device_name}/device/telemetry/status"
        try:
            self.mqtt_client.publish(topic, json.dumps(status), qos=self.mqtt_qos)
            logger.info(f"Sent device status to {topic}")
        except Exception as e:
            logger.error(f"Failed to publish status: {e}")

    def _publish_status(self, status: str) -> None:
        topic = f"{self.mqtt_topic_prefix}/{self.device_name}/status"
        status_payload = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "status": status
        }
        try:
            self.mqtt_client.publish(topic, json.dumps(status_payload), qos=self.mqtt_qos)
            logger.info(f"Published device status: {status_payload}")
        except Exception as e:
            logger.error(f"Failed to publish status: {e}")

    def setup_routes(self):
        @self.app.route('/health', methods=['GET'])
        def health():
            return jsonify({"status": "ok"}), 200

        @self.app.route('/status', methods=['GET'])
        def status():
            status = {
                "device": self.device_name,
                "connected": self.device_connected,
                "mqtt_connected": self.mqtt_connected.is_set(),
                "latest_data": self.latest_data,
            }
            return jsonify(status), 200

    def start_http_server(self):
        try:
            logger.info(f"Starting HTTP server on {self.http_host}:{self.http_port}")
            self.app.run(host=self.http_host, port=self.http_port, threaded=True, use_reloader=False)
        except Exception as e:
            logger.error(f"HTTP server error: {e}")

    def signal_handler(self, signum, frame):
        logger.info(f"Caught signal {signum}, initiating shutdown.")
        self.shutdown()

    def shutdown(self):
        if not self.shutdown_flag.is_set():
            self.shutdown_flag.set()
            logger.info("Shutting down DeviceShifu driver...")
            try:
                if self.mqtt_client:
                    self._publish_status("offline")
                    self.mqtt_client.disconnect()
                    self.mqtt_client.loop_stop()
            except Exception as e:
                logger.error(f"Error during MQTT shutdown: {e}")
            # Threads cleanup
            logger.info("Waiting for threads to finish...")
            for t in self.threads:
                if t.is_alive():
                    t.join(timeout=3)
            logger.info("Shutdown complete.")
            sys.exit(0)

    def run(self):
        # Signal handlers
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        # Start HTTP server in background
        self.http_thread = threading.Thread(target=self.start_http_server, name="HTTPServer", daemon=True)
        self.http_thread.start()
        self.threads.append(self.http_thread)

        # Connect to MQTT
        self.connect_mqtt()

        # Start periodic data publishers
        self._start_scheduled_publishers()

        # Main loop: monitor threads and keep alive
        try:
            while not self.shutdown_flag.is_set():
                if self.http_thread and not self.http_thread.is_alive():
                    logger.error("HTTP server thread stopped, but continuing MQTT operation.")
                if not self.mqtt_connected.is_set():
                    logger.warning("MQTT not connected, attempting reconnect.")
                    self.connect_mqtt()
                time.sleep(2)
        except Exception as e:
            logger.error(f"Error in main loop: {e}")
        finally:
            self.shutdown()

# =========================
# Main Entrypoint
# =========================

if __name__ == "__main__":
    driver = DeviceShifuMQTTDriver()
    driver.run()