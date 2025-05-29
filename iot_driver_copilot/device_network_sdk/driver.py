import os
import sys
import time
import json
import yaml
import signal
import logging
import threading
from typing import Dict, Any, Optional

import paho.mqtt.client as mqtt
from flask import Flask, jsonify
from kubernetes import client as k8s_client, config as k8s_config
from kubernetes.client.rest import ApiException

# ------------------- Logging Setup -------------------
LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=LOG_LEVEL,
    format="%(asctime)s %(levelname)s %(threadName)s %(name)s: %(message)s",
    stream=sys.stdout,
)
logger = logging.getLogger("deviceshifu-mqtt")

# ------------------- ShifuClient -------------------

class ShifuClient:
    def __init__(self):
        self.edge_device_name = os.getenv("EDGEDEVICE_NAME", "deviceshifu-decoder")
        self.edge_device_namespace = os.getenv("EDGEDEVICE_NAMESPACE", "devices")
        self.config_mount_path = os.getenv("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
        self.k8s_api = None
        self._init_k8s_client()

    def _init_k8s_client(self) -> None:
        try:
            if os.path.exists("/var/run/secrets/kubernetes.io/serviceaccount/token"):
                k8s_config.load_incluster_config()
                logger.info("Loaded in-cluster Kubernetes config.")
            else:
                k8s_config.load_kube_config()
                logger.info("Loaded local kubeconfig.")
            self.k8s_api = k8s_client.CustomObjectsApi()
        except Exception as e:
            logger.error(f"Failed to initialize Kubernetes client: {e}")
            self.k8s_api = None

    def get_edge_device(self) -> Optional[Dict[str, Any]]:
        if not self.k8s_api:
            logger.error("Kubernetes client not initialized, cannot get EdgeDevice.")
            return None
        try:
            result = self.k8s_api.get_namespaced_custom_object(
                group="deviceshifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.edge_device_namespace,
                plural="edgedevices",
                name=self.edge_device_name,
            )
            return result
        except ApiException as e:
            logger.error(f"Kubernetes API exception in get_edge_device: {e}")
        except Exception as e:
            logger.error(f"Failed to get EdgeDevice: {e}")
        return None

    def get_device_address(self) -> Optional[str]:
        try:
            device = self.get_edge_device()
            if not device:
                return None
            addr = device.get("spec", {}).get("address", "")
            logger.debug(f"Device address from EdgeDevice spec: {addr}")
            return addr
        except Exception as e:
            logger.error(f"Error getting device address: {e}")
            return None

    def update_device_status(self, status: Dict[str, Any]) -> None:
        if not self.k8s_api:
            logger.error("Kubernetes client not initialized, cannot update status.")
            return
        try:
            body = {"status": status}
            self.k8s_api.patch_namespaced_custom_object_status(
                group="deviceshifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.edge_device_namespace,
                plural="edgedevices",
                name=self.edge_device_name,
                body=body
            )
            logger.debug(f"Updated device status: {status}")
        except ApiException as e:
            logger.error(f"Kubernetes API exception in update_device_status: {e}")
        except Exception as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Optional[Any]:
        path = os.path.join(self.config_mount_path, filename)
        try:
            with open(path, "r") as f:
                data = yaml.safe_load(f)
                logger.debug(f"Loaded config file {filename}: {data}")
                return data
        except Exception as e:
            logger.error(f"Failed to read config file {filename} at {path}: {e}")
            return None

    def get_instruction_config(self) -> Dict[str, Any]:
        instructions = self.read_mounted_config_file("instructions")
        if not instructions or not isinstance(instructions, dict):
            logger.warning("No instruction config loaded or invalid format.")
            return {}
        return instructions

# ------------------- Device Protocol Abstraction -------------------

class DummyDeviceConnector:
    """
    Abstraction for device protocol connection (simulated).
    Replace with actual SDK/API as needed.
    """
    def __init__(self, address: str):
        self.address = address
        self.connected = False
        self.status = {}
        self.info = {}
        self.lock = threading.Lock()

    def connect(self) -> bool:
        try:
            # Simulate device connection
            logger.info(f"Connecting to device at {self.address} ...")
            time.sleep(0.5)
            self.connected = True
            logger.info("Device connection established.")
            return True
        except Exception as e:
            logger.error(f"Device connection failed: {e}")
            return False

    def disconnect(self):
        with self.lock:
            self.connected = False
            logger.info("Device disconnected.")

    def get_status(self) -> Dict[str, Any]:
        with self.lock:
            # Simulate retrieving device status
            status = {
                "operational_state": "ok" if self.connected else "disconnected",
                "version": "V1.2.3",
                "error_codes": [],
                "channel_config": {"channels": 32, "active": 28},
                "decode_channel_status": {"channel_1": "active", "channel_2": "idle"},
                "loop_decode_status": {"loop_1": "running"},
                "timestamp": int(time.time())
            }
            self.status = status
            return status

    def get_info(self) -> Dict[str, Any]:
        with self.lock:
            # Simulate retrieving device info
            info = {
                "device_name": "Device Network SDK",
                "model": "DS_64XXHD_S",
                "abilities": {
                    "channel": 32,
                    "stream": 16,
                    "wall_params": {"max_width": 8, "max_height": 4}
                },
                "config": {"scene_mode": "video_wall"},
                "timestamp": int(time.time())
            }
            self.info = info
            return info

    def send_command(self, command: str, params: Dict[str, Any]) -> Dict[str, Any]:
        with self.lock:
            # Simulate command execution
            logger.info(f"Executing device command: {command}, params: {params}")
            result = {"status": "success", "command": command, "params": params, "timestamp": int(time.time())}
            return result

    def activate(self, credentials: str) -> Dict[str, Any]:
        with self.lock:
            logger.info(f"Activating device with credentials: {credentials}")
            return {"status": "activated", "timestamp": int(time.time())}

    def login(self, username: str, password: str) -> Dict[str, Any]:
        with self.lock:
            logger.info(f"Logging in device user: {username}")
            return {"status": "logged_in", "timestamp": int(time.time())}

    def reboot(self) -> Dict[str, Any]:
        with self.lock:
            logger.info("Rebooting device ...")
            return {"status": "rebooted", "timestamp": int(time.time())}

# ------------------- Main DeviceShifu MQTT Driver -------------------

class DeviceShifuMQTTDriver:
    def __init__(self):
        # Config
        self.edge_device_name = os.getenv("EDGEDEVICE_NAME", "deviceshifu-decoder")
        self.edge_device_namespace = os.getenv("EDGEDEVICE_NAMESPACE", "devices")
        self.config_mount_path = os.getenv("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
        self.mqtt_broker = os.getenv("MQTT_BROKER", "localhost")
        self.mqtt_port = int(os.getenv("MQTT_BROKER_PORT", "1883"))
        self.mqtt_username = os.getenv("MQTT_BROKER_USERNAME", "")
        self.mqtt_password = os.getenv("MQTT_BROKER_PASSWORD", "")
        self.mqtt_topic_prefix = os.getenv("MQTT_TOPIC_PREFIX", "shifu")
        self.http_host = os.getenv("HTTP_HOST", "0.0.0.0")
        self.http_port = int(os.getenv("HTTP_PORT", "8080"))
        self.log_level = os.getenv("LOG_LEVEL", "INFO").upper()
        self.shutdown_flag = threading.Event()
        self.threads = []
        self.latest_data: Dict[str, Any] = {}
        self.status_lock = threading.Lock()
        self.connected = False

        # ShifuClient
        self.shifu_client = ShifuClient()

        # Device connection
        device_address = self.shifu_client.get_device_address() or "127.0.0.1"
        self.device_connector = DummyDeviceConnector(device_address)

        # MQTT
        self.mqtt_client = None
        self.mqtt_connected = threading.Event()
        self.mqtt_client_id = f"{self.edge_device_name}-{int(time.time())}"

        # Flask HTTP
        self.flask_app = Flask(__name__)
        self.setup_routes()

        # Instruction config
        self.instruction_config = self.shifu_client.get_instruction_config()
        self.publish_intervals = self._parse_publish_intervals(self.instruction_config)

    def _parse_publish_intervals(self, instruction_config: Dict[str, Any]) -> Dict[str, int]:
        intervals = {}
        for instruction, config in instruction_config.items():
            if isinstance(config, dict):
                if config.get("mode", "").lower() == "publisher" and "publishIntervalMS" in config:
                    try:
                        intervals[instruction] = int(config["publishIntervalMS"])
                    except Exception as e:
                        logger.warning(f"Failed to parse publishIntervalMS for {instruction}: {e}")
        logger.debug(f"Parsed publish intervals: {intervals}")
        return intervals

    def _start_scheduled_publishers(self):
        for instruction, interval_ms in self.publish_intervals.items():
            t = threading.Thread(
                target=self._publish_topic_periodically,
                args=(instruction, interval_ms),
                name=f"publisher-{instruction}",
                daemon=True
            )
            t.start()
            self.threads.append(t)
            logger.info(f"Started periodic publisher for {instruction} every {interval_ms} ms.")

    def _publish_topic_periodically(self, instruction: str, interval_ms: int):
        topic_path = self._map_instruction_to_topic(instruction)
        while not self.shutdown_flag.is_set():
            data = self._get_data_for_instruction(instruction)
            self.latest_data[instruction] = data
            self._mqtt_publish(topic_path, data, qos=1)
            time.sleep(interval_ms / 1000.0)

    def _get_data_for_instruction(self, instruction: str) -> Dict[str, Any]:
        # Map the instruction to device API
        if instruction in ["device/telemetry/status", "status"]:
            try:
                return self.device_connector.get_status()
            except Exception as e:
                logger.error(f"Error getting device status: {e}")
                return {"error": str(e)}
        elif instruction in ["device/telemetry/info", "info"]:
            try:
                return self.device_connector.get_info()
            except Exception as e:
                logger.error(f"Error getting device info: {e}")
                return {"error": str(e)}
        else:
            # Default: return empty
            return {"timestamp": int(time.time())}

    def _map_instruction_to_topic(self, instruction: str) -> str:
        return f"{self.mqtt_topic_prefix}/{self.edge_device_name}/{instruction}"

    def connect_mqtt(self):
        self.mqtt_client = mqtt.Client(client_id=self.mqtt_client_id, clean_session=True)
        if self.mqtt_username:
            self.mqtt_client.username_pw_set(self.mqtt_username, self.mqtt_password)
        self.mqtt_client.on_connect = self.on_connect
        self.mqtt_client.on_disconnect = self.on_disconnect
        self.mqtt_client.on_message = self.on_message
        self.mqtt_client.on_subscribe = self.on_subscribe
        self.mqtt_client.on_publish = self.on_publish
        self.mqtt_client.loop_start()
        while not self.shutdown_flag.is_set():
            try:
                logger.info(f"Connecting to MQTT broker at {self.mqtt_broker}:{self.mqtt_port}")
                self.mqtt_client.connect(self.mqtt_broker, self.mqtt_port, keepalive=30)
                break
            except Exception as e:
                logger.error(f"MQTT connection failed: {e}. Retrying in 5 seconds ...")
                time.sleep(5)
        # Wait for connection callback
        self.mqtt_connected.wait(timeout=15)
        if not self.mqtt_connected.is_set():
            logger.error("MQTT connection could not be established.")
            raise ConnectionError("MQTT connection timed out.")

    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.mqtt_connected.set()
            self.connected = True
            logger.info("MQTT Connected.")
            # Subscribe to control and telemetry topics
            control_topic = f"{self.mqtt_topic_prefix}/{self.edge_device_name}/control/#"
            self.mqtt_client.subscribe(control_topic, qos=1)
            logger.info(f"Subscribed to control topic: {control_topic}")
            # Subscribe to any subscriber instructions in instruction config
            for instruction, config in self.instruction_config.items():
                if isinstance(config, dict) and config.get("mode", "").lower() == "subscriber":
                    topic = self._map_instruction_to_topic(instruction)
                    qos = int(config.get("qos", 1))
                    self.mqtt_client.subscribe(topic, qos=qos)
                    logger.info(f"Subscribed to data topic: {topic} (QoS {qos})")
            # Subscribe to API-defined topics
            self.mqtt_client.subscribe(f"{self.mqtt_topic_prefix}/{self.edge_device_name}/device/commands/#", qos=2)
        else:
            logger.error(f"MQTT Connection failed with code {rc}.")

    def on_disconnect(self, client, userdata, rc):
        self.connected = False
        self.mqtt_connected.clear()
        logger.warning(f"MQTT Disconnected (rc={rc}).")
        # Attempt reconnect unless shutting down
        if not self.shutdown_flag.is_set():
            while not self.shutdown_flag.is_set():
                try:
                    self.mqtt_client.reconnect()
                    break
                except Exception as e:
                    logger.error(f"MQTT reconnection failed: {e}. Retrying in 5 seconds ...")
                    time.sleep(5)

    def on_subscribe(self, client, userdata, mid, granted_qos):
        logger.debug(f"Subscribed: mid={mid}, QoS={granted_qos}")

    def on_publish(self, client, userdata, mid):
        logger.debug(f"Published: mid={mid}")

    def on_message(self, client, userdata, msg):
        try:
            payload = msg.payload.decode("utf-8")
            logger.info(f"Received message on topic {msg.topic}: {payload}")
            self.handle_mqtt_message(msg.topic, payload)
        except Exception as e:
            logger.error(f"Error processing received MQTT message: {e}")

    def handle_mqtt_message(self, topic: str, payload: str):
        try:
            data = json.loads(payload)
        except Exception as e:
            logger.error(f"Invalid JSON in MQTT message: {e}")
            return

        # Control commands (e.g., /control/activate, /control/login, /control/reboot)
        if topic.startswith(f"{self.mqtt_topic_prefix}/{self.edge_device_name}/control"):
            command = topic.split("/")[-1]
            response = {}
            if command == "activate":
                credentials = data.get("credentials", "")
                response = self.device_connector.activate(credentials)
            elif command == "login":
                user = data.get("username", "")
                pwd = data.get("password", "")
                response = self.device_connector.login(user, pwd)
            elif command == "reboot":
                response = self.device_connector.reboot()
            elif command == "control":
                cmd = data.get("command", "")
                params = data.get("params", {})
                response = self.device_connector.send_command(cmd, params)
            else:
                logger.warning(f"Unknown control command: {command}")
                response = {"error": f"Unknown command {command}"}
            # Optionally publish response
            resp_topic = f"{self.mqtt_topic_prefix}/{self.edge_device_name}/control/{command}/response"
            self._mqtt_publish(resp_topic, response, qos=1)
        # API PUBLISH/Subscribe topics
        elif topic.endswith("device/commands/activate"):
            credentials = data.get("credentials", "")
            response = self.device_connector.activate(credentials)
            self._mqtt_publish(topic + "/response", response, qos=1)
        elif topic.endswith("device/commands/login"):
            user = data.get("username", "")
            pwd = data.get("password", "")
            response = self.device_connector.login(user, pwd)
            self._mqtt_publish(topic + "/response", response, qos=1)
        elif topic.endswith("device/commands/reboot"):
            response = self.device_connector.reboot()
            self._mqtt_publish(topic + "/response", response, qos=1)
        elif topic.endswith("device/commands/control"):
            cmd = data.get("command", "")
            params = data.get("params", {})
            response = self.device_connector.send_command(cmd, params)
            self._mqtt_publish(topic + "/response", response, qos=1)
        else:
            logger.info(f"Unhandled topic: {topic}")

    def _mqtt_publish(self, topic: str, payload: Any, qos: int = 1):
        try:
            message = json.dumps(payload, default=str)
            self.mqtt_client.publish(topic, message, qos=qos, retain=False)
            logger.debug(f"Published to {topic}: {message}")
        except Exception as e:
            logger.error(f"Failed to publish to {topic}: {e}")

    def setup_routes(self):
        @self.flask_app.route('/health', methods=['GET'])
        def health():
            health_info = {
                "status": "ok",
                "device_connected": self.device_connector.connected,
                "mqtt_connected": self.connected,
                "timestamp": int(time.time())
            }
            return jsonify(health_info), 200

        @self.flask_app.route('/status', methods=['GET'])
        def status():
            with self.status_lock:
                status = self.device_connector.get_status()
                return jsonify(status), 200

    def run_http_server(self):
        try:
            logger.info(f"Starting HTTP server at {self.http_host}:{self.http_port}")
            self.flask_app.run(host=self.http_host, port=self.http_port, threaded=True)
        except Exception as e:
            logger.error(f"HTTP server error: {e}")

    def device_connection_monitor(self):
        while not self.shutdown_flag.is_set():
            if not self.device_connector.connected:
                try:
                    self.device_connector.connect()
                except Exception as e:
                    logger.error(f"Device connection monitor error: {e}")
            time.sleep(5)

    def status_reporter(self):
        # Periodically publish status to MQTT
        topic = f"{self.mqtt_topic_prefix}/{self.edge_device_name}/status"
        while not self.shutdown_flag.is_set():
            status = self.device_connector.get_status()
            self._mqtt_publish(topic, status, qos=1)
            time.sleep(10)

    def signal_handler(self, signum, frame):
        logger.info(f"Received signal {signum}, shutting down ...")
        self.shutdown()

    def shutdown(self):
        self.shutdown_flag.set()
        logger.info("Shutting down threads ...")
        for t in self.threads:
            if t.is_alive():
                t.join(timeout=5)
        try:
            if self.mqtt_client:
                self.mqtt_client.loop_stop(force=True)
                self.mqtt_client.disconnect()
        except Exception as e:
            logger.error(f"Error disconnecting MQTT client: {e}")
        try:
            self.device_connector.disconnect()
        except Exception as e:
            logger.error(f"Error disconnecting device: {e}")
        logger.info("Shutdown complete.")
        sys.exit(0)

    def run(self):
        # Signal handling
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
        # Device connection monitor
        t_monitor = threading.Thread(target=self.device_connection_monitor, name="device-conn-monitor", daemon=True)
        t_monitor.start()
        self.threads.append(t_monitor)
        # Connect device
        if not self.device_connector.connect():
            logger.warning("Device not connected at startup.")
        # MQTT
        try:
            self.connect_mqtt()
        except Exception as e:
            logger.error(f"MQTT connection failed at startup: {e}")
        # HTTP
        t_http = threading.Thread(target=self.run_http_server, name="http-server", daemon=True)
        t_http.start()
        self.threads.append(t_http)
        # Periodic publishers
        self._start_scheduled_publishers()
        # Status reporter
        t_status = threading.Thread(target=self.status_reporter, name="status-reporter", daemon=True)
        t_status.start()
        self.threads.append(t_status)
        # Wait for shutdown
        try:
            while not self.shutdown_flag.is_set():
                time.sleep(1)
        except (KeyboardInterrupt, SystemExit):
            self.shutdown()

if __name__ == "__main__":
    driver = DeviceShifuMQTTDriver()
    driver.run()