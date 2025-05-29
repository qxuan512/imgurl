import os
import yaml
import json
import threading
import time
from typing import Callable, Dict, Any
from flask import Flask, request, jsonify
import paho.mqtt.client as mqtt
from kubernetes import client as k8s_client, config as k8s_config

# CONSTANTS
CONFIG_PATH = "/etc/edgedevice/config/instructions"
INSTRUCTION_FILE = os.path.join(CONFIG_PATH, "instructions.yaml")
EDGEDEVICE_CRD_GROUP = "shifu.edgenesis.io"
EDGEDEVICE_CRD_VERSION = "v1alpha1"
EDGEDEVICE_CRD_PLURAL = "edgedevices"

# ENVIRONMENT VARIABLES
EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE")
MQTT_BROKER_ADDRESS = os.environ.get("MQTT_BROKER_ADDRESS")
MQTT_KEEPALIVE = int(os.environ.get("MQTT_KEEPALIVE", "60"))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME", "")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")
MQTT_CLIENT_ID = os.environ.get("MQTT_CLIENT_ID", "deviceshifu-car-mqtt")
MQTT_TLS = os.environ.get("MQTT_TLS", "false").lower() == "true"
MQTT_PREFIX = "deviceshifu-car-mqtt"

if not EDGEDEVICE_NAME or not EDGEDEVICE_NAMESPACE or not MQTT_BROKER_ADDRESS:
    raise Exception("Required environment variables: EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE, MQTT_BROKER_ADDRESS")

# K8s API Initialization
k8s_config.load_incluster_config()
custom_api = k8s_client.CustomObjectsApi()

def update_edgedevice_phase(phase: str):
    body = {"status": {"edgeDevicePhase": phase}}
    try:
        custom_api.patch_namespaced_custom_object_status(
            group=EDGEDEVICE_CRD_GROUP,
            version=EDGEDEVICE_CRD_VERSION,
            namespace=EDGEDEVICE_NAMESPACE,
            plural=EDGEDEVICE_CRD_PLURAL,
            name=EDGEDEVICE_NAME,
            body=body
        )
    except Exception:
        pass  # Optionally log error

# Load EdgeDevice Spec for device address
def fetch_device_address():
    try:
        ed = custom_api.get_namespaced_custom_object(
            EDGEDEVICE_CRD_GROUP,
            EDGEDEVICE_CRD_VERSION,
            EDGEDEVICE_NAMESPACE,
            EDGEDEVICE_CRD_PLURAL,
            EDGEDEVICE_NAME
        )
        return ed.get("spec", {}).get("address")
    except Exception:
        return None

# Load instruction settings
def load_instruction_settings():
    try:
        with open(INSTRUCTION_FILE, "r") as f:
            return yaml.safe_load(f)
    except Exception:
        return {}

instruction_settings = load_instruction_settings()

# MQTT Client Setup
mqtt_client = mqtt.Client(MQTT_CLIENT_ID, clean_session=True)
if MQTT_USERNAME:
    mqtt_client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
if MQTT_TLS:
    mqtt_client.tls_set()

# MQTT Event Handlers
mqtt_connected = threading.Event()
mqtt_failed = threading.Event()
mqtt_subscriptions = {}

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        mqtt_connected.set()
        mqtt_failed.clear()
        update_edgedevice_phase("Running")
        for topic, callback in mqtt_subscriptions.items():
            qos = callback["qos"]
            client.subscribe(topic, qos)
    else:
        mqtt_connected.clear()
        mqtt_failed.set()
        update_edgedevice_phase("Failed")

def on_disconnect(client, userdata, rc):
    mqtt_connected.clear()
    if rc != 0:
        mqtt_failed.set()
        update_edgedevice_phase("Failed")
    else:
        update_edgedevice_phase("Pending")

def on_message(client, userdata, msg):
    topic = msg.topic
    if topic in mqtt_subscriptions:
        callback = mqtt_subscriptions[topic]["callback"]
        try:
            payload = msg.payload.decode("utf-8")
            data = json.loads(payload)
        except Exception:
            data = msg.payload
        callback(data)

mqtt_client.on_connect = on_connect
mqtt_client.on_disconnect = on_disconnect
mqtt_client.on_message = on_message

# EdgeDevice Phase Management Thread
def phase_monitor():
    while True:
        if not mqtt_connected.is_set():
            update_edgedevice_phase("Pending")
        time.sleep(10)

monitor_thread = threading.Thread(target=phase_monitor, daemon=True)
monitor_thread.start()

# MQTT Connect
def mqtt_connect():
    host, port = MQTT_BROKER_ADDRESS.split(":")
    mqtt_client.connect(host, int(port), MQTT_KEEPALIVE)
    mqtt_client.loop_start()

def mqtt_disconnect():
    mqtt_client.loop_stop()
    mqtt_client.disconnect()

# API Implementation
app = Flask(__name__)
received_data = {
    MQTT_PREFIX + "/odom": None,
    MQTT_PREFIX + "/scan": None,
    MQTT_PREFIX + "/joint_states": None,
    MQTT_PREFIX + "/point_cloud": None,
    MQTT_PREFIX + "/status": None,
}

def make_pub_api(endpoint: str, qos: int):
    @app.route(f"/{endpoint}", methods=["POST"])
    def handler():
        payload = request.get_json(force=True, silent=True) or {}
        topic = f"{MQTT_PREFIX}/{endpoint.split('/')[-1]}"
        settings = instruction_settings.get(endpoint, {}).get("protocolPropertyList", {})
        pub_qos = int(settings.get("qos", qos))
        ret = mqtt_client.publish(topic, json.dumps(payload), qos=pub_qos)
        if ret.rc == mqtt.MQTT_ERR_SUCCESS:
            return jsonify({"result": "published", "topic": topic}), 200
        else:
            return jsonify({"result": "publish_failed", "topic": topic}), 500
    handler.__name__ = f"api_{endpoint.replace('/', '_')}_pub"
    return handler

def make_sub_api(endpoint: str):
    @app.route(f"/{endpoint}", methods=["GET"])
    def handler():
        topic = f"{MQTT_PREFIX}/{endpoint.split('/')[-1]}"
        data = received_data.get(topic)
        if data is not None:
            return jsonify(data), 200
        else:
            return jsonify({"result": "no_data"}), 404
    handler.__name__ = f"api_{endpoint.replace('/', '_')}_sub"
    return handler

# MQTT Subscription Callback Registration
def sub_callback_factory(topic):
    def callback(data):
        received_data[topic] = data
    return callback

# API Endpoints Definition
pub_apis = [
    {"endpoint": "move/forward", "qos": 1},
    {"endpoint": "move/backward", "qos": 1},
    {"endpoint": "turn/left", "qos": 1},
    {"endpoint": "turn/right", "qos": 1},
    {"endpoint": "stop", "qos": 1},
]
sub_apis = [
    {"endpoint": "odom", "qos": 1},
    {"endpoint": "scan", "qos": 1},
    {"endpoint": "joint_states", "qos": 1},
    {"endpoint": "point_cloud", "qos": 1},
    {"endpoint": "status", "qos": 1},
]

# Register Publish APIs
for api in pub_apis:
    make_pub_api(api["endpoint"], api["qos"])

# Register Subscribe APIs and MQTT Subscriptions
for api in sub_apis:
    topic = f"{MQTT_PREFIX}/{api['endpoint']}"
    received_data[topic] = None
    mqtt_subscriptions[topic] = {
        "callback": sub_callback_factory(topic),
        "qos": api["qos"]
    }
    make_sub_api(api["endpoint"])

# MQTT Subscribe on Connect
def subscribe_topics():
    for topic, sub in mqtt_subscriptions.items():
        mqtt_client.subscribe(topic, sub["qos"])

# Main Run
if __name__ == "__main__":
    update_edgedevice_phase("Pending")
    device_address = fetch_device_address()
    if device_address is None:
        update_edgedevice_phase("Unknown")
    try:
        mqtt_connect()
        if mqtt_connected.wait(timeout=10):
            update_edgedevice_phase("Running")
        else:
            update_edgedevice_phase("Failed")
    except Exception:
        update_edgedevice_phase("Failed")

    subscribe_topics()
    app.run(host="0.0.0.0", port=int(os.environ.get("HTTP_PORT", "8080")), threaded=True)