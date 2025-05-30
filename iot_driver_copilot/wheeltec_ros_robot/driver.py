import os
import yaml
import asyncio
import json
import logging
import signal
from typing import Dict, Any, Optional

from fastapi import FastAPI, Request, HTTPException, Body, Response, status
from fastapi.responses import StreamingResponse, JSONResponse
import uvicorn

from kubernetes import client as k8s_client, config as k8s_config
from kubernetes.client.rest import ApiException

import paho.mqtt.client as mqtt

# --- Configurations from environment variables ---
EDGEDEVICE_NAME = os.environ["EDGEDEVICE_NAME"]
EDGEDEVICE_NAMESPACE = os.environ["EDGEDEVICE_NAMESPACE"]
MQTT_BROKER_HOST = os.environ.get("MQTT_BROKER_HOST", "localhost")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME", "")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")
MQTT_KEEPALIVE = int(os.environ.get("MQTT_KEEPALIVE", "60"))
MQTT_PREFIX = os.environ.get("MQTT_PREFIX", "deviceshifu-car-mqtt")

SERVER_HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("SERVER_PORT", "8080"))

INSTRUCTIONS_PATH = "/etc/edgedevice/config/instructions"

# --- Logging ---
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("deviceshifu")

# --- Load API instructions from YAML ---
def load_api_instructions(path: str) -> Dict[str, Any]:
    try:
        with open(path, 'r') as f:
            return yaml.safe_load(f) or {}
    except Exception as e:
        logger.error(f"Failed to load API instructions: {e}")
        return {}

api_instructions = load_api_instructions(INSTRUCTIONS_PATH)

# --- Kubernetes Client Setup ---
def get_k8s_api():
    try:
        k8s_config.load_incluster_config()
    except Exception as e:
        logger.error(f"Failed to load in-cluster K8s config: {e}")
        raise
    return k8s_client.CustomObjectsApi()

def get_edgedevice_resource(api: k8s_client.CustomObjectsApi):
    try:
        ed = api.get_namespaced_custom_object(
            group="shifu.edgenesis.io",
            version="v1alpha1",
            namespace=EDGEDEVICE_NAMESPACE,
            plural="edgedevices",
            name=EDGEDEVICE_NAME
        )
        return ed
    except ApiException as e:
        logger.error(f"Error retrieving EdgeDevice CRD: {e}")
        return None

def update_edgedevice_phase(api: k8s_client.CustomObjectsApi, phase: str):
    body = {"status": {"edgeDevicePhase": phase}}
    try:
        api.patch_namespaced_custom_object_status(
            group="shifu.edgenesis.io",
            version="v1alpha1",
            namespace=EDGEDEVICE_NAMESPACE,
            plural="edgedevices",
            name=EDGEDEVICE_NAME,
            body=body
        )
    except ApiException as e:
        logger.error(f"Failed to update EdgeDevice phase: {e}")

# --- MQTT Client Handling ---
class MQTTClientManager:
    def __init__(self, prefix: str):
        self.prefix = prefix
        self.data_cache = {}
        self.connected = False
        self.loop = asyncio.get_event_loop()
        self.sub_topics = [
            "odom", "scan", "joint_states", "point_cloud", "status"
        ]
        self.client = mqtt.Client()
        if MQTT_USERNAME and MQTT_PASSWORD:
            self.client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_disconnect = self.on_disconnect
        self._connect_future: Optional[asyncio.Future] = None

    def _topic(self, suffix: str) -> str:
        return f"{self.prefix}/{suffix}"

    def on_connect(self, client, userdata, flags, rc):
        logger.info(f"MQTT: Connected with result code {rc}")
        if rc == 0:
            self.connected = True
            for topic in self.sub_topics:
                client.subscribe(self._topic(topic))
            logger.info("MQTT: Subscribed to topics.")
            if self._connect_future and not self._connect_future.done():
                self.loop.call_soon_threadsafe(self._connect_future.set_result, True)
        else:
            self.connected = False
            if self._connect_future and not self._connect_future.done():
                self.loop.call_soon_threadsafe(self._connect_future.set_result, False)

    def on_disconnect(self, client, userdata, rc):
        logger.warning("MQTT: Disconnected.")
        self.connected = False

    def on_message(self, client, userdata, msg):
        suffix = msg.topic.replace(f"{self.prefix}/", "")
        # Try to decode JSON, fallback to raw text
        try:
            payload = json.loads(msg.payload.decode())
        except Exception:
            payload = msg.payload.decode(errors="replace")
        self.data_cache[suffix] = payload

    def start(self):
        self.client.connect_async(MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_KEEPALIVE)
        self.client.loop_start()

    async def ensure_connected(self, retries=5, delay=2):
        for _ in range(retries):
            if self.connected:
                return True
            self._connect_future = asyncio.Future()
            self.client.reconnect()
            try:
                await asyncio.wait_for(self._connect_future, timeout=10)
                if self.connected:
                    return True
            except asyncio.TimeoutError:
                pass
            await asyncio.sleep(delay)
        return False

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()

    def get_data(self, key: str):
        return self.data_cache.get(key)

    def publish_command(self, command: str, payload: Optional[dict] = None):
        topic = self._topic(command)
        payload_str = json.dumps(payload) if payload else ""
        logger.info(f"Publishing to {topic}: {payload_str}")
        self.client.publish(topic, payload=payload_str)

# --- Initialize MQTT Client Manager ---
mqtt_manager = MQTTClientManager(MQTT_PREFIX)
mqtt_manager.start()

# --- FastAPI App ---
app = FastAPI()

# --- Helper: Kubernetes Phase Update ---
@app.on_event("startup")
async def startup_event():
    app.state.k8s_api = get_k8s_api()
    app.state.phase_task = asyncio.create_task(phase_heartbeat())

@app.on_event("shutdown")
async def shutdown_event():
    mqtt_manager.stop()
    if hasattr(app.state, "phase_task"):
        app.state.phase_task.cancel()

async def phase_heartbeat():
    api = app.state.k8s_api
    last_phase = None
    while True:
        try:
            # Check MQTT connection status for phase
            if mqtt_manager.connected:
                phase = "Running"
            else:
                phase = "Failed"
            if phase != last_phase:
                update_edgedevice_phase(api, phase)
                last_phase = phase
        except Exception as e:
            logger.error(f"Error updating device phase: {e}")
        await asyncio.sleep(5)

# --- HTTP API Endpoints ---

@app.get("/odom")
async def get_odom():
    data = mqtt_manager.get_data("odom")
    if data is None:
        raise HTTPException(status_code=404, detail="No odometry data yet.")
    return JSONResponse(content=data)

@app.get("/scan")
async def get_scan():
    data = mqtt_manager.get_data("scan")
    if data is None:
        raise HTTPException(status_code=404, detail="No scan data yet.")
    return JSONResponse(content=data)

@app.get("/joint_states")
async def get_joint_states():
    data = mqtt_manager.get_data("joint_states")
    if data is None:
        raise HTTPException(status_code=404, detail="No joint_states data yet.")
    return JSONResponse(content=data)

@app.get("/point_cloud")
async def get_point_cloud():
    data = mqtt_manager.get_data("point_cloud")
    if data is None:
        raise HTTPException(status_code=404, detail="No point_cloud data yet.")
    return JSONResponse(content=data)

@app.get("/status")
async def get_status():
    data = mqtt_manager.get_data("status")
    if data is None:
        raise HTTPException(status_code=404, detail="No status data yet.")
    return JSONResponse(content=data)

@app.post("/move/forward")
async def move_forward(request: Request):
    try:
        payload = await request.json()
    except Exception:
        payload = None
    mqtt_manager.publish_command("move/forward", payload)
    return {"result": "forward command sent"}

@app.post("/move/backward")
async def move_backward(request: Request):
    try:
        payload = await request.json()
    except Exception:
        payload = None
    mqtt_manager.publish_command("move/backward", payload)
    return {"result": "backward command sent"}

@app.post("/turn/left")
async def turn_left(request: Request):
    try:
        payload = await request.json()
    except Exception:
        payload = None
    mqtt_manager.publish_command("turn/left", payload)
    return {"result": "left command sent"}

@app.post("/turn/right")
async def turn_right(request: Request):
    try:
        payload = await request.json()
    except Exception:
        payload = None
    mqtt_manager.publish_command("turn/right", payload)
    return {"result": "right command sent"}

@app.post("/stop")
async def stop_robot():
    mqtt_manager.publish_command("stop")
    return {"result": "stop command sent"}

# --- Healthz endpoint for liveness/readiness probe ---
@app.get("/healthz")
async def healthz():
    return {"status": "ok"}

if __name__ == "__main__":
    uvicorn.run(app, host=SERVER_HOST, port=SERVER_PORT)