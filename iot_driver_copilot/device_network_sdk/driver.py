import os
import sys
import json
import yaml
import time
import logging
import threading
import signal
from typing import Dict, Any, Optional

import paho.mqtt.client as mqtt
from flask import Flask, jsonify
from kubernetes import client, config, config as k8s_config
from kubernetes.client.rest import ApiException

# Logging configuration
LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    stream=sys.stdout,
    level=LOG_LEVEL,
    format="%(asctime)s %(levelname)s %(threadName)s %(message)s"
)
logger = logging.getLogger("deviceshifu-mqtt-driver")

# Environment variables & defaults
EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME", "deviceshifu-decoder")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE", "devices")
CONFIG_MOUNT_PATH = os.environ.get("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
MQTT_BROKER = os.environ.get("MQTT_BROKER", "localhost")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
MQTT_BROKER_USERNAME = os.environ.get("MQTT_BROKER_USERNAME")
MQTT_BROKER_PASSWORD = os.environ.get("MQTT_BROKER_PASSWORD")
MQTT_TOPIC_PREFIX = os.environ.get("MQTT_TOPIC_PREFIX", "shifu")
HTTP_HOST = os.environ.get("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8080"))

INSTRUCTION_CONFIG_PATH = os.path.join(CONFIG_MOUNT_PATH, "instructions")
DRIVER_PROPERTIES_PATH = os.path.join(CONFIG_MOUNT_PATH, "driverProperties")

# Global shutdown flag for all threads
shutdown_flag = threading.Event()

# Device-specific placeholder for SDK/API connection
class DeviceConnection:
    def __init__(self, device_address: str):
        self.device_address = device_address
        self.connected = False

    def connect(self) -> bool:
        # Implement actual device SDK connection here
        logger.info(f"Connecting to device at {self.device_address} ...")
        try:
            # Simulate a connection
            time.sleep(1)
            self.connected = True
            logger.info("Device connected successfully.")
            return True
        except Exception as e:
            logger.error(f"Device connection failed: {e}")
            self.connected = False
            return False

    def disconnect(self):
        logger.info("Disconnecting device...")
        self.connected = False

    def get_status(self) -> Dict[str, Any]:
        # Placeholder: Replace with actual device status read
        return {
            "status": "online" if self.connected else "offline",
            "error_code": 0 if self.connected else 1,
            "timestamp": int(time.time())
        }

    def get_telemetry_info(self) -> Dict[str, Any]:
        # Placeholder: Replace with real device telemetry info
        return {
            "version": "1.0.0",
            "abilities": {
                "channels": 16,
                "streams": 4,
                "scene_modes": ["normal", "cinema", "presentation"]
            },
            "config": {
                "ip": self.device_address,
                "model": "DS-6400HD"
            }
        }

    def execute_command(self, command: str, params: Dict[str, Any]) -> Dict[str, Any]:
        # Placeholder: Implement command execution against device SDK
        logger.info(f"Executing device command: {command}, params: {params}")
        # Simulate success/failure
        return {
            "result": "success",
            "command": command,
            "params": params,
            "timestamp": int(time.time())
        }

    def login(self, username: str, password: str) -> Dict[str, Any]:
        # Placeholder: Implement login against device
        logger.info(f"Device login with username: {username}")
        # Simulate login
        if username and password:
            return {"result": "success", "timestamp": int(time.time())}
        else:
            return {"result": "failure", "timestamp": int(time.time())}


class ShifuClient:
    def __init__(self, device_name: str, namespace: str, config_mount_path: str):
        self.device_name = device_name
        self.namespace = namespace
        self.config_mount_path = config_mount_path
        self.api_instance = None
        self.custom_api_instance = None
        self._init_k8s_client()

    def _init_k8s_client(self):
        try:
            config.load_incluster_config()
            logger.info("Loaded in-cluster Kubernetes config.")
        except config.ConfigException:
            try:
                k8s_config.load_kube_config()
                logger.info("Loaded local kubeconfig.")
            except Exception as e:
                logger.error(f"Failed to load Kubernetes config: {e}")
                self.api_instance = None
                self.custom_api_instance = None
                return
        self.api_instance = client.CoreV1Api()
        self.custom_api_instance = client.CustomObjectsApi()

    def get_edge_device(self) -> Optional[Dict[str, Any]]:
        if not self.custom_api_instance:
            logger.warning("Kubernetes custom API not initialized.")
            return None
        try:
            device = self.custom_api_instance.get_namespaced_custom_object(
                group="deviceshifu.edgedevice.shifudev",
                version="v1alpha1",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.device_name
            )
            logger.debug(f"Retrieved EdgeDevice resource: {device}")
            return device
        except ApiException as e:
            logger.error(f"Failed to get EdgeDevice CR: {e}")
            return None

    def get_device_address(self) -> Optional[str]:
        device = self.get_edge_device()
        if device and 'spec' in device and 'address' in device['spec']:
            return device['spec']['address']
        logger.warning("Device address not found in EdgeDevice resource.")
        return None

    def update_device_status(self, status: str):
        if not self.custom_api_instance:
            logger.warning("Kubernetes custom API not initialized.")
            return
        try:
            body = {
                "status": status
            }
            self.custom_api_instance.patch_namespaced_custom_object_status(
                group="deviceshifu.edgedevice.shifudev",
                version="v1alpha1",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.device_name,
                body={"status": body}
            )
            logger.info(f"Updated device status to '{status}' in EdgeDevice CR.")
        except ApiException as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Dict[str, Any]:
        path = os.path.join(self.config_mount_path, filename)
        try:
            with open(path, "r") as f:
                content = yaml.safe_load(f)
                logger.info(f"Read config file {filename} successfully.")
                return content if content else {}
        except Exception as e:
            logger.error(f"Failed to read config file {filename}: {e}")
            return {}

    def get_instruction_config(self) -> Dict[str, Any]:
        return self.read_mounted_config_file("instructions")


class DeviceShifuMQTTDriver:
    def __init__(self):
        self.edge_device_name = EDGEDEVICE_NAME
        self.edge_device_namespace = EDGEDEVICE_NAMESPACE
        self.config_mount_path = CONFIG_MOUNT_PATH
        self.latest_data: Dict[str, Any] = {}
        self.publish_intervals: Dict[str, int] = {}
        self.threads: Dict[str, threading.Thread] = {}
        self.mqtt_client: Optional[mqtt.Client] = None
        self.mqtt_connected = False
        self.app = Flask(__name__)
        self.shifu_client = ShifuClient(self.edge_device_name, self.edge_device_namespace, self.config_mount_path)
        self.device_address = self.shifu_client.get_device_address() or "127.0.0.1"
        self.device_connection = DeviceConnection(self.device_address)
        self.device_online = False
        self._setup_routes()
        self._parse_publish_intervals()
        self.status_report_interval = 10  # seconds

    def _parse_publish_intervals(self):
        instructions = self.shifu_client.get_instruction_config()
        # Example YAML:
        # instruction:
        #   device/telemetry/info:
        #     protocolProperty:
        #       mode: publisher
        #       publishIntervalMS: 10000
        #   device/telemetry/status:
        #     protocolProperty:
        #       mode: publisher
        #       publishIntervalMS: 5000
        if not instructions or "instruction" not in instructions:
            logger.warning("No instruction config found.")
            return
        for instr, details in instructions["instruction"].items():
            protocol_prop = details.get("protocolProperty", {})
            mode = protocol_prop.get("mode", "publisher")
            if mode == "publisher":
                interval_ms = int(protocol_prop.get("publishIntervalMS", 10000))
                self.publish_intervals[instr] = interval_ms
                logger.info(f"Instruction {instr}: publish interval {interval_ms} ms.")

    def _start_scheduled_publishers(self):
        for topic, interval_ms in self.publish_intervals.items():
            th = threading.Thread(
                target=self._publish_topic_periodically,
                args=(topic, interval_ms),
                name=f"Publisher-{topic}",
                daemon=True
            )
            self.threads[topic] = th
            th.start()
            logger.info(f"Started publisher thread for topic: {topic}")

        # Status thread
        status_thread = threading.Thread(
            target=self._publish_status_periodically,
            args=(),
            name="StatusPublisher",
            daemon=True
        )
        self.threads["status_report"] = status_thread
        status_thread.start()
        logger.info("Started status report thread.")

    def _publish_topic_periodically(self, topic: str, interval_ms: int):
        while not shutdown_flag.is_set():
            try:
                payload = None
                if topic == "device/telemetry/status":
                    payload = self.device_connection.get_status()
                elif topic == "device/telemetry/info":
                    payload = self.device_connection.get_telemetry_info()
                else:
                    # Fallback for user-defined topics
                    payload = self.latest_data.get(topic, {"timestamp": int(time.time())})

                self.latest_data[topic] = payload
                self.publish_mqtt_topic(topic, payload)
            except Exception as e:
                logger.error(f"Error publishing topic '{topic}': {e}")
            time.sleep(interval_ms / 1000.0)

    def _publish_status_periodically(self):
        status_topic = f"{MQTT_TOPIC_PREFIX}/{self.edge_device_name}/status"
        while not shutdown_flag.is_set():
            try:
                status = self.device_connection.get_status()
                self.latest_data["status_report"] = status
                self.publish_mqtt_topic(status_topic, status, qos=1)
                # Periodically update EdgeDevice CR status as well
                self.shifu_client.update_device_status(status.get("status", "unknown"))
            except Exception as e:
                logger.error(f"Error in status report: {e}")
            time.sleep(self.status_report_interval)

    def connect_mqtt(self):
        client_id = f"{self.edge_device_name}-{int(time.time())}"
        self.mqtt_client = mqtt.Client(client_id=client_id, clean_session=True)
        if MQTT_BROKER_USERNAME and MQTT_BROKER_PASSWORD:
            self.mqtt_client.username_pw_set(MQTT_BROKER_USERNAME, MQTT_BROKER_PASSWORD)
        self.mqtt_client.on_connect = self._on_mqtt_connect
        self.mqtt_client.on_disconnect = self._on_mqtt_disconnect
        self.mqtt_client.on_message = self._on_mqtt_message
        self.mqtt_client.on_subscribe = self._on_mqtt_subscribe

        # Reconnection loop
        while not shutdown_flag.is_set():
            try:
                self.mqtt_client.connect(MQTT_BROKER, MQTT_BROKER_PORT, keepalive=60)
                self.mqtt_client.loop_start()
                logger.info(f"MQTT client connecting to {MQTT_BROKER}:{MQTT_BROKER_PORT}")
                # Wait for on_connect to complete
                for _ in range(10):
                    if self.mqtt_connected:
                        break
                    time.sleep(1)
                if not self.mqtt_connected:
                    logger.warning("MQTT not connected after 10s, retrying...")
                    self.mqtt_client.loop_stop()
                    time.sleep(5)
                    continue
                return
            except Exception as e:
                logger.error(f"MQTT connection failed: {e}")
                time.sleep(5)  # Retry after 5 seconds

    def _on_mqtt_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.mqtt_connected = True
            logger.info("Connected to MQTT broker.")
            # Subscribe to control command and login topics
            control_topic = f"{MQTT_TOPIC_PREFIX}/{self.edge_device_name}/control/#"
            self.mqtt_client.subscribe(control_topic, qos=1)
            logger.info(f"Subscribed to control topic: {control_topic}")
        else:
            logger.error(f"Failed to connect to MQTT broker, rc={rc}")

    def _on_mqtt_disconnect(self, client, userdata, rc):
        self.mqtt_connected = False
        logger.warning(f"Disconnected from MQTT broker with rc={rc}. Retrying...")
        # Reconnection handled in connect_mqtt loop

    def _on_mqtt_subscribe(self, client, userdata, mid, granted_qos):
        logger.info(f"MQTT subscription established: mid={mid}, qos={granted_qos}")

    def _on_mqtt_message(self, client, userdata, msg):
        try:
            logger.info(f"Received MQTT message: topic={msg.topic}, payload={msg.payload}")
            topic = msg.topic
            payload = json.loads(msg.payload.decode('utf-8'))
            if topic.endswith("/control/login"):
                username = payload.get("username")
                password = payload.get("password")
                resp = self.device_connection.login(username, password)
                self.publish_mqtt_topic(topic.replace("/control/login", "/control/login/response"), resp, qos=1)
            elif "/control/" in topic:
                # Extract command from topic
                command = topic.split("/control/")[-1]
                resp = self.device_connection.execute_command(command, payload)
                self.publish_mqtt_topic(topic + "/response", resp, qos=1)
            else:
                # Store as latest data for telemetry
                self.latest_data[topic] = payload
                logger.info(f"Updated latest_data for topic {topic}")
        except Exception as e:
            logger.error(f"Error processing MQTT message: {e}")

    def publish_mqtt_topic(self, topic_suffix: str, payload: Any, qos: int = 1):
        if not self.mqtt_connected or not self.mqtt_client:
            logger.warning("MQTT not connected, cannot publish.")
            return
        if topic_suffix.startswith(MQTT_TOPIC_PREFIX + "/"):
            topic = topic_suffix
        else:
            topic = f"{MQTT_TOPIC_PREFIX}/{self.edge_device_name}/{topic_suffix.strip('/')}"
        try:
            payload_json = json.dumps(payload, default=str)
            self.mqtt_client.publish(topic, payload=payload_json, qos=qos, retain=False)
            logger.info(f"Published to MQTT topic '{topic}' payload: {payload_json}")
        except Exception as e:
            logger.error(f"Failed to publish MQTT topic '{topic}': {e}")

    def _setup_routes(self):
        @self.app.route("/health", methods=["GET"])
        def health():
            return jsonify({"status": "ok", "device_online": self.device_connection.connected})

        @self.app.route("/status", methods=["GET"])
        def status():
            return jsonify(self.device_connection.get_status())

    def _start_http_server(self):
        def run_flask():
            try:
                self.app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)
            except Exception as e:
                logger.error(f"HTTP server error: {e}")
        th = threading.Thread(target=run_flask, name="HTTPServer", daemon=True)
        self.threads["http_server"] = th
        th.start()
        logger.info(f"Started HTTP server at {HTTP_HOST}:{HTTP_PORT}")

    def signal_handler(self, sig, frame):
        logger.info(f"Signal {sig} received, shutting down driver...")
        shutdown_flag.set()
        self.shutdown()

    def shutdown(self):
        logger.info("Shutting down DeviceShifu MQTT Driver...")
        try:
            if self.mqtt_client:
                self.mqtt_client.loop_stop()
                self.mqtt_client.disconnect()
        except Exception as e:
            logger.error(f"Error during MQTT disconnect: {e}")
        try:
            self.device_connection.disconnect()
        except Exception as e:
            logger.error(f"Error during device disconnect: {e}")
        for name, th in self.threads.items():
            if th.is_alive():
                logger.info(f"Joining thread {name} ...")
                th.join(timeout=5)
        logger.info("All threads stopped. Exiting.")

    def run(self):
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
        logger.info("Starting DeviceShifu MQTT Driver...")

        # Device connection
        self.device_online = self.device_connection.connect()

        # Start HTTP server
        self._start_http_server()

        # Connect MQTT
        self.connect_mqtt()

        # Start periodic publishers
        self._start_scheduled_publishers()

        # Main thread: wait until shutdown_flag is set
        while not shutdown_flag.is_set():
            time.sleep(1)
        self.shutdown()


if __name__ == "__main__":
    driver = DeviceShifuMQTTDriver()
    driver.run()