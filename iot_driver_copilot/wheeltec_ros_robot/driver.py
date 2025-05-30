import os
import asyncio
import yaml
import json
from fastapi import FastAPI, Request, Response, HTTPException, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from kubernetes import config, client
from kubernetes.client.rest import ApiException
import uvicorn

import rclpy
from rclpy.node import Node
from std_msgs.msg import Header
from nav_msgs.msg import Odometry
from sensor_msgs.msg import Imu, LaserScan
from geometry_msgs.msg import Twist, PoseStamped

import threading
from typing import Optional, Dict, Any

# --- ROS 2 Communication Layer ---

class ROS2Bridge(Node):
    def __init__(self, name='shifu_ros2_bridge'):
        super().__init__(name)
        self.odom_msg = None
        self.imu_msg = None
        self.scan_msg = None
        self.odom_event = threading.Event()
        self.imu_event = threading.Event()
        self.scan_event = threading.Event()

        self.create_subscription(Odometry, '/odom', self.odom_callback, 10)
        self.create_subscription(Imu, '/imu', self.imu_callback, 10)
        self.create_subscription(LaserScan, '/scan', self.scan_callback, 10)
        self.cmd_vel_publisher = self.create_publisher(Twist, '/cmd_vel', 10)
        self.nav_goal_publisher = self.create_publisher(PoseStamped, '/goal_pose', 10)

    def odom_callback(self, msg):
        self.odom_msg = msg
        self.odom_event.set()

    def imu_callback(self, msg):
        self.imu_msg = msg
        self.imu_event.set()

    def scan_callback(self, msg):
        self.scan_msg = msg
        self.scan_event.set()

    def get_odom(self, timeout=2.0):
        self.odom_event.clear()
        if self.odom_msg is not None:
            return self.odom_msg
        if self.odom_event.wait(timeout):
            return self.odom_msg
        return None

    def get_imu(self, timeout=2.0):
        self.imu_event.clear()
        if self.imu_msg is not None:
            return self.imu_msg
        if self.imu_event.wait(timeout):
            return self.imu_msg
        return None

    def get_scan(self, timeout=2.0):
        self.scan_event.clear()
        if self.scan_msg is not None:
            return self.scan_msg
        if self.scan_event.wait(timeout):
            return self.scan_msg
        return None

    def send_cmd_vel(self, data: dict):
        twist = Twist()
        twist.linear.x = data.get('linear', {}).get('x', 0.0)
        twist.linear.y = data.get('linear', {}).get('y', 0.0)
        twist.linear.z = data.get('linear', {}).get('z', 0.0)
        twist.angular.x = data.get('angular', {}).get('x', 0.0)
        twist.angular.y = data.get('angular', {}).get('y', 0.0)
        twist.angular.z = data.get('angular', {}).get('z', 0.0)
        self.cmd_vel_publisher.publish(twist)

    def send_nav_goal(self, data: dict):
        pose = PoseStamped()
        pose.header = Header()
        pose.header.stamp = self.get_clock().now().to_msg()
        pose.header.frame_id = data.get('frame_id', 'map')
        pose.pose.position.x = data['position']['x']
        pose.pose.position.y = data['position']['y']
        pose.pose.position.z = data['position'].get('z', 0.0)
        pose.pose.orientation.x = data['orientation'].get('x', 0.0)
        pose.pose.orientation.y = data['orientation'].get('y', 0.0)
        pose.pose.orientation.z = data['orientation'].get('z', 0.0)
        pose.pose.orientation.w = data['orientation']['w']
        self.nav_goal_publisher.publish(pose)

# --- Utility Functions ---

def ros_odom_to_dict(msg: Odometry) -> dict:
    if msg is None:
        return {}
    return {
        "header": {
            "stamp": {"sec": msg.header.stamp.sec, "nanosec": msg.header.stamp.nanosec},
            "frame_id": msg.header.frame_id
        },
        "child_frame_id": msg.child_frame_id,
        "pose": {
            "pose": {
                "position": {
                    "x": msg.pose.pose.position.x,
                    "y": msg.pose.pose.position.y,
                    "z": msg.pose.pose.position.z
                },
                "orientation": {
                    "x": msg.pose.pose.orientation.x,
                    "y": msg.pose.pose.orientation.y,
                    "z": msg.pose.pose.orientation.z,
                    "w": msg.pose.pose.orientation.w
                }
            },
            "covariance": list(msg.pose.covariance)
        },
        "twist": {
            "twist": {
                "linear": {
                    "x": msg.twist.twist.linear.x,
                    "y": msg.twist.twist.linear.y,
                    "z": msg.twist.twist.linear.z
                },
                "angular": {
                    "x": msg.twist.twist.angular.x,
                    "y": msg.twist.twist.angular.y,
                    "z": msg.twist.twist.angular.z
                }
            },
            "covariance": list(msg.twist.covariance)
        }
    }

def ros_imu_to_dict(msg: Imu) -> dict:
    if msg is None:
        return {}
    return {
        "header": {
            "stamp": {"sec": msg.header.stamp.sec, "nanosec": msg.header.stamp.nanosec},
            "frame_id": msg.header.frame_id
        },
        "orientation": {
            "x": msg.orientation.x,
            "y": msg.orientation.y,
            "z": msg.orientation.z,
            "w": msg.orientation.w
        },
        "orientation_covariance": list(msg.orientation_covariance),
        "angular_velocity": {
            "x": msg.angular_velocity.x,
            "y": msg.angular_velocity.y,
            "z": msg.angular_velocity.z
        },
        "angular_velocity_covariance": list(msg.angular_velocity_covariance),
        "linear_acceleration": {
            "x": msg.linear_acceleration.x,
            "y": msg.linear_acceleration.y,
            "z": msg.linear_acceleration.z
        },
        "linear_acceleration_covariance": list(msg.linear_acceleration_covariance)
    }

def ros_scan_to_dict(msg: LaserScan) -> dict:
    if msg is None:
        return {}
    return {
        "header": {
            "stamp": {"sec": msg.header.stamp.sec, "nanosec": msg.header.stamp.nanosec},
            "frame_id": msg.header.frame_id
        },
        "angle_min": msg.angle_min,
        "angle_max": msg.angle_max,
        "angle_increment": msg.angle_increment,
        "time_increment": msg.time_increment,
        "scan_time": msg.scan_time,
        "range_min": msg.range_min,
        "range_max": msg.range_max,
        "ranges": list(msg.ranges),
        "intensities": list(msg.intensities)
    }

# --- Kubernetes CRD Updater ---

class EdgeDeviceStatusUpdater:
    def __init__(self, name: str, namespace: str):
        try:
            config.load_incluster_config()
        except Exception:
            config.load_kube_config()
        self.api = client.CustomObjectsApi()
        self.name = name
        self.namespace = namespace

    def update_phase(self, phase: str):
        try:
            body = {"status": {"edgeDevicePhase": phase}}
            self.api.patch_namespaced_custom_object_status(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.name,
                body=body
            )
        except ApiException as e:
            pass

    def get_edgedevice(self) -> Optional[Dict[str, Any]]:
        try:
            return self.api.get_namespaced_custom_object(
                group="shifu.edgenesis.io",
                version="v1alpha1",
                namespace=self.namespace,
                plural="edgedevices",
                name=self.name
            )
        except ApiException:
            return None

# --- ConfigMap Loader ---

def load_instructions_config(path: str) -> dict:
    if not os.path.exists(path):
        return {}
    with open(path, 'r') as f:
        return yaml.safe_load(f)

# --- FastAPI App Definition ---

app = FastAPI()

# --- Environment Variables ---

EDGEDEVICE_NAME = os.environ.get("EDGEDEVICE_NAME")
EDGEDEVICE_NAMESPACE = os.environ.get("EDGEDEVICE_NAMESPACE")
HOST = os.environ.get("SERVER_HOST", "0.0.0.0")
PORT = int(os.environ.get("SERVER_PORT", "8080"))
ROS_DOMAIN_ID = os.environ.get("ROS_DOMAIN_ID", "0")
CONFIG_PATH = "/etc/edgedevice/config/instructions"

# --- Startup: ROS Bridge Thread ---

ros_bridge_ready = threading.Event()
ros_bridge = None

def ros2_spin_thread():
    global ros_bridge
    try:
        rclpy.init(args=None)
        ros_bridge = ROS2Bridge()
        ros_bridge_ready.set()
        rclpy.spin(ros_bridge)
    except Exception:
        ros_bridge_ready.set()
    finally:
        rclpy.shutdown()

ros_thread = threading.Thread(target=ros2_spin_thread, daemon=True)
ros_thread.start()

# --- Startup: Kubernetes Updater ---

k8s_updater = None
edgedevice_spec = {}
device_address = None

def k8s_init():
    global k8s_updater, edgedevice_spec, device_address
    if not EDGEDEVICE_NAME or not EDGEDEVICE_NAMESPACE:
        raise RuntimeError("EDGEDEVICE_NAME or EDGEDEVICE_NAMESPACE env not set")
    k8s_updater = EdgeDeviceStatusUpdater(EDGEDEVICE_NAME, EDGEDEVICE_NAMESPACE)
    dev = k8s_updater.get_edgedevice()
    if dev is None:
        k8s_updater.update_phase("Unknown")
        raise RuntimeError("EdgeDevice resource not found")
    edgedevice_spec = dev.get("spec", {})
    device_address = edgedevice_spec.get("address")
    k8s_updater.update_phase("Pending")
    return dev

@app.on_event("startup")
async def startup_event():
    # Wait for ROS bridge
    await asyncio.get_event_loop().run_in_executor(None, ros_bridge_ready.wait)
    # K8s
    try:
        k8s_init()
    except Exception:
        pass
    # If ROS node is alive, set Running
    if ros_bridge_ready.is_set() and ros_bridge is not None:
        try:
            k8s_updater.update_phase("Running")
        except Exception:
            pass
    else:
        try:
            k8s_updater.update_phase("Failed")
        except Exception:
            pass

@app.on_event("shutdown")
async def shutdown_event():
    try:
        k8s_updater.update_phase("Unknown")
    except:
        pass
    try:
        rclpy.shutdown()
    except:
        pass

# --- ConfigMap API Settings Loader ---

api_settings = load_instructions_config(CONFIG_PATH) if os.path.exists(CONFIG_PATH) else {}

def get_api_settings(api_name: str) -> dict:
    return api_settings.get(api_name, {}).get('protocolPropertyList', {})

# --- API Schemas ---

class NavGoalRequest(BaseModel):
    position: dict
    orientation: dict
    frame_id: Optional[str] = 'map'

class CmdVelRequest(BaseModel):
    linear: dict
    angular: dict

# --- HTTP API Endpoints ---

@app.get("/sensors/odom")
async def get_odom():
    if ros_bridge is None:
        raise HTTPException(status_code=503, detail="ROS node unavailable")
    msg = ros_bridge.get_odom()
    if msg is None:
        raise HTTPException(status_code=504, detail="Timeout waiting for odometry")
    return JSONResponse(content=ros_odom_to_dict(msg), headers={"Content-Type": "application/json"})

@app.get("/sensors/imu")
async def get_imu():
    if ros_bridge is None:
        raise HTTPException(status_code=503, detail="ROS node unavailable")
    msg = ros_bridge.get_imu()
    if msg is None:
        raise HTTPException(status_code=504, detail="Timeout waiting for imu")
    return JSONResponse(content=ros_imu_to_dict(msg), headers={"Content-Type": "application/json"})

@app.get("/sensors/scan")
async def get_scan():
    if ros_bridge is None:
        raise HTTPException(status_code=503, detail="ROS node unavailable")
    msg = ros_bridge.get_scan()
    if msg is None:
        raise HTTPException(status_code=504, detail="Timeout waiting for scan")
    return JSONResponse(content=ros_scan_to_dict(msg), headers={"Content-Type": "application/json"})

@app.post("/commands/nav")
async def post_nav_goal(request: Request):
    if ros_bridge is None:
        raise HTTPException(status_code=503, detail="ROS node unavailable")
    try:
        data = await request.json()
        if isinstance(data, list):  # Multi-goal
            for goal in data:
                ros_bridge.send_nav_goal(goal)
        else:
            ros_bridge.send_nav_goal(data)
        return Response(status_code=204)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid nav goal: {str(e)}")

@app.post("/commands/vel")
async def post_cmd_vel(request: Request):
    if ros_bridge is None:
        raise HTTPException(status_code=503, detail="ROS node unavailable")
    try:
        data = await request.json()
        ros_bridge.send_cmd_vel(data)
        return Response(status_code=204)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid velocity command: {str(e)}")

# --- Run Server ---

if __name__ == "__main__":
    uvicorn.run(app, host=HOST, port=PORT)