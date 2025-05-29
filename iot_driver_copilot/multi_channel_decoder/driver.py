import os
import sys
import signal
import threading
import time
import json
import yaml
import logging
from typing import Dict, Any, Optional

import paho.mqtt.client as mqtt
from flask import Flask, jsonify
from kubernetes import client as k8s_client, config as k8s_config
from kubernetes.client.rest import ApiException

# ========================
# Logging Configuration
# ========================
LOG_LEVEL = os.environ.get('LOG_LEVEL', 'INFO').upper()
logging.basicConfig(
    format='[%(asctime)s][%(levelname)s] %(message)s',
    level=getattr(logging, LOG_LEVEL, logging.INFO)
)
logger = logging.getLogger(__name__)

# ========================
# Environment Variables
# ========================
EDGEDEVICE_NAME = os.environ.get('EDGEDEVICE_NAME', 'deviceshifu-video-decoder')
EDGEDEVICE_NAMESPACE = os.environ.get('EDGEDEVICE_NAMESPACE', 'devices')
CONFIG_MOUNT_PATH = os.environ.get('CONFIG_MOUNT_PATH', '/etc/edgedevice/config')
MQTT_BROKER = os.environ.get('MQTT_BROKER', 'localhost')
MQTT_BROKER_PORT = int(os.environ.get('MQTT_BROKER_PORT', 1883))
MQTT_BROKER_USERNAME = os.environ.get('MQTT_BROKER_USERNAME')
MQTT_BROKER_PASSWORD = os.environ.get('MQTT_BROKER_PASSWORD')
MQTT_TOPIC_PREFIX = os.environ.get('MQTT_TOPIC_PREFIX', 'shifu')
HTTP_HOST = os.environ.get('HTTP_HOST', '0.0.0.0')
HTTP_PORT = int(os.environ.get('HTTP_PORT', 8080))

# ========================
# ShifuClient Class
# ========================
class ShifuClient:
    def __init__(self, device_name: str = EDGEDEVICE_NAME, device_namespace: str = EDGEDEVICE_NAMESPACE,
                 config_mount_path: str = CONFIG_MOUNT_PATH) -> None:
        self.device_name = device_name
        self.device_namespace = device_namespace
        self.config_mount_path = config_mount_path
        self.k8s_api = None
        self._init_k8s_client()

    def _init_k8s_client(self) -> None:
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
        group = 'edgedevice.shifu.edgenesis.io'
        version = 'v1alpha1'
        plural = 'edgedevices'
        try:
            dev = self.k8s_api.get_namespaced_custom_object(
                group, version, self.device_namespace, plural, self.device_name)
            logger.debug("EdgeDevice resource retrieved successfully.")
            return dev
        except ApiException as e:
            logger.error(f"Failed to get EdgeDevice resource: {e}")
            return None

    def get_device_address(self) -> Optional[str]:
        dev = self.get_edge_device()
        if not dev:
            return None
        try:
            return dev['spec']['address']
        except KeyError:
            logger.warning('Device address not found in EdgeDevice spec.')
            return None

    def update_device_status(self, status: str) -> None:
        if not self.k8s_api:
            logger.warning('Kubernetes client not initialized, skipping status update.')
            return
        group = 'edgedevice.shifu.edgenesis.io'
        version = 'v1alpha1'
        plural = 'edgedevices'
        body = {"status": {"devicePhase": status}}
        try:
            self.k8s_api.patch_namespaced_custom_object_status(
                group, version, self.device_namespace, plural, self.device_name, body)
            logger.info(f"Updated EdgeDevice status to '{status}'.")
        except ApiException as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Optional[str]:
        file_path = os.path.join(self.config_mount_path, filename)
        try:
            with open(file_path, 'r') as f:
                content = f.read()
            logger.debug(f"Config file '{filename}' read successfully.")
            return content
        except Exception as e:
            logger.error(f"Failed to read config file '{filename}': {e}")
            return None

    def get_instruction_config(self) -> Optional[Dict[str, Any]]:
        content = self.read_mounted_config_file('instructions')
        if content is None:
            logger.error("Instructions config not found.")
            return None
        try:
            config = yaml.safe_load(content)
            logger.info("Instruction config parsed successfully.")
            return config
        except yaml.YAMLError as e:
            logger.error(f"Failed to parse instruction config: {e}")
            return None

    def get_driver_properties(self) -> Optional[Dict[str, Any]]:
        content = self.read_mounted_config_file('driverProperties')
        if content is None:
            logger.warning("driverProperties config not found.")
            return None
        try:
            config = yaml.safe_load(content)
            logger.info("driverProperties config parsed successfully.")
            return config
        except yaml.YAMLError as e:
            logger.error(f"Failed to parse driverProperties config: {e}")
            return None

# ========================
# DeviceShifuMQTTDriver
# ========================
class DeviceShifuMQTTDriver:
    def __init__(self):
        self.shifu_client = ShifuClient()
        self.device_name = EDGEDEVICE_NAME
        self.shutdown_flag = threading.Event()
        self.latest_data: Dict[str, Any] = {}
        self.mqtt_client: Optional[mqtt.Client] = None
        self.scheduled_threads: Dict[str, threading.Thread] = {}
        self.connection_status = {
            'mqtt': False,
            'device': False
        }
        self.app = Flask(__name__)
        self._setup_routes()
        self.instruction_config = self.shifu_client.get_instruction_config() or {}
        self.publish_intervals = self._parse_publish_intervals(self.instruction_config)
        self._lock = threading.Lock()
        self.driver_properties = self.shifu_client.get_driver_properties() or {}
        self.mqtt_qos = self._get_default_qos()
        self.mqtt_topic_prefix = MQTT_TOPIC_PREFIX

    def _get_default_qos(self) -> int:
        try:
            return int(self.driver_properties.get("mqttQos", 1))
        except Exception:
            return 1

    def _parse_publish_intervals(self, config: Dict[str, Any]) -> Dict[str, int]:
        intervals = {}
        instructions = config.get('instructions', {})
        for instr, prop in instructions.items():
            protocol = prop.get('protocolProperties', {})
            if protocol.get('mode', '').lower() == 'publisher':
                interval = int(protocol.get('publishIntervalMS', 1000))
                intervals[instr] = interval
        logger.info(f"Parsed publish intervals: {intervals}")
        return intervals

    def _start_scheduled_publishers(self):
        for topic, interval in self.publish_intervals.items():
            t = threading.Thread(
                target=self._publish_topic_periodically,
                args=(topic, interval),
                daemon=True
            )
            self.scheduled_threads[topic] = t
            t.start()
            logger.info(f"Started periodic publisher for '{topic}' every {interval}ms.")

    def _publish_topic_periodically(self, topic: str, interval_ms: int):
        while not self.shutdown_flag.is_set():
            try:
                data = self._retrieve_device_data(topic)
                if data is not None:
                    self._publish_mqtt_data(topic, data)
            except Exception as e:
                logger.error(f"Error in periodic publisher for topic '{topic}': {e}")
            self.shutdown_flag.wait(interval_ms / 1000.0)

    def _retrieve_device_data(self, topic: str) -> Any:
        # Device protocol abstraction (stubbed)
        # Replace this with actual device SDK/API calls as needed.
        # Simulate with generated data for demonstration.
        data = {}
        if topic == "device/decoder/status":
            data = {
                "device_name": self.device_name,
                "timestamp": int(time.time()),
                "device_status": "online",
                "channels": [
                    {"channel_id": 1, "status": "active"},
                    {"channel_id": 2, "status": "idle"}
                ],
                "error_codes": [],
                "config": {
                    "display": "4x4",
                    "scene_mode": "normal"
                }
            }
        else:
            data = {
                "device_name": self.device_name,
                "timestamp": int(time.time()),
                "message": f"Sample data for {topic}"
            }
        with self._lock:
            self.latest_data[topic] = data
        return data

    def _publish_mqtt_data(self, topic: str, data: Any):
        if not self.mqtt_client or not self.connection_status['mqtt']:
            logger.warning("MQTT client not connected, skipping publish.")
            return
        payload = json.dumps(data, default=str)
        mqtt_topic = f"{self.mqtt_topic_prefix}/{self.device_name}/{topic.replace('/', '_')}"
        try:
            result = self.mqtt_client.publish(
                mqtt_topic, payload, qos=self.mqtt_qos, retain=False)
            if result.rc != mqtt.MQTT_ERR_SUCCESS:
                logger.error(f"Failed to publish to MQTT topic {mqtt_topic}: {mqtt.error_string(result.rc)}")
            else:
                logger.debug(f"Published data to MQTT topic {mqtt_topic}: {payload}")
        except Exception as e:
            logger.error(f"Error publishing to MQTT: {e}")

    def _handle_command(self, topic: str, payload: Any):
        # Handle device commands (stubbed)
        try:
            command = payload.get('command')
            params = payload.get('params', {})
            logger.info(f"Received command '{command}' with params: {params}")
            # Simulate command execution and update status
            response = {
                "result": f"Command '{command}' executed",
                "timestamp": int(time.time())
            }
            # Optionally, publish command echo/ack
            ack_topic = f"{self.mqtt_topic_prefix}/{self.device_name}/ack/{command}"
            self.mqtt_client.publish(ack_topic, json.dumps(response), qos=self.mqtt_qos)
            logger.debug(f"Published command ack to {ack_topic}: {json.dumps(response)}")
        except Exception as e:
            logger.error(f"Failed to handle command on topic '{topic}': {e}")

    def connect_mqtt(self):
        client_id = f"{self.device_name}-{int(time.time())}"
        self.mqtt_client = mqtt.Client(client_id=client_id, clean_session=True)
        if MQTT_BROKER_USERNAME and MQTT_BROKER_PASSWORD:
            self.mqtt_client.username_pw_set(MQTT_BROKER_USERNAME, MQTT_BROKER_PASSWORD)
        self.mqtt_client.on_connect = self._on_mqtt_connect
        self.mqtt_client.on_disconnect = self._on_mqtt_disconnect
        self.mqtt_client.on_message = self._on_mqtt_message
        while not self.shutdown_flag.is_set():
            try:
                logger.info(f"Connecting to MQTT broker {MQTT_BROKER}:{MQTT_BROKER_PORT} ...")
                self.mqtt_client.connect(MQTT_BROKER, MQTT_BROKER_PORT, keepalive=60)
                self.mqtt_client.loop_start()
                break
            except Exception as e:
                logger.error(f"MQTT connection failed: {e}. Retrying in 5s...")
                self.connection_status['mqtt'] = False
                time.sleep(5)

    def _on_mqtt_connect(self, client, userdata, flags, rc):
        if rc == 0:
            logger.info("Connected to MQTT broker successfully.")
            self.connection_status['mqtt'] = True
            self.shifu_client.update_device_status('Connected')
            # Subscribe to control/command topics
            # Subscribe to: [mqtt_topic_prefix]/[device_name]/control/[command]
            # and to subscriber topics in config
            self._subscribe_to_topics()
        else:
            logger.error(f"MQTT connection error: {mqtt.connack_string(rc)}")
            self.connection_status['mqtt'] = False

    def _subscribe_to_topics(self):
        # From driver API info, commands are published to device/decoder/cmd
        # So we should subscribe to this topic for control commands
        control_topic = f"{self.mqtt_topic_prefix}/{self.device_name}/device_decoder_cmd"
        try:
            self.mqtt_client.subscribe(control_topic, qos=self.mqtt_qos)
            logger.info(f"Subscribed to control command topic: {control_topic}")
        except Exception as e:
            logger.error(f"Failed to subscribe to {control_topic}: {e}")

        # Subscribe to any additional subscriber topics defined in config
        # e.g. in instructions with mode=subscriber
        instructions = self.instruction_config.get('instructions', {})
        for instr, prop in instructions.items():
            protocol = prop.get('protocolProperties', {})
            if protocol.get('mode', '').lower() == 'subscriber':
                topic_name = instr.replace('/', '_')
                mqtt_topic = f"{self.mqtt_topic_prefix}/{self.device_name}/{topic_name}"
                try:
                    self.mqtt_client.subscribe(mqtt_topic, qos=int(protocol.get('qos', self.mqtt_qos)))
                    logger.info(f"Subscribed to topic: {mqtt_topic}")
                except Exception as e:
                    logger.error(f"Failed to subscribe to {mqtt_topic}: {e}")

    def _on_mqtt_disconnect(self, client, userdata, rc):
        logger.warning(f"Disconnected from MQTT broker. rc={rc}")
        self.connection_status['mqtt'] = False
        self.shifu_client.update_device_status('Disconnected')
        # Automatic reconnect handled by main loop

    def _on_mqtt_message(self, client, userdata, msg):
        try:
            topic = msg.topic
            payload = msg.payload.decode('utf-8')
            logger.info(f"MQTT message received on topic {topic}: {payload}")
            data = json.loads(payload)
            if topic.endswith("device_decoder_cmd"):
                self._handle_command(topic, data)
            else:
                # Store/update latest data for the topic
                with self._lock:
                    self.latest_data[topic] = data
        except Exception as e:
            logger.error(f"Failed to process MQTT message: {e}")

    def _setup_routes(self):
        @self.app.route('/health', methods=['GET'])
        def health():
            status = {
                "status": "ok",
                "mqtt_connected": self.connection_status['mqtt'],
                "device_connected": self.connection_status['device']
            }
            return jsonify(status), 200

        @self.app.route('/status', methods=['GET'])
        def status():
            with self._lock:
                data = {
                    "latest_data": self.latest_data.copy(),
                    "mqtt_connected": self.connection_status['mqtt'],
                    "device_connected": self.connection_status['device']
                }
            return jsonify(data), 200

    def _start_http_server(self):
        def flask_thread():
            try:
                logger.info(f"Starting HTTP server on {HTTP_HOST}:{HTTP_PORT}")
                self.app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True, use_reloader=False)
            except Exception as e:
                logger.error(f"HTTP server error: {e}")
        t = threading.Thread(target=flask_thread, daemon=True)
        t.start()
        return t

    def signal_handler(self, signum, frame):
        logger.info(f"Received signal {signum}, initiating shutdown...")
        self.shutdown()

    def shutdown(self):
        self.shutdown_flag.set()
        try:
            if self.mqtt_client:
                self.mqtt_client.loop_stop()
                self.mqtt_client.disconnect()
                logger.info("MQTT client disconnected.")
        except Exception as e:
            logger.error(f"Error during MQTT shutdown: {e}")
        self.shifu_client.update_device_status('Disconnected')
        logger.info("DeviceShifuMQTTDriver shutdown complete.")

    def run(self):
        # Register signal handlers for graceful shutdown
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        http_thread = self._start_http_server()
        self.connect_mqtt()
        self.connection_status['device'] = True
        self.shifu_client.update_device_status('Connected')
        self._start_scheduled_publishers()

        try:
            while not self.shutdown_flag.is_set():
                time.sleep(1)
        except Exception as e:
            logger.error(f"Driver main loop error: {e}")
        finally:
            self.shutdown()
            logger.info("Exiting DeviceShifuMQTTDriver.")

# ========================
# Entrypoint
# ========================
if __name__ == '__main__':
    driver = DeviceShifuMQTTDriver()
    driver.run()