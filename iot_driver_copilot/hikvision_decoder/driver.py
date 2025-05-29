import os
import sys
import json
import time
import signal
import logging
import threading
from typing import Dict, Any, Optional
import yaml

from flask import Flask, request, jsonify, make_response
from flask_cors import CORS

from kubernetes import client as k8s_client
from kubernetes import config as k8s_config
from kubernetes.client.rest import ApiException

# ======================= Logging Configuration =======================
LOG_LEVEL = os.environ.get('LOG_LEVEL', 'INFO').upper()
logging.basicConfig(
    level=LOG_LEVEL,
    format='%(asctime)s - %(levelname)s - %(threadName)s - %(message)s'
)
logger = logging.getLogger("DeviceShifuDecoder")

# ======================= ShifuClient Class =======================
class ShifuClient:
    def __init__(self):
        self.device_name = os.environ.get('EDGEDEVICE_NAME', 'deviceshifu-decoder')
        self.namespace = os.environ.get('EDGEDEVICE_NAMESPACE', 'devices')
        self.config_mount_path = os.environ.get('CONFIG_MOUNT_PATH', '/etc/edgedevice/config')

        self.instructions_file = os.path.join(self.config_mount_path, 'instructions')
        self.driver_properties_file = os.path.join(self.config_mount_path, 'driverProperties')

        self.api = None
        self.crd_api = None
        self._init_k8s_client()

    def _init_k8s_client(self):
        try:
            k8s_config.load_incluster_config()
            logger.info("Loaded in-cluster Kubernetes config.")
        except Exception:
            try:
                k8s_config.load_kube_config()
                logger.info("Loaded kubeconfig from default location.")
            except Exception as e:
                logger.warning("Failed to load Kubernetes config: %s", str(e))
                self.api = None
                self.crd_api = None
                return

        self.api = k8s_client.CoreV1Api()
        self.crd_api = k8s_client.CustomObjectsApi()

    def read_mounted_config_file(self, file_path: str) -> Optional[Dict[str, Any]]:
        try:
            with open(file_path, 'r') as f:
                content = f.read()
                if content.strip() == "":
                    return None
                return yaml.safe_load(content)
        except Exception as e:
            logger.error(f"Error reading config file {file_path}: {e}")
            return None

    def get_instruction_config(self) -> Optional[Dict[str, Any]]:
        return self.read_mounted_config_file(self.instructions_file)

    def get_driver_properties(self) -> Optional[Dict[str, Any]]:
        return self.read_mounted_config_file(self.driver_properties_file)

    def get_edge_device(self) -> Optional[Dict[str, Any]]:
        if not self.crd_api:
            logger.warning("Kubernetes CRD API client not initialized.")
            return None
        try:
            return self.crd_api.get_namespaced_custom_object(
                group="deviceshifu.siemens.com",
                version="v1alpha1",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.device_name
            )
        except ApiException as e:
            logger.error(f"Failed to get EdgeDevice resource: {e}")
            return None

    def get_device_address(self) -> Optional[str]:
        edgedevice = self.get_edge_device()
        if edgedevice:
            try:
                address = edgedevice.get('spec', {}).get('address', None)
                if address:
                    logger.info("Device address obtained from EdgeDevice spec: %s", address)
                return address
            except Exception as e:
                logger.error(f"Error getting device address from EdgeDevice: {e}")
                return None
        return None

    def update_device_status(self, status: str, reason: str = "", message: str = "") -> None:
        if not self.crd_api:
            logger.warning("Kubernetes CRD API client not initialized.")
            return
        try:
            body = {
                "status": {
                    "connection": {
                        "status": status,
                        "reason": reason,
                        "message": message,
                        "timestamp": int(time.time())
                    }
                }
            }
            self.crd_api.patch_namespaced_custom_object_status(
                group="deviceshifu.siemens.com",
                version="v1alpha1",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.device_name,
                body=body
            )
            logger.info("Updated EdgeDevice connection status: %s", status)
        except Exception as e:
            logger.error(f"Failed to update EdgeDevice status: {e}")

# ======================= Device Communication Abstraction =======================
import requests

class HikvisionDecoderConnector:
    """
    Handles communication with the Hikvision Decoder device via HTTP.
    """

    def __init__(self, address: str, port: Optional[str], username: Optional[str], password: Optional[str]):
        self.address = address
        self.port = port
        self.username = username
        self.password = password
        self.session = requests.Session()
        self.connected = False
        self.base_url = self._build_base_url()

    def _build_base_url(self) -> str:
        if self.port:
            return f"http://{self.address}:{self.port}"
        return f"http://{self.address}"

    def connect(self) -> bool:
        # Example: Test connection by a GET to a known endpoint (customize as needed)
        try:
            url = f"{self.base_url}/"
            resp = self.session.get(url, timeout=2)
            if resp.status_code in [200, 401, 403]:
                self.connected = True
                logger.info("Connected to Hikvision Decoder at %s", self.base_url)
                return True
            else:
                logger.warning("Unexpected status from device: %s", resp.status_code)
                self.connected = False
                return False
        except Exception as e:
            logger.error("Failed to connect to device: %s", str(e))
            self.connected = False
            return False

    def disconnect(self):
        self.session.close()
        self.connected = False

    def post(self, path: str, payload: Dict[str, Any]) -> requests.Response:
        url = f"{self.base_url}/{path.lstrip('/')}"
        logger.debug("POST %s payload=%s", url, payload)
        headers = {'Content-Type': 'application/json'}
        auth = (self.username, self.password) if self.username and self.password else None
        return self.session.post(url, json=payload, headers=headers, auth=auth, timeout=5)

    def get(self, path: str, params: Optional[Dict[str, Any]] = None) -> requests.Response:
        url = f"{self.base_url}/{path.lstrip('/')}"
        logger.debug("GET %s params=%s", url, params)
        auth = (self.username, self.password) if self.username and self.password else None
        return self.session.get(url, params=params, auth=auth, timeout=5)

    def put(self, path: str, payload: Dict[str, Any]) -> requests.Response:
        url = f"{self.base_url}/{path.lstrip('/')}"
        logger.debug("PUT %s payload=%s", url, payload)
        headers = {'Content-Type': 'application/json'}
        auth = (self.username, self.password) if self.username and self.password else None
        return self.session.put(url, json=payload, headers=headers, auth=auth, timeout=5)

    def delete(self, path: str, payload: Optional[Dict[str, Any]] = None) -> requests.Response:
        url = f"{self.base_url}/{path.lstrip('/')}"
        logger.debug("DELETE %s payload=%s", url, payload)
        headers = {'Content-Type': 'application/json'}
        auth = (self.username, self.password) if self.username and self.password else None
        return self.session.delete(url, json=payload, headers=headers, auth=auth, timeout=5)

# ======================= DeviceShifuDriver =======================
class DeviceShifuDriver:
    def __init__(self):
        self.shifu_client = ShifuClient()
        self.shutdown_flag = threading.Event()
        self.latest_data: Dict[str, Any] = {}
        self.data_lock = threading.Lock()

        self.device_address = (
            os.environ.get('DEVICE_ADDRESS') or
            self.shifu_client.get_device_address()
        )
        self.device_port = os.environ.get('DEVICE_PORT')
        self.device_username = os.environ.get('DEVICE_USERNAME')
        self.device_password = os.environ.get('DEVICE_PASSWORD')

        self.http_host = os.environ.get('HTTP_HOST', '0.0.0.0')
        self.http_port = int(os.environ.get('HTTP_PORT', 8080))
        self.flask_app = Flask(__name__)
        CORS(self.flask_app)

        self.instruction_config = self._parse_instruction_config()
        self.connector = None

        self.status: Dict[str, Any] = {
            "server": "initializing",
            "device_connection": "unknown",
            "timestamp": int(time.time())
        }

        self._setup_signal_handlers()
        self.setup_routes()

    def _parse_instruction_config(self) -> Dict[str, Any]:
        # Generate instructions dict based on provided API info if instructions file is not present
        instructions = self.shifu_client.get_instruction_config()
        if instructions:
            logger.info("Loaded instructions from ConfigMap.")
            return instructions

        # If not present, build from static API info
        logger.warning("No instructions file found, generating from static API info.")
        # Map driver API info to instruction configs
        # These are the four endpoints as per API info provided
        return {
            "manage": {
                "method": "POST",
                "path": "device/decoder/manage",
                "description": "Device management commands (activation, reboot, shutdown, upgrade, restore config)",
                "content_type": "application/json",
                "parameters": [
                    {"name": "action", "in": "body", "required": True, "type": "string"}
                ]
            },
            "config": {
                "method": "POST",
                "path": "device/decoder/config",
                "description": "Configuration commands (display, window, scene, parameter adjustments)",
                "content_type": "application/json",
                "parameters": [
                    {"name": "config", "in": "body", "required": True, "type": "object"}
                ]
            },
            "status": {
                "method": "GET",
                "path": "device/decoder/status",
                "description": "Get real-time telemetry/status from the decoder",
                "content_type": "application/json",
                "parameters": []
            },
            "decode": {
                "method": "POST",
                "path": "device/decoder/decode",
                "description": "Start/stop dynamic decoding process",
                "content_type": "application/json",
                "parameters": [
                    {"name": "action", "in": "body", "required": True, "type": "string"},
                    {"name": "channels", "in": "body", "required": False, "type": "array"}
                ]
            }
        }

    def _setup_signal_handlers(self):
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

    def setup_routes(self):
        app = self.flask_app

        @app.route('/health', methods=['GET'])
        def health():
            status = {
                "server": "running",
                "device_connection": self.status.get("device_connection", "unknown"),
                "timestamp": int(time.time())
            }
            return make_response(jsonify(status), 200)

        @app.route('/status', methods=['GET'])
        def driver_status():
            # Returns both driver and device status info
            with self.data_lock:
                device_status = self.latest_data.get('status', {})
            response = {
                "driver_status": self.status,
                "device_status": device_status
            }
            return make_response(jsonify(response), 200)

        # Dynamic instruction routes
        for instruction, config in self.instruction_config.items():
            endpoint = f"/{instruction}"
            method = config.get("method", "GET").upper()

            if method == "GET":
                app.add_url_rule(endpoint, endpoint, self._make_get_handler(instruction, config), methods=['GET'])
            elif method == "POST":
                app.add_url_rule(endpoint, endpoint, self._make_post_handler(instruction, config), methods=['POST'])
            elif method == "PUT":
                app.add_url_rule(endpoint, endpoint, self._make_put_handler(instruction, config), methods=['PUT'])
            elif method == "DELETE":
                app.add_url_rule(endpoint, endpoint, self._make_delete_handler(instruction, config), methods=['DELETE'])

    # Handler Factories
    def _make_get_handler(self, instruction: str, config: Dict[str, Any]):
        def handler():
            try:
                with self.data_lock:
                    cached = self.latest_data.get(instruction)
                if cached is not None:
                    logger.info(f"Serving cached data for {instruction}")
                    return make_response(jsonify(cached), 200)

                # Fetch fresh data from device
                result, code = self.handle_device_request(instruction, config, None, method='GET')
                if code == 200:
                    with self.data_lock:
                        self.latest_data[instruction] = result
                return make_response(jsonify(result), code)
            except Exception as e:
                logger.error(f"GET {instruction} error: {str(e)}")
                return make_response(jsonify({"error": str(e)}), 500)
        handler.__name__ = f"get_{instruction}_handler"
        return handler

    def _make_post_handler(self, instruction: str, config: Dict[str, Any]):
        def handler():
            if not request.is_json:
                logger.warning(f"POST {instruction}: Invalid content type.")
                return make_response(jsonify({"error": "Content-Type must be application/json"}), 400)
            try:
                payload = request.get_json()
                # Validate required parameters
                for param in config.get("parameters", []):
                    if param["required"] and param["name"] not in payload:
                        return make_response(jsonify({"error": f"Missing required parameter: {param['name']}"}), 400)
                result, code = self.handle_device_request(instruction, config, payload, method='POST')
                if code == 200:
                    with self.data_lock:
                        self.latest_data[instruction] = result
                return make_response(jsonify(result), code)
            except Exception as e:
                logger.error(f"POST {instruction} error: {str(e)}")
                return make_response(jsonify({"error": str(e)}), 500)
        handler.__name__ = f"post_{instruction}_handler"
        return handler

    def _make_put_handler(self, instruction: str, config: Dict[str, Any]):
        def handler():
            if not request.is_json:
                return make_response(jsonify({"error": "Content-Type must be application/json"}), 400)
            try:
                payload = request.get_json()
                result, code = self.handle_device_request(instruction, config, payload, method='PUT')
                if code == 200:
                    with self.data_lock:
                        self.latest_data[instruction] = result
                return make_response(jsonify(result), code)
            except Exception as e:
                logger.error(f"PUT {instruction} error: {str(e)}")
                return make_response(jsonify({"error": str(e)}), 500)
        handler.__name__ = f"put_{instruction}_handler"
        return handler

    def _make_delete_handler(self, instruction: str, config: Dict[str, Any]):
        def handler():
            try:
                payload = request.get_json(silent=True)
                result, code = self.handle_device_request(instruction, config, payload, method='DELETE')
                if code == 200:
                    with self.data_lock:
                        self.latest_data[instruction] = result
                return make_response(jsonify(result), code)
            except Exception as e:
                logger.error(f"DELETE {instruction} error: {str(e)}")
                return make_response(jsonify({"error": str(e)}), 500)
        handler.__name__ = f"delete_{instruction}_handler"
        return handler

    def connect_device(self):
        # Establish connection to the decoder
        if not self.device_address:
            logger.error("DEVICE_ADDRESS is not set or resolvable from EdgeDevice spec.")
            self.status["device_connection"] = "unavailable"
            self.shifu_client.update_device_status("unavailable", reason="No device address")
            return False
        try:
            self.connector = HikvisionDecoderConnector(
                address=self.device_address,
                port=self.device_port,
                username=self.device_username,
                password=self.device_password
            )
            if self.connector.connect():
                self.status["device_connection"] = "connected"
                self.shifu_client.update_device_status("connected", reason="Connected to device")
                return True
            else:
                self.status["device_connection"] = "unavailable"
                self.shifu_client.update_device_status("unavailable", reason="Device not responding")
                return False
        except Exception as e:
            logger.error("Error connecting to device: %s", e)
            self.status["device_connection"] = "unavailable"
            self.shifu_client.update_device_status("unavailable", reason=str(e))
            return False

    def handle_device_request(self, instruction: str, config: Dict[str, Any], payload: Optional[Dict[str, Any]], method: str) -> (Dict[str, Any], int):
        """
        Handles the actual device communication per instruction.
        Returns (result, http_code)
        """
        if not self.connector or not self.connector.connected:
            logger.warning("Device not connected, attempting to reconnect...")
            if not self.connect_device():
                return {"error": "Device not connected"}, 503

        try:
            device_path = config.get("path")
            if instruction == "manage":
                if method == "POST":
                    resp = self.connector.post(device_path, payload)
                    if resp.status_code == 200:
                        return resp.json(), 200
                    return {"error": resp.text}, resp.status_code
            elif instruction == "config":
                if method == "POST":
                    resp = self.connector.post(device_path, payload)
                    if resp.status_code == 200:
                        return resp.json(), 200
                    return {"error": resp.text}, resp.status_code
            elif instruction == "status":
                if method == "GET":
                    resp = self.connector.get(device_path)
                    if resp.status_code == 200:
                        return resp.json(), 200
                    return {"error": resp.text}, resp.status_code
            elif instruction == "decode":
                if method == "POST":
                    resp = self.connector.post(device_path, payload)
                    if resp.status_code == 200:
                        return resp.json(), 200
                    return {"error": resp.text}, resp.status_code
            else:
                logger.warning(f"Unknown instruction {instruction}")
                return {"error": "Unknown instruction"}, 404

        except requests.exceptions.RequestException as e:
            logger.error(f"Device HTTP error for {instruction}: {e}")
            self.status["device_connection"] = "error"
            self.shifu_client.update_device_status("error", reason=str(e))
            return {"error": "Device communication error: " + str(e)}, 504
        except Exception as e:
            logger.error(f"Device handler error for {instruction}: {e}")
            self.status["device_connection"] = "error"
            self.shifu_client.update_device_status("error", reason=str(e))
            return {"error": str(e)}, 500

        return {"error": "Unhandled error"}, 500

    def background_status_refresh(self, interval: int = 10):
        """
        Periodically refreshes device status and caches it.
        """
        logger.info("Background status refresh thread started.")
        while not self.shutdown_flag.is_set():
            try:
                config = self.instruction_config.get("status")
                if config:
                    result, code = self.handle_device_request("status", config, None, method="GET")
                    if code == 200:
                        with self.data_lock:
                            self.latest_data["status"] = result
                        self.status["device_connection"] = "connected"
                        self.shifu_client.update_device_status("connected")
                    else:
                        self.status["device_connection"] = "unavailable"
                        self.shifu_client.update_device_status("unavailable", reason=result.get("error", "Status unavailable"))
            except Exception as e:
                logger.error(f"Background refresh error: {e}")
                self.status["device_connection"] = "error"
                self.shifu_client.update_device_status("error", reason=str(e))
            time.sleep(interval)
        logger.info("Background status refresh thread stopped.")

    def signal_handler(self, signum, frame):
        logger.info(f"Received signal {signum}, shutting down gracefully...")
        self.shutdown()

    def shutdown(self):
        self.shutdown_flag.set()
        logger.info("Shutting down HTTP server and cleaning resources.")
        try:
            if self.connector:
                self.connector.disconnect()
            self.status["server"] = "stopped"
            self.status["device_connection"] = "disconnected"
            self.shifu_client.update_device_status("disconnected", reason="Server shutdown")
        except Exception as e:
            logger.error(f"Exception during shutdown: {e}")
        func = request.environ.get('werkzeug.server.shutdown')
        if func:
            func()
        else:
            logger.info("Flask shutdown function not found; exiting process.")
            os._exit(0)

    def connection_monitor(self, interval: int = 5):
        """
        Monitors device connection and updates status.
        """
        logger.info("Connection monitor thread started.")
        while not self.shutdown_flag.is_set():
            try:
                if self.connector:
                    if not self.connector.connect():
                        self.status["device_connection"] = "unavailable"
                        self.shifu_client.update_device_status("unavailable", reason="Device unreachable")
                    else:
                        self.status["device_connection"] = "connected"
                        self.shifu_client.update_device_status("connected")
            except Exception as e:
                logger.error(f"Connection monitor error: {e}")
                self.status["device_connection"] = "error"
                self.shifu_client.update_device_status("error", reason=str(e))
            time.sleep(interval)
        logger.info("Connection monitor thread stopped.")

    def run(self):
        # Connect to device first
        self.connect_device()

        # Start background status refresh
        refresh_thread = threading.Thread(target=self.background_status_refresh, args=(10,), daemon=True, name="StatusRefreshThread")
        refresh_thread.start()

        # Start connection monitor
        monitor_thread = threading.Thread(target=self.connection_monitor, args=(5,), daemon=True, name="ConnectionMonitorThread")
        monitor_thread.start()

        try:
            logger.info(f"Starting Flask HTTP server at {self.http_host}:{self.http_port}")
            self.status["server"] = "running"
            self.flask_app.run(host=self.http_host, port=self.http_port, threaded=True)
        except Exception as e:
            logger.error(f"HTTP server error: {e}")
        finally:
            self.shutdown_flag.set()
            refresh_thread.join(2)
            monitor_thread.join(2)
            logger.info("DeviceShifu HTTP driver terminated.")

# ======================= Entrypoint =======================
if __name__ == "__main__":
    driver = DeviceShifuDriver()
    driver.run()