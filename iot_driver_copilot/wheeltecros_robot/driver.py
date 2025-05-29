import os
import sys
import yaml
import threading
import logging
import signal
import json
from typing import Any, Dict, Callable

import paho.mqtt.client as mqtt

from kubernetes import client as k8s_client, config as k8s_config, watch as k8s_watch

# -------------------- CONFIGURATION & CONSTANTS --------------------

CONFIGMAP_INSTRUCTIONS_PATH = '/etc/edgedevice/config/instructions'
MQTT_BROKER_ADDRESS = os.environ.get('MQTT_BROKER_ADDRESS')
EDGEDEVICE_NAME = os.environ.get('EDGEDEVICE_NAME')
EDGEDEVICE_NAMESPACE = os.environ.get('EDGEDEVICE_NAMESPACE')

if not (MQTT_BROKER_ADDRESS and EDGEDEVICE_NAME and EDGEDEVICE_NAMESPACE):
    sys.stderr.write("Missing required environment variables: MQTT_BROKER_ADDRESS, EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE\n")
    sys.exit(1)

# MQTT topics to subscribe (from given driver api info)
CONTROL_TOPICS = [
    'wheeltecros/move/forward',
    'wheeltecros/move/backward',
    'wheeltecros/turn/left',
    'wheeltecros/turn/right',
    'wheeltecros/stop'
]

# For deduplication, only subscribe each topic once
CONTROL_TOPICS = list(set(CONTROL_TOPICS))

# -------------------- LOGGING SETUP --------------------

logging.basicConfig(
    level=logging.INFO,
    format='[%(asctime)s] %(levelname)s %(message)s',
    handlers=[logging.StreamHandler()]
)

logger = logging.getLogger('DeviceShifu')

# -------------------- LOAD DEVICE INSTRUCTIONS --------------------

def load_instructions(config_path: str) -> Dict[str, Any]:
    if not os.path.exists(config_path):
        logger.warning(f"Instructions config not found: {config_path}")
        return {}
    with open(config_path, 'r') as f:
        return yaml.safe_load(f) or {}

instructions = load_instructions(CONFIGMAP_INSTRUCTIONS_PATH)

# -------------------- KUBERNETES CLIENT SETUP --------------------

def get_k8s_client():
    try:
        k8s_config.load_incluster_config()
    except Exception as e:
        logger.error(f"Failed to load in-cluster config: {e}")
        sys.exit(1)
    return k8s_client.CustomObjectsApi()

k8s_api = get_k8s_client()

EDGEDEVICE_CRD_GROUP = "shifu.edgenesis.io"
EDGEDEVICE_CRD_VERSION = "v1alpha1"
EDGEDEVICE_CRD_PLURAL = "edgedevices"

# -------------------- EDGEDEVICE STATUS MANAGEMENT --------------------

def update_edgedevice_phase(phase: str):
    body = {"status": {"edgeDevicePhase": phase}}
    try:
        k8s_api.patch_namespaced_custom_object_status(
            group=EDGEDEVICE_CRD_GROUP,
            version=EDGEDEVICE_CRD_VERSION,
            namespace=EDGEDEVICE_NAMESPACE,
            plural=EDGEDEVICE_CRD_PLURAL,
            name=EDGEDEVICE_NAME,
            body=body
        )
        logger.info(f"Updated EdgeDevice status to phase: {phase}")
    except Exception as e:
        logger.error(f"Failed to update EdgeDevice status: {e}")

def get_edgedevice_address() -> str:
    try:
        dev = k8s_api.get_namespaced_custom_object(
            group=EDGEDEVICE_CRD_GROUP,
            version=EDGEDEVICE_CRD_VERSION,
            namespace=EDGEDEVICE_NAMESPACE,
            plural=EDGEDEVICE_CRD_PLURAL,
            name=EDGEDEVICE_NAME
        )
        return dev.get("spec", {}).get("address", "")
    except Exception as e:
        logger.error(f"Failed to get EdgeDevice: {e}")
        return ""

# -------------------- MQTT CLIENT SETUP --------------------

class WheeltecrosMQTTClient:
    def __init__(self, broker_addr: str, topics: list, instructions: Dict[str, Any]):
        self.broker_addr = broker_addr
        self.topics = topics
        self.instructions = instructions
        self.client = mqtt.Client()
        self.connected_event = threading.Event()
        self.should_stop = threading.Event()
        self.subscribed_topics = set()
        self._setup_handlers()

    def _setup_handlers(self):
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.on_subscribe = self._on_subscribe

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            logger.info("Connected to MQTT broker successfully.")
            self.connected_event.set()
            update_edgedevice_phase("Running")
            self._subscribe_topics()
        else:
            logger.error("Failed to connect to MQTT broker, rc=%s", rc)
            update_edgedevice_phase("Failed")

    def _on_disconnect(self, client, userdata, rc):
        if rc != 0:
            logger.warning("Unexpected MQTT disconnection.")
            update_edgedevice_phase("Pending")
        else:
            logger.info("Disconnected from MQTT broker gracefully.")
            update_edgedevice_phase("Pending")
        self.connected_event.clear()
        self.subscribed_topics.clear()

    def _on_subscribe(self, client, userdata, mid, granted_qos):
        logger.info(f"Subscribed (mid={mid}) with QoS: {granted_qos}")

    def _on_message(self, client, userdata, msg):
        logger.info(f"Received MQTT message on topic '{msg.topic}': {msg.payload}")
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            payload = msg.payload.decode("utf-8")
        method = "SUBSCRIBE"
        topic_key = msg.topic.replace('/', '_')
        api_instruction = self.instructions.get(topic_key, {})
        self.handle_device_command(msg.topic, payload, api_instruction)

    def _subscribe_topics(self):
        for topic in self.topics:
            if topic not in self.subscribed_topics:
                # Get QoS from instructions or default to 1
                topic_key = topic.replace('/', '_')
                qos = 1
                if topic_key in self.instructions:
                    protocol_props = self.instructions[topic_key].get('protocolPropertyList', {})
                    qos = int(protocol_props.get('qos', 1))
                self.client.subscribe(topic, qos)
                self.subscribed_topics.add(topic)
                logger.info(f"Subscribing to topic: {topic} with QoS {qos}")

    def handle_device_command(self, topic: str, payload: Any, api_instruction: Dict[str, Any]):
        # The actual command handling for each topic
        if topic.endswith("move/forward"):
            self._move_forward(payload)
        elif topic.endswith("move/backward"):
            self._move_backward(payload)
        elif topic.endswith("turn/left"):
            self._turn_left(payload)
        elif topic.endswith("turn/right"):
            self._turn_right(payload)
        elif topic.endswith("stop"):
            self._stop(payload)
        else:
            logger.warning(f"Received command for unknown topic: {topic}")

    def _move_forward(self, payload: Any):
        logger.info(f"Handling move forward with payload: {payload}")
        # Implement actual device communication here

    def _move_backward(self, payload: Any):
        logger.info(f"Handling move backward with payload: {payload}")
        # Implement actual device communication here

    def _turn_left(self, payload: Any):
        logger.info(f"Handling turn left with payload: {payload}")
        # Implement actual device communication here

    def _turn_right(self, payload: Any):
        logger.info(f"Handling turn right with payload: {payload}")
        # Implement actual device communication here

    def _stop(self, payload: Any):
        logger.info(f"Handling stop with payload: {payload}")
        # Implement actual device communication here

    def loop_forever(self):
        while not self.should_stop.is_set():
            try:
                self.client.loop(timeout=1.0)
            except Exception as e:
                logger.error(f"Exception in MQTT loop: {e}")
                update_edgedevice_phase("Failed")
                self.connected_event.clear()
                break

    def start(self):
        try:
            broker, port = self.broker_addr.split(':')
            port = int(port)
        except Exception:
            logger.error(f"Invalid MQTT_BROKER_ADDRESS: {self.broker_addr}")
            update_edgedevice_phase("Failed")
            return
        update_edgedevice_phase("Pending")
        self.client.connect_async(broker, port, keepalive=60)
        self.client.loop_start()

    def stop(self):
        self.should_stop.set()
        try:
            self.client.disconnect()
        except Exception:
            pass
        self.client.loop_stop()

# -------------------- SIGNAL HANDLING --------------------

def handle_signal(signum, frame):
    logger.info("Received termination signal, shutting down...")
    update_edgedevice_phase("Pending")
    mqtt_client.stop()
    sys.exit(0)

signal.signal(signal.SIGTERM, handle_signal)
signal.signal(signal.SIGINT, handle_signal)

# -------------------- MAIN --------------------

if __name__ == "__main__":
    update_edgedevice_phase("Pending")
    device_address = get_edgedevice_address()
    logger.info(f"EdgeDevice Address from CRD: {device_address}")

    mqtt_client = WheeltecrosMQTTClient(
        broker_addr=MQTT_BROKER_ADDRESS,
        topics=CONTROL_TOPICS,
        instructions=instructions
    )
    mqtt_client.start()

    try:
        while True:
            signal.pause()
    except KeyboardInterrupt:
        handle_signal(signal.SIGINT, None)