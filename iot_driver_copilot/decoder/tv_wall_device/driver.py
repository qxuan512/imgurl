import os
import sys
import time
import json
import yaml
import signal
import threading
import logging
from typing import Dict, Any, Optional

import paho.mqtt.client as mqtt
from flask import Flask, jsonify
from kubernetes import client as k8s_client, config as k8s_config
from kubernetes.client.rest import ApiException

# Logging setup
LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=LOG_LEVEL,
    format="%(asctime)s [%(levelname)s] %(threadName)s %(name)s: %(message)s"
)
logger = logging.getLogger("deviceshifu-mqtt-driver")

# Default env vars
EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME", "deviceshifu-multichannel-video-decoder")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE", "devices")
CONFIG_MOUNT_PATH = os.environ.get("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
MQTT_BROKER = os.environ.get("MQTT_BROKER", "localhost")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
MQTT_BROKER_USERNAME = os.environ.get("MQTT_BROKER_USERNAME", "")
MQTT_BROKER_PASSWORD = os.environ.get("MQTT_BROKER_PASSWORD", "")
MQTT_TOPIC_PREFIX = os.environ.get("MQTT_TOPIC_PREFIX", "shifu")
HTTP_HOST = os.environ.get("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8080"))

# ConfigMap filenames
INSTRUCTIONS_FILE = os.path.join(CONFIG_MOUNT_PATH, "instructions")
DRIVER_PROPERTIES_FILE = os.path.join(CONFIG_MOUNT_PATH, "driverProperties")

shutdown_flag = threading.Event()

class ShifuClient:
    def __init__(self, name: str, namespace: str):
        self.name = name
        self.namespace = namespace
        self.k8s_api = self._init_k8s_client()

    def _init_k8s_client(self):
        try:
            k8s_config.load_incluster_config()
            logger.info("Loaded in-cluster Kubernetes config.")
        except Exception:
            try:
                k8s_config.load_kube_config()
                logger.info("Loaded local kubeconfig.")
            except Exception as e:
                logger.error(f"Failed to load Kubernetes config: {e}")
                raise
        return k8s_client.CustomObjectsApi()

    def get_edge_device(self) -> Optional[Dict[str, Any]]:
        group = "shifu.edgenesis.io"
        version = "v1alpha1"
        plural = "edgedevices"
        try:
            ed = self.k8s_api.get_namespaced_custom_object(
                group=group,
                version=version,
                namespace=self.namespace,
                plural=plural,
                name=self.name
            )
            logger.debug(f"EdgeDevice fetched: {ed}")
            return ed
        except ApiException as e:
            logger.error(f"Failed to get EdgeDevice: {e}")
            return None
        except Exception as exc:
            logger.error(f"Unexpected exception getting EdgeDevice: {exc}")
            return None

    def get_device_address(self) -> Optional[str]:
        ed = self.get_edge_device()
        if ed:
            try:
                addr = ed.get("spec", {}).get("address", None)
                logger.info(f"Device address: {addr}")
                return addr
            except Exception as e:
                logger.warning(f"Could not get device address: {e}")
        return None

    def update_device_status(self, status: Dict[str, Any]) -> bool:
        group = "shifu.edgenesis.io"
        version = "v1alpha1"
        plural = "edgedevices"
        body = {"status": status}
        try:
            self.k8s_api.patch_namespaced_custom_object_status(
                group=group,
                version=version,
                namespace=self.namespace,
                plural=plural,
                name=self.name,
                body=body
            )
            logger.info(f"EdgeDevice status updated: {status}")
            return True
        except ApiException as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")
            return False

    def read_mounted_config_file(self, filename: str) -> Optional[Dict[str, Any]]:
        try:
            with open(filename, "r") as f:
                content = yaml.safe_load(f)
                logger.debug(f"Config file '{filename}' loaded: {content}")
                return content
        except Exception as e:
            logger.error(f"Could not read config file {filename}: {e}")
            return None

    def get_instruction_config(self) -> Dict[str, Any]:
        config = self.read_mounted_config_file(INSTRUCTIONS_FILE)
        if not config:
            logger.warning("Instructions config is empty or missing.")
            return {}
        return config

class DeviceShifuMQTTDriver:
    def __init__(self):
        self.device_name = EDGEDEVICE_NAME
        self.device_namespace = EDGEDEVICE_NAMESPACE
        self.shifu_client = ShifuClient(self.device_name, self.device_namespace)
        self.device_address = self.shifu_client.get_device_address()
        self.latest_data: Dict[str, Any] = {}
        self.publish_intervals: Dict[str, int] = {}
        self.publish_threads: Dict[str, threading.Thread] = {}
        self.mqtt_client: Optional[mqtt.Client] = None
        self.connected = False
        self.mqtt_client_id = f"{self.device_name}-{int(time.time())}"
        self.instruction_config = self.shifu_client.get_instruction_config()
        self.driver_properties = self.shifu_client.read_mounted_config_file(DRIVER_PROPERTIES_FILE)
        self._parse_publish_intervals()
        self._flask_app = Flask(__name__)
        self._http_thread = threading.Thread(target=self._run_http_server, name="HTTPServer", daemon=True)
        self._mqtt_thread = threading.Thread(target=self._run_mqtt_loop, name="MQTTLoop", daemon=True)

    def _parse_publish_intervals(self):
        if not self.instruction_config:
            logger.warning("No instruction config to parse publish intervals.")
            return
        for instruction, cfg in self.instruction_config.items():
            if isinstance(cfg, dict) and cfg.get("mode", "publisher") == "publisher":
                interval = int(cfg.get("publishIntervalMS", 1000))
                self.publish_intervals[instruction] = interval
                logger.debug(f"Instruction '{instruction}' set to publish every {interval} ms.")

    def _start_scheduled_publishers(self):
        for topic, interval in self.publish_intervals.items():
            if topic not in self.publish_threads:
                t = threading.Thread(
                    target=self._publish_topic_periodically,
                    args=(topic, interval),
                    name=f"Publisher-{topic}",
                    daemon=True
                )
                self.publish_threads[topic] = t
                t.start()
                logger.info(f"Started scheduled publisher for topic '{topic}' with interval {interval} ms.")

    def _publish_topic_periodically(self, topic: str, interval_ms: int):
        while not shutdown_flag.is_set():
            try:
                data = self._get_device_data(topic)
                self.latest_data[topic] = data
                self._publish_mqtt_data(topic, data)
                logger.debug(f"Periodic data published for '{topic}': {data}")
            except Exception as e:
                logger.error(f"Error in scheduled publishing for '{topic}': {e}")
            time.sleep(interval_ms / 1000.0)

    def _get_device_data(self, topic: str) -> Dict[str, Any]:
        # Simulate device data acquisition; replace with SDK/protocol logic
        t = int(time.time())
        if topic == "status":
            # Example: overall device status
            return {
                "timestamp": t,
                "device": self.device_name,
                "status": "OK",
                "channels": [{"id": i, "state": "active" if i % 2 == 0 else "idle"} for i in range(4)],
                "alarm": {"active": False, "code": 0},
                "config_health": "good"
            }
        elif topic == "decoder_channel_status":
            return {
                "timestamp": t,
                "channels": [{"id": i, "decode_state": "decoding"} for i in range(4)]
            }
        elif topic == "display_config":
            return {
                "timestamp": t,
                "layout": "4x4",
                "active_scene": "default"
            }
        else:
            # Generic
            return {
                "timestamp": t,
                "topic": topic,
                "data": "Sample data"
            }

    def _publish_mqtt_data(self, topic: str, data: Any, qos: int = 1):
        if not self.mqtt_client or not self.connected:
            logger.warning(f"MQTT client not connected; cannot publish topic '{topic}'")
            return
        mqtt_topic = f"{MQTT_TOPIC_PREFIX}/{self.device_name}/{topic}"
        try:
            payload = json.dumps(data, default=str)
            self.mqtt_client.publish(mqtt_topic, payload, qos=qos)
            logger.info(f"Published to MQTT topic '{mqtt_topic}': {payload}")
        except Exception as e:
            logger.error(f"Failed to publish to MQTT topic '{mqtt_topic}': {e}")

    def _run_http_server(self):
        app = self._flask_app

        @app.route("/health", methods=["GET"])
        def health():
            return jsonify({"status": "healthy"}), 200

        @app.route("/status", methods=["GET"])
        def status():
            try:
                status_data = self.latest_data.get("status", {})
                return jsonify(status_data), 200
            except Exception as e:
                logger.error(f"Failed to get status: {e}")
                return jsonify({"error": "Failed to get status"}), 500

        try:
            app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)
        except Exception as e:
            logger.error(f"HTTP server failed: {e}")

    def _run_mqtt_loop(self):
        while not shutdown_flag.is_set():
            try:
                if not self.mqtt_client:
                    self.connect_mqtt()
                self.mqtt_client.loop_forever()
            except Exception as e:
                logger.error(f"MQTT loop error: {e}")
                time.sleep(5)

    def connect_mqtt(self):
        client = mqtt.Client(client_id=self.mqtt_client_id, clean_session=True)
        if MQTT_BROKER_USERNAME and MQTT_BROKER_PASSWORD:
            client.username_pw_set(MQTT_BROKER_USERNAME, MQTT_BROKER_PASSWORD)
        client.on_connect = self.on_mqtt_connect
        client.on_message = self.on_mqtt_message
        client.on_disconnect = self.on_mqtt_disconnect
        self.mqtt_client = client
        try:
            client.connect(MQTT_BROKER, MQTT_BROKER_PORT, keepalive=60)
            logger.info(f"Connecting to MQTT broker {MQTT_BROKER}:{MQTT_BROKER_PORT} as {self.mqtt_client_id}")
        except Exception as e:
            logger.error(f"MQTT connection failed: {e}")
            self.connected = False

    def on_mqtt_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            logger.info("MQTT connected successfully.")
            # Subscribe to control topics
            for control_topic in [
                f"{MQTT_TOPIC_PREFIX}/{self.device_name}/control/reboot",
                f"{MQTT_TOPIC_PREFIX}/{self.device_name}/control/playback"
            ]:
                try:
                    client.subscribe(control_topic, qos=1)
                    logger.info(f"Subscribed to control topic: {control_topic}")
                except Exception as e:
                    logger.error(f"Failed to subscribe to {control_topic}: {e}")
            # Subscribe to telemetry topic if needed
            telemetry_topic = f"{MQTT_TOPIC_PREFIX}/{self.device_name}/status"
            try:
                client.subscribe(telemetry_topic, qos=1)
                logger.info(f"Subscribed to telemetry topic: {telemetry_topic}")
            except Exception as e:
                logger.error(f"Failed to subscribe to {telemetry_topic}: {e}")
        else:
            logger.error(f"MQTT connect failed with rc={rc}")
            self.connected = False

    def on_mqtt_disconnect(self, client, userdata, rc):
        self.connected = False
        if rc != 0:
            logger.warning(f"Unexpected MQTT disconnection (rc={rc}). Attempting to reconnect.")
            try:
                client.reconnect()
            except Exception as e:
                logger.error(f"MQTT reconnect failed: {e}")
                time.sleep(5)

    def on_mqtt_message(self, client, userdata, msg):
        logger.info(f"MQTT message received: {msg.topic} {msg.payload}")
        try:
            payload = json.loads(msg.payload.decode('utf-8'))
        except Exception as e:
            logger.error(f"Failed to decode MQTT message: {e}")
            return

        if msg.topic.endswith("/control/reboot"):
            self._handle_reboot_command(payload)
        elif msg.topic.endswith("/control/playback"):
            self._handle_playback_command(payload)
        else:
            logger.warning(f"Unhandled topic: {msg.topic}")

    def _handle_reboot_command(self, payload: Dict[str, Any]):
        logger.info(f"Handling reboot command: {payload}")
        # Simulate reboot: update status, publish confirmation
        if payload.get("command") == "reboot":
            status = {"status": "rebooting", "timestamp": int(time.time())}
            self.latest_data["status"] = status
            self._publish_mqtt_data("status", status, qos=2)
            # Simulate reboot delay
            time.sleep(2)
            self.latest_data["status"] = {"status": "OK", "timestamp": int(time.time())}
            self._publish_mqtt_data("status", self.latest_data["status"], qos=1)
            logger.info("Device reboot simulated and status updated.")

    def _handle_playback_command(self, payload: Dict[str, Any]):
        logger.info(f"Handling playback command: {payload}")
        status = {"playback_action": payload.get("action"), "params": payload.get("params", {}), "timestamp": int(time.time())}
        # Simulate playback state update
        self.latest_data["playback"] = status
        self._publish_mqtt_data("playback", status, qos=1)

    def setup_routes(self):
        # Already set up in _run_http_server()
        pass

    def signal_handler(self, signum, frame):
        logger.info(f"Signal received: {signum}")
        shutdown_flag.set()
        self.shutdown()

    def shutdown(self):
        logger.info("Shutting down driver.")
        try:
            if self.mqtt_client:
                self.mqtt_client.disconnect()
                logger.info("MQTT client disconnected.")
        except Exception as e:
            logger.error(f"Error during MQTT disconnect: {e}")
        # Wait for threads to exit
        for t in self.publish_threads.values():
            t.join(timeout=2)
        if self._http_thread.is_alive():
            # Flask doesn't provide a native shutdown in threaded mode
            logger.info("HTTP server will terminate with main process.")
        logger.info("Shutdown complete.")

    def run(self):
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
        self._http_thread.start()
        self._mqtt_thread.start()
        self._start_scheduled_publishers()
        logger.info("DeviceShifu MQTT Driver started.")
        try:
            while not shutdown_flag.is_set():
                time.sleep(1)
        except KeyboardInterrupt:
            logger.info("KeyboardInterrupt received, shutting down.")
            shutdown_flag.set()
        self.shutdown()

if __name__ == "__main__":
    driver = DeviceShifuMQTTDriver()
    driver.run()