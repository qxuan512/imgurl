import os
import json
import time
import yaml
import signal
import sys
import threading
import logging
from typing import Dict, Any, Optional

from flask import Flask, request, jsonify, make_response
from flask_cors import CORS

from kubernetes import client as k8s_client
from kubernetes import config as k8s_config
from kubernetes.client.rest import ApiException

# --------------------------- Logging Setup ---------------------------

LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=LOG_LEVEL,
    format="%(asctime)s %(levelname)s %(threadName)s %(message)s"
)
logger = logging.getLogger("deviceshifu-hikvision-decoder")

# --------------------------- ShifuClient ----------------------------

class ShifuClient:
    def __init__(self):
        self.device_name = os.getenv("EDGEDEVICE_NAME", "deviceshifu-decoder")
        self.namespace = os.getenv("EDGEDEVICE_NAMESPACE", "devices")
        self.config_mount_path = os.getenv("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")
        self.k8s_client = None
        self._init_k8s_client()

    def _init_k8s_client(self) -> None:
        try:
            if os.path.exists('/var/run/secrets/kubernetes.io/serviceaccount/token'):
                k8s_config.load_incluster_config()
                logger.info("Loaded in-cluster Kubernetes config")
            else:
                k8s_config.load_kube_config()
                logger.info("Loaded local kubeconfig")
            self.k8s_client = k8s_client.CustomObjectsApi()
        except Exception as e:
            logger.error(f"Failed to initialize Kubernetes client: {e}")
            self.k8s_client = None

    def get_edge_device(self) -> Optional[Dict[str, Any]]:
        if not self.k8s_client:
            logger.warning("Kubernetes client not initialized")
            return None
        try:
            edge_device = self.k8s_client.get_namespaced_custom_object(
                group="devices.kubeedge.io",
                version="v1alpha2",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.device_name
            )
            logger.debug(f"Fetched EdgeDevice CR: {edge_device}")
            return edge_device
        except ApiException as e:
            logger.error(f"ApiException getting EdgeDevice: {e}")
        except Exception as e:
            logger.error(f"Exception getting EdgeDevice: {e}")
        return None

    def get_device_address(self) -> Optional[str]:
        edge_device = self.get_edge_device()
        if edge_device:
            try:
                address = edge_device.get('spec', {}).get('address')
                logger.info(f"Device address from CR: {address}")
                return address
            except Exception as e:
                logger.error(f"Error extracting address from EdgeDevice CR: {e}")
        return None

    def update_device_status(self, status: Dict[str, Any]) -> None:
        if not self.k8s_client:
            logger.warning("Kubernetes client not initialized")
            return
        try:
            body = {"status": status}
            self.k8s_client.patch_namespaced_custom_object_status(
                group="devices.kubeedge.io",
                version="v1alpha2",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.device_name,
                body=body
            )
            logger.info(f"Updated EdgeDevice status: {status}")
        except ApiException as e:
            logger.error(f"ApiException updating EdgeDevice status: {e}")
        except Exception as e:
            logger.error(f"Exception updating EdgeDevice status: {e}")

    def read_mounted_config_file(self, filename: str) -> Optional[Dict[str, Any]]:
        file_path = os.path.join(self.config_mount_path, filename)
        try:
            with open(file_path, 'r') as f:
                data = yaml.safe_load(f)
                logger.info(f"Loaded config file: {filename}")
                return data
        except Exception as e:
            logger.error(f"Error reading config file {filename}: {e}")
            return None

    def get_instruction_config(self) -> Optional[Dict[str, Any]]:
        return self.read_mounted_config_file("instructions")

# ---------------------- Device Communication Abstraction ----------------------

class HikvisionDecoderHTTPClient:
    """
    Abstracts device HTTP communication for the Hikvision Decoder.
    """
    def __init__(self, address: str, port: Optional[str], username: Optional[str], password: Optional[str]):
        self.address = address
        self.port = port
        self.username = username
        self.password = password
        self.base_url = self._build_base_url()
        self.session = None  # For requests.Session() if needed
        self.connected = False
        self.last_error = None
        # self.session = requests.Session()  # Uncomment if requests is used

    def _build_base_url(self) -> str:
        url = self.address
        if not url.startswith('http://') and not url.startswith('https://'):
            url = f"http://{url}"
        if self.port:
            url = f"{url}:{self.port}"
        return url.rstrip('/')

    def connect(self) -> bool:
        # Simulate connection check: e.g., ping a known status endpoint or try HTTP OPTIONS
        try:
            # Placeholder: In a real SDK, test connectivity, login, etc.
            self.connected = True
            logger.info(f"Connected to Hikvision Decoder at {self.base_url}")
            return True
        except Exception as e:
            self.last_error = str(e)
            logger.error(f"Failed to connect: {e}")
            self.connected = False
            return False

    def disconnect(self):
        self.connected = False
        # self.session.close()  # If using requests.Session()
        logger.info("Disconnected from Hikvision Decoder.")

    def is_connected(self) -> bool:
        return self.connected

    def get_status(self) -> Dict[str, Any]:
        # Simulated status
        return {
            "connected": self.connected,
            "address": self.base_url,
            "last_error": self.last_error
        }

    def device_manage(self, action: str, params: Dict[str, Any]) -> Dict[str, Any]:
        # Simulated management command (activate, reboot, shutdown, upgrade, restore config)
        # In a real implementation, this would send an HTTP POST to the device endpoint.
        logger.info(f"Device manage action: {action}, params: {params}")
        if not self.connected:
            raise Exception("Device not connected")
        # Simulated response
        return {"result": "success", "action": action, "params": params}

    def device_config(self, config_params: Dict[str, Any]) -> Dict[str, Any]:
        # Simulated configuration setting
        logger.info(f"Device config params: {config_params}")
        if not self.connected:
            raise Exception("Device not connected")
        # Simulated response
        return {"result": "success", "config": config_params}

    def device_decode(self, action: str, channels: Any) -> Dict[str, Any]:
        # Simulated decode control
        logger.info(f"Device decode action: {action}, channels: {channels}")
        if not self.connected:
            raise Exception("Device not connected")
        # Simulated response
        return {"result": "success", "action": action, "channels": channels}

    def get_device_status(self) -> Dict[str, Any]:
        # Simulated status payload
        logger.info("Getting device status")
        if not self.connected:
            raise Exception("Device not connected")
        # Example status
        return {
            "decoding_channel_status": "active",
            "device_state": "operational",
            "alarm_conditions": [],
            "sdk_version": "V5.0.0",
            "error_info": None,
            "timestamp": int(time.time())
        }


# --------------------------- Main Driver Class ------------------------------

class DeviceShifuDriver:
    def __init__(self):
        self.shutdown_flag = threading.Event()
        self.shifu_client = ShifuClient()
        self.latest_data: Dict[str, Any] = {}
        self.device_client: Optional[HikvisionDecoderHTTPClient] = None

        # Environment variables
        self.http_host = os.getenv("HTTP_HOST", "0.0.0.0")
        self.http_port = int(os.getenv("HTTP_PORT", "8080"))
        self.device_address = (
            os.getenv("DEVICE_ADDRESS") or
            self.shifu_client.get_device_address() or
            "127.0.0.1"
        )
        self.device_port = os.getenv("DEVICE_PORT")
        self.device_username = os.getenv("DEVICE_USERNAME")
        self.device_password = os.getenv("DEVICE_PASSWORD")

        # Flask app
        self.app = Flask(__name__)
        CORS(self.app)

        # Instruction config
        self.instruction_config = self._parse_instruction_config()

        self._setup_routes()
        self.cache_lock = threading.Lock()
        self._start_background_refresh()

    def _parse_instruction_config(self) -> Dict[str, Any]:
        config = self.shifu_client.get_instruction_config()
        if not config:
            logger.error("No instruction config found in ConfigMap")
            sys.exit(1)
        return config

    def _setup_routes(self) -> None:
        # Map instruction names to device communication methods
        routes_map = {
            "device/decoder/manage": self._handle_manage,
            "device/decoder/config": self._handle_config,
            "device/decoder/decode": self._handle_decode,
            "device/decoder/status": self._handle_status,
        }

        # Dynamic route creation for each instruction
        for instr_name, instr_details in self.instruction_config.items():
            endpoint_path = "/" + instr_name.replace('/', '_')
            method = instr_details.get("method", "GET").upper()
            logger.info(f"Registering endpoint {endpoint_path} [{method}]")

            if instr_name in routes_map:
                handler = routes_map[instr_name]
            else:
                handler = self._handle_not_implemented

            if method in ("GET", "SUBSCRIBE"):
                self.app.add_url_rule(endpoint_path, endpoint_path, self._make_flask_handler(handler, instr_details), methods=["GET"])
            if method in ("POST", "PUT", "PUBLISH"):
                self.app.add_url_rule(endpoint_path, endpoint_path, self._make_flask_handler(handler, instr_details), methods=["POST", "PUT"])

        # Health and status endpoints
        self.app.add_url_rule("/health", "health", self._health_handler, methods=["GET"])
        self.app.add_url_rule("/status", "status", self._status_handler, methods=["GET"])

    def _make_flask_handler(self, handler_func, instr_details):
        def flask_handler():
            try:
                # Validate Content-Type for POST/PUT
                if request.method in ["POST", "PUT"]:
                    if not request.is_json:
                        logger.warning("Request payload is not JSON")
                        return make_response(jsonify({"error": "Content-Type must be application/json"}), 400)
                    payload = request.get_json()
                else:
                    payload = request.args.to_dict()

                response = handler_func(payload, instr_details)
                return make_response(jsonify(response), 200)
            except ValueError as ve:
                logger.error(f"Value error: {ve}")
                return make_response(jsonify({"error": str(ve)}), 400)
            except KeyError as ke:
                logger.error(f"Key error: {ke}")
                return make_response(jsonify({"error": f"Missing key: {ke}"}), 400)
            except Exception as e:
                logger.error(f"Internal error: {e}")
                return make_response(jsonify({"error": str(e)}), 500)
        flask_handler.__name__ = f"handler_{handler_func.__name__}_{instr_details.get('id', '')}"
        return flask_handler

    def _handle_manage(self, payload: Dict[str, Any], instr_details: Dict[str, Any]) -> Dict[str, Any]:
        # POST to /device_decoder_manage
        action = payload.get("action")
        params = payload.get("params", {})
        if not action:
            raise ValueError("Missing 'action' in payload")
        ret = self.device_client.device_manage(action, params)
        self._cache_latest_data("manage", ret)
        return ret

    def _handle_config(self, payload: Dict[str, Any], instr_details: Dict[str, Any]) -> Dict[str, Any]:
        # POST to /device_decoder_config
        config_params = payload.get("config_params") or payload
        ret = self.device_client.device_config(config_params)
        self._cache_latest_data("config", ret)
        return ret

    def _handle_decode(self, payload: Dict[str, Any], instr_details: Dict[str, Any]) -> Dict[str, Any]:
        # POST to /device_decoder_decode
        action = payload.get("action")
        channels = payload.get("channels")
        if not action or not channels:
            raise ValueError("Missing 'action' or 'channels' in payload")
        ret = self.device_client.device_decode(action, channels)
        self._cache_latest_data("decode", ret)
        return ret

    def _handle_status(self, payload: Dict[str, Any], instr_details: Dict[str, Any]) -> Dict[str, Any]:
        # GET to /device_decoder_status
        # Return latest cached device status
        with self.cache_lock:
            if "status" in self.latest_data:
                return self.latest_data["status"]
        # If no cached data, fetch from device
        ret = self.device_client.get_device_status()
        self._cache_latest_data("status", ret)
        return ret

    def _handle_not_implemented(self, payload: Dict[str, Any], instr_details: Dict[str, Any]) -> Dict[str, Any]:
        logger.warning("Instruction not implemented")
        raise NotImplementedError("Instruction not implemented")

    def _cache_latest_data(self, key: str, data: Any):
        with self.cache_lock:
            self.latest_data[key] = data

    def _background_refresh(self):
        refresh_interval = 10  # seconds
        while not self.shutdown_flag.is_set():
            try:
                if self.device_client and self.device_client.is_connected():
                    status = self.device_client.get_device_status()
                    self._cache_latest_data("status", status)
            except Exception as e:
                logger.error(f"Error during background refresh: {e}")
            time.sleep(refresh_interval)

    def _start_background_refresh(self):
        t = threading.Thread(target=self._background_refresh, daemon=True, name="BackgroundRefresh")
        t.start()

    def connect_device(self) -> None:
        self.device_client = HikvisionDecoderHTTPClient(
            address=self.device_address,
            port=self.device_port,
            username=self.device_username,
            password=self.device_password
        )
        connected = self.device_client.connect()
        if not connected:
            logger.error("Failed to connect to device; exiting")
            sys.exit(1)

    def _health_handler(self):
        status = {
            "server": "ok",
            "device_connection": self.device_client.is_connected() if self.device_client else False
        }
        return make_response(jsonify(status), 200)

    def _status_handler(self):
        try:
            device_status = self.device_client.get_status() if self.device_client else {}
            with self.cache_lock:
                cached_status = self.latest_data.get("status")
            return make_response(jsonify({
                "device_status": device_status,
                "cached_status": cached_status
            }), 200)
        except Exception as e:
            logger.error(f"Error in /status: {e}")
            return make_response(jsonify({"error": str(e)}), 500)

    def signal_handler(self, sig, frame):
        logger.info(f"Received signal {sig}, shutting down gracefully.")
        self.shutdown_flag.set()
        self.shutdown()

    def shutdown(self):
        if self.device_client:
            self.device_client.disconnect()
        logger.info("Driver shutdown complete.")
        # Flask will be terminated by main thread

    def run(self):
        self.connect_device()
        # Signal handling
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
        logger.info(f"Starting Flask HTTP server at {self.http_host}:{self.http_port}")
        self.app.run(host=self.http_host, port=self.http_port, threaded=True)


# ------------------------ Entrypoint --------------------------

if __name__ == "__main__":
    driver = DeviceShifuDriver()
    try:
        driver.run()
    except Exception as e:
        logger.error(f"Unhandled exception: {e}")
        driver.shutdown()
        sys.exit(1)