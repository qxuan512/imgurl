import os
import sys
import signal
import time
import threading
import yaml
import json

from kubernetes import client, config
from kubernetes.client.rest import ApiException

import paho.mqtt.client as mqtt
from flask import Flask, request, jsonify

# --- Constants & Env Vars ---
EDGEDEVICE_NAME = os.environ.get('EDGEDEVICE_NAME')
EDGEDEVICE_NAMESPACE = os.environ.get('EDGEDEVICE_NAMESPACE')
MQTT_BROKER_ADDRESS = os.environ.get('MQTT_BROKER_ADDRESS')

CONFIGMAP_INSTRUCTIONS_PATH = '/etc/edgedevice/config/instructions'

if not EDGEDEVICE_NAME or not EDGEDEVICE_NAMESPACE or not MQTT_BROKER_ADDRESS:
    sys.stderr.write('Missing required environment variables.\n')
    sys.exit(1)

EDGEDEVICE_CRD_GROUP = 'shifu.edgenesis.io'
EDGEDEVICE_CRD_VERSION = 'v1alpha1'
EDGEDEVICE_CRD_PLURAL = 'edgedevices'

PHASE_PENDING = 'Pending'
PHASE_RUNNING = 'Running'
PHASE_FAILED = 'Failed'
PHASE_UNKNOWN = 'Unknown'

# --- Global State ---
stop_event = threading.Event()
device_connected = threading.Event()
device_failed = threading.Event()

api_instruction_settings = {}

# --- MQTT Client Setup ---
mqtt_client = None

# --- Flask App for DeviceShifu API ---
app = Flask(__name__)

# --- Kubernetes API Setup ---
def load_k8s_config():
    try:
        config.load_incluster_config()
    except Exception:
        config.load_kube_config()

load_k8s_config()
crd_api = client.CustomObjectsApi()

def get_edgedevice():
    try:
        return crd_api.get_namespaced_custom_object(
            group=EDGEDEVICE_CRD_GROUP,
            version=EDGEDEVICE_CRD_VERSION,
            namespace=EDGEDEVICE_NAMESPACE,
            plural=EDGEDEVICE_CRD_PLURAL,
            name=EDGEDEVICE_NAME
        )
    except ApiException:
        return None

def update_edgedevice_phase(phase):
    for _ in range(5):
        try:
            ed = get_edgedevice()
            if not ed:
                return
            if 'status' not in ed:
                ed['status'] = {}
            if ed['status'].get('edgeDevicePhase', '') == phase:
                return
            ed['status']['edgeDevicePhase'] = phase
            crd_api.patch_namespaced_custom_object_status(
                group=EDGEDEVICE_CRD_GROUP,
                version=EDGEDEVICE_CRD_VERSION,
                namespace=EDGEDEVICE_NAMESPACE,
                plural=EDGEDEVICE_CRD_PLURAL,
                name=EDGEDEVICE_NAME,
                body={'status': {'edgeDevicePhase': phase}}
            )
            return
        except ApiException:
            time.sleep(1)
    return

def get_device_address():
    ed = get_edgedevice()
    if not ed:
        return None
    return ed.get('spec', {}).get('address')

# --- Instruction Config Parsing ---
def load_instruction_settings():
    global api_instruction_settings
    try:
        with open(CONFIGMAP_INSTRUCTIONS_PATH, 'r') as f:
            api_instruction_settings = yaml.safe_load(f)
    except Exception:
        api_instruction_settings = {}

load_instruction_settings()

def get_api_settings(api):
    return api_instruction_settings.get(api, {}).get('protocolPropertyList', {})

# --- MQTT Logic ---
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        device_connected.set()
        device_failed.clear()
        update_edgedevice_phase(PHASE_RUNNING)
    else:
        device_failed.set()
        device_connected.clear()
        update_edgedevice_phase(PHASE_FAILED)

def on_disconnect(client, userdata, rc):
    device_connected.clear()
    if rc != 0:
        device_failed.set()
        update_edgedevice_phase(PHASE_FAILED)
    else:
        update_edgedevice_phase(PHASE_PENDING)

def on_message_factory():
    # Each subscribe endpoint installs its own callback.
    # We'll store the last payload for each topic.
    last_payloads = {}
    lock = threading.Lock()
    def on_message(client, userdata, msg):
        with lock:
            last_payloads[msg.topic] = msg.payload.decode()
    on_message.last_payloads = last_payloads
    on_message.lock = lock
    return on_message

mqtt_on_message = on_message_factory()

def mqtt_loop_thread():
    global mqtt_client
    while not stop_event.is_set():
        try:
            if mqtt_client is None:
                mqtt_client = mqtt.Client()
                mqtt_client.on_connect = on_connect
                mqtt_client.on_disconnect = on_disconnect
                mqtt_client.on_message = mqtt_on_message
                broker_host, broker_port = MQTT_BROKER_ADDRESS.split(':')
                mqtt_client.connect(broker_host, int(broker_port), 60)
                mqtt_client.loop_start()
            else:
                if not device_connected.is_set():
                    try:
                        mqtt_client.reconnect()
                    except Exception:
                        device_failed.set()
                        update_edgedevice_phase(PHASE_FAILED)
                        time.sleep(2)
                else:
                    time.sleep(1)
        except Exception:
            device_failed.set()
            update_edgedevice_phase(PHASE_FAILED)
            time.sleep(2)

mqtt_thread = threading.Thread(target=mqtt_loop_thread, daemon=True)
mqtt_thread.start()

# --- API Implementations ---
# For each SUBSCRIBE, maintain a subscription and endpoint to get the latest msg.
# For each PUBLISH, provide an endpoint to send a message.

# 1. Subscribe Battery Voltage
@app.route('/subscribe/battery', methods=['GET'])
def api_subscribe_battery():
    topic = 'device/sensors/battery'
    qos = 1
    mqtt_client.subscribe(topic, qos)
    with mqtt_on_message.lock:
        value = mqtt_on_message.last_payloads.get(topic)
    if value:
        try:
            return jsonify(json.loads(value))
        except Exception:
            return value, 200
    else:
        return jsonify({'status': 'No data'})

# 2. Subscribe Odometry
@app.route('/subscribe/odometry', methods=['GET'])
def api_subscribe_odometry():
    topic = 'device/sensors/odometry'
    qos = 1
    mqtt_client.subscribe(topic, qos)
    with mqtt_on_message.lock:
        value = mqtt_on_message.last_payloads.get(topic)
    if value:
        try:
            return jsonify(json.loads(value))
        except Exception:
            return value, 200
    else:
        return jsonify({'status': 'No data'})

# 3. Publish Voice Command
@app.route('/publish/voice', methods=['POST'])
def api_publish_voice():
    topic = 'device/commands/voice'
    qos = 1
    payload = request.get_json(force=True)
    mqtt_client.publish(topic, json.dumps(payload), qos=qos)
    return jsonify({'status': 'published'})

# 4. Publish Motion Control (cmd_vel)
@app.route('/publish/cmd_vel', methods=['POST'])
def api_publish_cmd_vel():
    topic = 'device/commands/cmd_vel'
    qos = 2
    payload = request.get_json(force=True)
    mqtt_client.publish(topic, json.dumps(payload), qos=qos)
    return jsonify({'status': 'published'})

# 5. Publish Navigation Goal
@app.route('/publish/nav_goal', methods=['POST'])
def api_publish_nav_goal():
    topic = 'device/commands/nav_goal'
    qos = 1
    payload = request.get_json(force=True)
    mqtt_client.publish(topic, json.dumps(payload), qos=qos)
    return jsonify({'status': 'published'})

# --- Health & Status Endpoints ---
@app.route('/healthz', methods=['GET'])
def healthz():
    if device_connected.is_set():
        return 'ok', 200
    return 'not connected', 503

@app.route('/status', methods=['GET'])
def status():
    ed = get_edgedevice()
    if not ed:
        return jsonify({'edgeDevicePhase': PHASE_UNKNOWN}), 503
    status = ed.get('status', {})
    return jsonify({'edgeDevicePhase': status.get('edgeDevicePhase', PHASE_UNKNOWN)})

# --- Signal Handling ---
def shutdown_handler(signum, frame):
    stop_event.set()
    try:
        if mqtt_client:
            mqtt_client.loop_stop()
            mqtt_client.disconnect()
    except Exception:
        pass
    sys.exit(0)

signal.signal(signal.SIGTERM, shutdown_handler)
signal.signal(signal.SIGINT, shutdown_handler)

# --- Initial Device Phase ---
def set_initial_phase():
    ed = get_edgedevice()
    if not ed:
        update_edgedevice_phase(PHASE_UNKNOWN)
    else:
        update_edgedevice_phase(PHASE_PENDING)

set_initial_phase()

# --- Main App ---
if __name__ == '__main__':
    app.run(host='0.0.0.0', port=int(os.environ.get('SHIFU_SERVICE_PORT', '8080')))