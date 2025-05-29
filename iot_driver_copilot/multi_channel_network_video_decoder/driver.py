import os
import sys
import json
import time
import logging
import threading
import signal
from typing import Dict, Any, Optional
import yaml
from flask import Flask, jsonify
import paho.mqtt.client as mqtt

# Kubernetes imports
from kubernetes import client as k8s_client, config as k8s_config
from kubernetes.client.rest import ApiException

# Logging setup
LOG_LEVEL = os.getenv('LOG_LEVEL', 'INFO').upper()
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL, logging.INFO),
    format='%(asctime)s %(levelname)s %(threadName)s %(message)s'
)
logger = logging.getLogger(__name__)

def get_env(key: str, default: Optional[str] = None) -> str:
    v = os.getenv(key, default)
    if v is None:
        logger.error(f"Required environment variable '{key}' is not set and no default provided.")
        sys.exit(1)
    return v

# Environment variables
EDGEDEVICE_NAME = get_env('EDGEDEVICE_NAME', 'deviceshifu-networkvideodecoder')
EDGEDEVICE_NAMESPACE = get_env('EDGEDEVICE_NAMESPACE', 'devices')
CONFIG_MOUNT_PATH = get_env('CONFIG_MOUNT_PATH', '/etc/edgedevice/config')
MQTT_BROKER = get_env('MQTT_BROKER')
MQTT_BROKER_PORT = int(get_env('MQTT_BROKER_PORT', '1883'))
MQTT_BROKER_USERNAME = os.getenv('MQTT_BROKER_USERNAME')
MQTT_BROKER_PASSWORD = os.getenv('MQTT_BROKER_PASSWORD')
MQTT_TOPIC_PREFIX = get_env('MQTT_TOPIC_PREFIX', 'shifu')
HTTP_HOST = get_env('HTTP_HOST', '0.0.0.0')
HTTP_PORT = int(get_env('HTTP_PORT', '8080'))

INSTRUCTION_CONFIG_PATH = os.path.join(CONFIG_MOUNT_PATH, 'instructions')
DRIVER_PROPERTIES_PATH = os.path.join(CONFIG_MOUNT_PATH, 'driverProperties')

shutdown_flag = threading.Event()

class ShifuClient:
    def __init__(self):
        self.core_v1 = None
        self.custom_api = None
        self.group = "shifu.edgedevice.microsoft.com"
        self.version = "v1alpha1"
        self.plural = "edgedevices"
        self._init_k8s_client()

    def _init_k8s_client(self):
        try:
            k8s_config.load_incluster_config()
            logger.info("Loaded in-cluster Kubernetes configuration.")
        except Exception:
            try:
                k8s_config.load_kube_config()
                logger.info("Loaded local kubeconfig.")
            except Exception as e:
                logger.error(f"Failed to initialize Kubernetes client: {e}")
                sys.exit(1)
        self.core_v1 = k8s_client.CoreV1Api()
        self.custom_api = k8s_client.CustomObjectsApi()

    def get_edge_device(self) -> Dict[str, Any]:
        try:
            device = self.custom_api.get_namespaced_custom_object(
                group=self.group,
                version=self.version,
                namespace=EDGEDEVICE_NAMESPACE,
                plural=self.plural,
                name=EDGEDEVICE_NAME
            )
            logger.debug(f"Fetched EdgeDevice resource: {device}")
            return device
        except ApiException as e:
            logger.error(f"Failed to get EdgeDevice resource: {e}")
            return {}

    def get_device_address(self) -> Optional[str]:
        device = self.get_edge_device()
        try:
            return device['spec']['address']
        except KeyError:
            logger.warning("Device address not found in EdgeDevice spec.")
            return None

    def update_device_status(self, status: Dict[str, Any]) -> None:
        body = {'status': status}
        try:
            self.custom_api.patch_namespaced_custom_object_status(
                group=self.group,
                version=self.version,
                namespace=EDGEDEVICE_NAMESPACE,
                plural=self.plural,
                name=EDGEDEVICE_NAME,
                body=body
            )
            logger.debug(f"Updated EdgeDevice status: {status}")
        except ApiException as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Dict[str, Any]:
        path = os.path.join(CONFIG_MOUNT_PATH, filename)
        try:
            with open(path, 'r') as f:
                data = yaml.safe_load(f)
                logger.debug(f"Read config file {filename}: {data}")
                return data if data else {}
        except Exception as e:
            logger.error(f"Could not read config file {filename}: {e}")
            return {}

    def get_instruction_config(self) -> Dict[str, Any]:
        return self.read_mounted_config_file('instructions')

    def get_driver_properties(self) -> Dict[str, Any]:
        return self.read_mounted_config_file('driverProperties')


class DeviceShifuMQTTDriver:
    def __init__(self):
        self.shifu_client = ShifuClient()
        self.device_address = self.shifu_client.get_device_address()
        self.instruction_config = self.shifu_client.get_instruction_config()
        self.driver_properties = self.shifu_client.get_driver_properties()
        self.latest_data: Dict[str, Any] = {}
        self.publish_intervals: Dict[str, int] = {}
        self.publish_threads: Dict[str, threading.Thread] = {}
        self.mqtt_connected = threading.Event()
        self.mqtt_client = None
        self.http_app = Flask(__name__)
        self.http_thread = None
        self.status_lock = threading.Lock()
        self.last_status: Dict[str, Any] = {}
        self._parse_publish_intervals()
        self.setup_routes()

    def _parse_publish_intervals(self) -> None:
        if not self.instruction_config:
            logger.warning("No instruction configuration found; no periodic publishing will be set up.")
            return
        instructions = self.instruction_config.get('instructions', {})
        for instr_name, instr_config in instructions.items():
            mode = instr_config.get('protocolProperties', {}).get('mode', '').lower()
            publish_interval = instr_config.get('protocolProperties', {}).get('publishIntervalMS')
            if mode == 'publisher' and publish_interval is not None:
                try:
                    interval_ms = int(publish_interval)
                    if interval_ms > 0:
                        self.publish_intervals[instr_name] = interval_ms
                        logger.info(f"Set publish interval for {instr_name}: {interval_ms} ms")
                except Exception as e:
                    logger.error(f"Invalid publishIntervalMS for {instr_name}: {e}")

    def _start_scheduled_publishers(self) -> None:
        for topic, interval_ms in self.publish_intervals.items():
            t = threading.Thread(
                target=self._publish_topic_periodically,
                args=(topic, interval_ms),
                name=f"Publisher-{topic}",
                daemon=True
            )
            self.publish_threads[topic] = t
            t.start()
            logger.info(f"Started periodic publisher for topic '{topic}' every {interval_ms} ms")

    def _publish_topic_periodically(self, topic: str, interval_ms: int) -> None:
        while not shutdown_flag.is_set():
            try:
                data = self._retrieve_device_data(topic)
                if data is not None:
                    self.latest_data[topic] = data
                    self._publish_mqtt_data(topic, data)
            except Exception as e:
                logger.error(f"Error in periodic publishing for {topic}: {e}")
            shutdown_flag.wait(interval_ms / 1000.0)

    def _retrieve_device_data(self, topic: str) -> Optional[Dict[str, Any]]:
        # Device-specific data retrieval logic.
        # Placeholder implementation:
        if topic == 'status':
            return {
                "device_status": "online",
                "channels": [
                    {"id": 1, "state": "active", "error_code": 0},
                    {"id": 2, "state": "inactive", "error_code": 0}
                ],
                "upgrade_progress": 0,
                "alarm": {"active": False, "type": None},
                "sdk_version": "v1.0.0"
            }
        elif topic == 'config':
            return {
                "decoding_channels": 4,
                "display_config": {"windows": 2, "layout": "2x2"},
                "current_scene": "default"
            }
        else:
            logger.debug(f"No retrieval logic for topic '{topic}', returning dummy data.")
            return {"dummy": "data"}

    def _publish_mqtt_data(self, topic: str, data: Any, qos: int = 1) -> None:
        if not self.mqtt_connected.is_set():
            logger.warning(f"MQTT not connected; cannot publish to topic {topic}")
            return
        topic_path = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/{topic}"
        payload = json.dumps(data, default=str)
        try:
            self.mqtt_client.publish(topic_path, payload, qos=qos)
            logger.info(f"Published to {topic_path} (QoS={qos}): {payload}")
        except Exception as e:
            logger.error(f"Failed to publish to {topic_path}: {e}")

    def _publish_status_report(self) -> None:
        # Special topic for status updates
        with self.status_lock:
            status = {
                "mqtt_connected": self.mqtt_connected.is_set(),
                "device_connected": True if self.device_address else False,
                "last_status": self.latest_data.get('status', {})
            }
            self.last_status = status
        topic_path = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/status"
        try:
            self.mqtt_client.publish(topic_path, json.dumps(status, default=str), qos=1)
            logger.debug(f"Published status report to {topic_path}: {status}")
        except Exception as e:
            logger.error(f"Failed to publish status report: {e}")

    def connect_mqtt(self) -> None:
        client_id = f"{EDGEDEVICE_NAME}-{int(time.time())}"
        self.mqtt_client = mqtt.Client(client_id=client_id, clean_session=True)
        if MQTT_BROKER_USERNAME and MQTT_BROKER_PASSWORD:
            self.mqtt_client.username_pw_set(MQTT_BROKER_USERNAME, MQTT_BROKER_PASSWORD)
        self.mqtt_client.on_connect = self._on_mqtt_connect
        self.mqtt_client.on_disconnect = self._on_mqtt_disconnect
        self.mqtt_client.on_message = self._on_mqtt_message

        def loop_forever():
            while not shutdown_flag.is_set():
                try:
                    self.mqtt_client.connect(MQTT_BROKER, MQTT_BROKER_PORT, keepalive=60)
                    self.mqtt_client.loop_forever()
                except Exception as e:
                    logger.error(f"MQTT connection error: {e}")
                    shutdown_flag.wait(5)
        t = threading.Thread(target=loop_forever, name='MQTTLoop', daemon=True)
        t.start()

    def _on_mqtt_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.mqtt_connected.set()
            logger.info("Connected to MQTT broker.")
            # Subscribe to control/command topics
            control_topics = [
                f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/control/reboot",
                f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/control/config"
            ]
            for topic in control_topics:
                try:
                    self.mqtt_client.subscribe(topic, qos=1)
                    logger.info(f"Subscribed to control topic: {topic}")
                except Exception as e:
                    logger.error(f"Failed to subscribe to topic {topic}: {e}")

            # Telemetry subscription (if needed)
            telemetry_topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/device/telemetry/status"
            try:
                self.mqtt_client.subscribe(telemetry_topic, qos=1)
                logger.info(f"Subscribed to telemetry topic: {telemetry_topic}")
            except Exception as e:
                logger.error(f"Failed to subscribe to telemetry topic: {e}")

            # On connect, publish initial status
            self._publish_status_report()
        else:
            logger.error(f"Failed to connect to MQTT broker. Return code: {rc}")

    def _on_mqtt_disconnect(self, client, userdata, rc):
        self.mqtt_connected.clear()
        logger.warning(f"Disconnected from MQTT broker (rc={rc})")
        # Optionally, publish offline status
        try:
            self._publish_status_report()
        except Exception:
            pass

    def _on_mqtt_message(self, client, userdata, msg):
        logger.info(f"Received MQTT message on {msg.topic}: {msg.payload}")
        topic = msg.topic
        try:
            payload = json.loads(msg.payload.decode('utf-8'))
        except Exception as e:
            logger.error(f"Invalid JSON payload: {e}")
            return

        # Handle control commands
        if topic.endswith('/control/reboot'):
            self._handle_reboot_command(payload)
        elif topic.endswith('/control/config'):
            self._handle_config_command(payload)
        elif topic.endswith('/device/telemetry/status'):
            self.latest_data['status'] = payload
        else:
            logger.debug(f"Unhandled MQTT message topic: {topic}")

    def _handle_reboot_command(self, payload: Dict[str, Any]) -> None:
        logger.info(f"Received reboot command: {payload}")
        # Device-specific reboot logic here
        # Placeholder: acknowledge reboot
        ack_topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/ack/reboot"
        ack_payload = {"result": "rebooted", "timestamp": int(time.time())}
        try:
            self.mqtt_client.publish(ack_topic, json.dumps(ack_payload, default=str), qos=1)
        except Exception as e:
            logger.error(f"Failed to publish reboot ack: {e}")

    def _handle_config_command(self, payload: Dict[str, Any]) -> None:
        logger.info(f"Received config command: {payload}")
        # Device-specific config logic here
        # Placeholder: echo config
        ack_topic = f"{MQTT_TOPIC_PREFIX}/{EDGEDEVICE_NAME}/ack/config"
        ack_payload = {"result": "config_applied", "config": payload, "timestamp": int(time.time())}
        try:
            self.mqtt_client.publish(ack_topic, json.dumps(ack_payload, default=str), qos=1)
        except Exception as e:
            logger.error(f"Failed to publish config ack: {e}")

    def setup_routes(self) -> None:
        @self.http_app.route('/health', methods=['GET'])
        def health():
            return jsonify({"status": "healthy"}), 200

        @self.http_app.route('/status', methods=['GET'])
        def status():
            with self.status_lock:
                return jsonify(self.last_status), 200

    def run_http_server(self) -> None:
        logger.info(f"HTTP server starting at {HTTP_HOST}:{HTTP_PORT}")
        try:
            self.http_app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)
        except Exception as e:
            logger.error(f"HTTP server error: {e}")

    def signal_handler(self, signum, frame):
        logger.info(f"Signal {signum} received, shutting down gracefully.")
        shutdown_flag.set()

    def shutdown(self):
        logger.info("Shutting down DeviceShifu driver.")
        shutdown_flag.set()
        try:
            if self.mqtt_client is not None:
                self.mqtt_client.disconnect()
                logger.info("MQTT client disconnected.")
        except Exception as e:
            logger.error(f"Error during MQTT disconnect: {e}")

    def run(self):
        # Setup signal handlers
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        # Start MQTT client
        self.connect_mqtt()
        # Start periodic publishers
        self._start_scheduled_publishers()

        # Start HTTP server in background
        self.http_thread = threading.Thread(target=self.run_http_server, name='HTTPServer', daemon=True)
        self.http_thread.start()

        # Main loop: monitor shutdown_flag
        try:
            while not shutdown_flag.is_set():
                self._publish_status_report()
                time.sleep(10)
        except Exception as e:
            logger.error(f"Exception in main loop: {e}")
        finally:
            self.shutdown()


if __name__ == '__main__':
    driver = DeviceShifuMQTTDriver()
    driver.run()