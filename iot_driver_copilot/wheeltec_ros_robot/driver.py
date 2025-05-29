import os
import time
import yaml
import json
import threading
import logging
from kubernetes import client, config
from kubernetes.client.rest import ApiException
import paho.mqtt.client as mqtt

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE")
MQTT_BROKER_ADDRESS = os.environ.get("MQTT_BROKER_ADDRESS")
if not EDGEDEVICE_NAME or not EDGEDEVICE_NAMESPACE or not MQTT_BROKER_ADDRESS:
    raise EnvironmentError("EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE, and MQTT_BROKER_ADDRESS must be set.")

CONFIG_PATH = "/etc/edgedevice/config/instructions"

# MQTT Topics
TOPIC_PREFIX = "deviceshifu-car-mqtt"
PUBLISH_TOPICS = {
    "odom": f"{TOPIC_PREFIX}/odom",
    "scan": f"{TOPIC_PREFIX}/scan",
    "joint_states": f"{TOPIC_PREFIX}/joint_states",
    "point_cloud": f"{TOPIC_PREFIX}/point_cloud",
    "status": f"{TOPIC_PREFIX}/status",
}
SUBSCRIBE_TOPICS = {
    "move_forward": f"{TOPIC_PREFIX}/move/forward",
    "move_backward": f"{TOPIC_PREFIX}/move/backward",
    "turn_left": f"{TOPIC_PREFIX}/turn/left",
    "turn_right": f"{TOPIC_PREFIX}/turn/right",
    "stop": f"{TOPIC_PREFIX}/stop",
}

# EdgeDevice CRD info
CRD_GROUP = "shifu.edgenesis.io"
CRD_VERSION = "v1alpha1"
CRD_PLURAL = "edgedevices"

EDGEDEVICE_PHASE_PENDING = "Pending"
EDGEDEVICE_PHASE_RUNNING = "Running"
EDGEDEVICE_PHASE_FAILED = "Failed"
EDGEDEVICE_PHASE_UNKNOWN = "Unknown"

class EdgeDeviceStatusManager:
    def __init__(self, name, namespace):
        config.load_incluster_config()
        self.name = name
        self.namespace = namespace
        self.api = client.CustomObjectsApi()
        self.current_phase = None
        self.lock = threading.Lock()

    def set_phase(self, phase):
        with self.lock:
            if self.current_phase == phase:
                return
            body = {"status": {"edgeDevicePhase": phase}}
            try:
                self.api.patch_namespaced_custom_object_status(
                    group=CRD_GROUP,
                    version=CRD_VERSION,
                    namespace=self.namespace,
                    plural=CRD_PLURAL,
                    name=self.name,
                    body=body,
                )
                self.current_phase = phase
                logging.info(f"EdgeDevice phase updated to {phase}")
            except ApiException as e:
                logging.error(f"Failed to update EdgeDevice status: {e}")

    def get_device_address(self):
        try:
            result = self.api.get_namespaced_custom_object(
                group=CRD_GROUP,
                version=CRD_VERSION,
                namespace=self.namespace,
                plural=CRD_PLURAL,
                name=self.name,
            )
            return result.get("spec", {}).get("address", "")
        except ApiException as e:
            logging.error(f"Failed to get EdgeDevice CRD: {e}")
            return ""

class DeviceShifuMQTTClient:
    def __init__(self, broker_addr, status_manager, config_path):
        self.broker_addr, self.broker_port = self._parse_broker_addr(broker_addr)
        self.status_manager = status_manager
        self.mqttc = mqtt.Client()
        self.mqttc.on_connect = self.on_connect
        self.mqttc.on_disconnect = self.on_disconnect
        self.mqttc.on_message = self.on_message
        self.instruction = self._load_instruction(config_path)
        self.subscriptions = {}
        self._setup_subscriptions()
        self.connected = False
        self.device_address = self.status_manager.get_device_address()
        # For keep-alive
        self._keepalive_thread = threading.Thread(target=self._keepalive, daemon=True)
        self._stop = threading.Event()

    def _parse_broker_addr(self, broker_addr):
        if ":" not in broker_addr:
            return broker_addr, 1883
        host, port = broker_addr.split(":", 1)
        return host, int(port)

    def _load_instruction(self, path):
        try:
            with open(path, "r") as f:
                return yaml.safe_load(f)
        except Exception as e:
            logging.warning(f"Could not load instruction config: {e}")
            return {}

    def _setup_subscriptions(self):
        # Map topic to handler
        self.subscriptions = {
            SUBSCRIBE_TOPICS["move_forward"]: self.handle_move_forward,
            SUBSCRIBE_TOPICS["move_backward"]: self.handle_move_backward,
            SUBSCRIBE_TOPICS["turn_left"]: self.handle_turn_left,
            SUBSCRIBE_TOPICS["turn_right"]: self.handle_turn_right,
            SUBSCRIBE_TOPICS["stop"]: self.handle_stop,
        }

    def connect(self):
        try:
            self.status_manager.set_phase(EDGEDEVICE_PHASE_PENDING)
            self.mqttc.connect(self.broker_addr, self.broker_port, keepalive=60)
            self.mqttc.loop_start()
            self._keepalive_thread.start()
        except Exception as e:
            logging.error(f"MQTT connection failed: {e}")
            self.status_manager.set_phase(EDGEDEVICE_PHASE_FAILED)

    def disconnect(self):
        self._stop.set()
        self.mqttc.disconnect()
        self.mqttc.loop_stop()

    # MQTT callbacks
    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            self.status_manager.set_phase(EDGEDEVICE_PHASE_RUNNING)
            for topic in self.subscriptions:
                qos = self._get_topic_qos(topic)
                client.subscribe(topic, qos)
                logging.info(f"Subscribed to MQTT topic: {topic}")
        else:
            self.status_manager.set_phase(EDGEDEVICE_PHASE_FAILED)
            logging.error(f"MQTT connect failed with rc={rc}")

    def on_disconnect(self, client, userdata, rc):
        self.connected = False
        if rc != 0:
            self.status_manager.set_phase(EDGEDEVICE_PHASE_FAILED)
            logging.warning("Unexpected disconnection.")
        else:
            self.status_manager.set_phase(EDGEDEVICE_PHASE_UNKNOWN)

    def on_message(self, client, userdata, msg):
        handler = self.subscriptions.get(msg.topic, None)
        if handler:
            try:
                payload = msg.payload.decode("utf-8")
                handler(payload)
            except Exception as e:
                logging.error(f"Failed to handle message on {msg.topic}: {e}")

    # Topic QoS from instruction config
    def _get_topic_qos(self, topic):
        api_key = topic.split("/", 1)[-1]
        api_settings = self.instruction.get(api_key, {})
        return int(api_settings.get("protocolPropertyList", {}).get("qos", 1))

    # Keepalive: Check device connection
    def _keepalive(self):
        while not self._stop.is_set():
            if not self.connected:
                self.status_manager.set_phase(EDGEDEVICE_PHASE_PENDING)
            time.sleep(5)

    # --- Handlers for each subscribed command topic ---
    def handle_move_forward(self, payload):
        logging.info(f"Received move_forward: {payload}")
        # Actual robot command logic should be implemented here

    def handle_move_backward(self, payload):
        logging.info(f"Received move_backward: {payload}")

    def handle_turn_left(self, payload):
        logging.info(f"Received turn_left: {payload}")

    def handle_turn_right(self, payload):
        logging.info(f"Received turn_right: {payload}")

    def handle_stop(self, payload):
        logging.info(f"Received stop: {payload}")

    # --- API Methods for publishing data ---
    def publish_odom(self, data):
        self._publish(PUBLISH_TOPICS["odom"], data)

    def publish_scan(self, data):
        self._publish(PUBLISH_TOPICS["scan"], data)

    def publish_joint_states(self, data):
        self._publish(PUBLISH_TOPICS["joint_states"], data)

    def publish_point_cloud(self, data):
        self._publish(PUBLISH_TOPICS["point_cloud"], data)

    def publish_status(self, data):
        self._publish(PUBLISH_TOPICS["status"], data)

    # --- Generic publish ---
    def _publish(self, topic, data):
        if not self.connected:
            logging.warning(f"Cannot publish to {topic}: MQTT not connected.")
            return
        qos = self._get_topic_qos(topic)
        payload = json.dumps(data) if not isinstance(data, str) else data
        try:
            self.mqttc.publish(topic, payload, qos=qos)
            logging.info(f"Published to {topic}: {payload}")
        except Exception as e:
            logging.error(f"Failed to publish {topic}: {e}")

def main():
    status_manager = EdgeDeviceStatusManager(EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE)
    mqtt_client = DeviceShifuMQTTClient(MQTT_BROKER_ADDRESS, status_manager, CONFIG_PATH)
    mqtt_client.connect()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        logging.info("Shutting down DeviceShifu MQTT Client.")
        mqtt_client.disconnect()

if __name__ == "__main__":
    main()