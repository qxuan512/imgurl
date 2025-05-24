import os
import time
import json
import threading
import random
import paho.mqtt.client as mqtt

# Environment Variables (all must be set externally)
MQTT_BROKER = os.environ.get("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME", None)
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", None)
DEVICE_ID = os.environ.get("DEVICE_ID", "envmonitor001")
PUBLISH_INTERVAL_FAST = int(os.environ.get("PUBLISH_INTERVAL_FAST", "30")) # seconds for fast data
PUBLISH_INTERVAL_SLOW = int(os.environ.get("PUBLISH_INTERVAL_SLOW", "60")) # seconds for slow data

# Device state
device_state = {
    "temperature": 22.0,
    "humidity": 50.0,
    "airquality": {"PM2.5": 12, "CO2": 400},
    "battery": 87,
    "status": {"status": "normal", "led": "on"},
    "led_brightness": 75,
    "buzzer": "DISABLE",
    "alarm_threshold": {"PM2.5": 35, "CO2": 800}
}

# ---- MQTT Topic Definitions ----
TOPICS = [
    # Telemetry (publish)
    ("device/sensors/temperature", 0),
    ("device/sensors/humidity", 0),
    ("device/sensors/airquality", 0),
    ("device/sensors/battery", 0),
    ("device/sensors/status", 1),
    # Alarms (publish)
    ("device/alerts/alarm", 2),
    # Config/Commands (subscribe)
    ("device/config/alarm_threshold", 2),
    ("device/config/led", 2),
    ("device/config/buzzer", 2),
    ("device/commands/calibrate", 2),
    ("device/commands/restart", 2),
    ("device/commands/reset", 2)
]

# ---- MQTT Client Setup ----
client = mqtt.Client(client_id=f"{DEVICE_ID}-driver", clean_session=True, protocol=mqtt.MQTTv311)

if MQTT_USERNAME:
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

def publish_json(topic, payload, qos):
    client.publish(topic, json.dumps(payload), qos=qos, retain=False)

def publish_text(topic, payload, qos):
    client.publish(topic, payload, qos=qos, retain=False)

# ---- Device Logic Simulations ----

def simulate_telemetry():
    while True:
        # Temperature, Humidity every 30s (QoS 0)
        temp = round(device_state['temperature'] + random.uniform(-0.5, 0.5), 1)
        hum = round(device_state['humidity'] + random.uniform(-2, 2), 1)
        device_state['temperature'] = temp
        device_state['humidity'] = hum
        publish_json("device/sensors/temperature", {"temperature": temp, "unit": "C"}, qos=0)
        publish_json("device/sensors/humidity", {"humidity": hum, "unit": "%"}, qos=0)
        # Battery every 30s (QoS 0)
        device_state['battery'] = max(0, device_state['battery'] - random.uniform(0, 0.05))
        publish_json("device/sensors/battery", {"battery": int(device_state['battery'])}, qos=0)
        # Status every 30s (QoS 1)
        publish_json("device/sensors/status", device_state['status'], qos=1)
        # Wait for next fast cycle
        time.sleep(PUBLISH_INTERVAL_FAST)

def simulate_airquality():
    while True:
        # Air Quality every 60s (QoS 0)
        pm25 = max(0, round(device_state['airquality']['PM2.5'] + random.uniform(-2, 2), 1))
        co2 = max(300, int(device_state['airquality']['CO2'] + random.uniform(-10, 10)))
        device_state['airquality']['PM2.5'] = pm25
        device_state['airquality']['CO2'] = co2
        publish_json("device/sensors/airquality", {"PM2.5": pm25, "CO2": co2}, qos=0)
        # Check for alarm
        if pm25 > device_state["alarm_threshold"]["PM2.5"]:
            publish_json("device/alerts/alarm", {"alarm": "threshold exceeded", "sensor": "PM2.5", "value": pm25}, qos=2)
            device_state["status"]["status"] = "alarm"
        elif co2 > device_state["alarm_threshold"]["CO2"]:
            publish_json("device/alerts/alarm", {"alarm": "threshold exceeded", "sensor": "CO2", "value": co2}, qos=2)
            device_state["status"]["status"] = "alarm"
        else:
            device_state["status"]["status"] = "normal"
        time.sleep(PUBLISH_INTERVAL_SLOW)

# ---- MQTT Callback Handlers ----

def on_connect(client, userdata, flags, rc):
    for topic, qos in TOPICS:
        if topic.startswith("device/config/") or topic.startswith("device/commands/"):
            client.subscribe(topic, qos=qos)

def on_message(client, userdata, msg):
    topic = msg.topic
    payload_text = msg.payload.decode("utf-8")
    try:
        payload = json.loads(payload_text)
    except:
        payload = payload_text

    # Configurations
    if topic == "device/config/alarm_threshold":
        if isinstance(payload, dict):
            device_state["alarm_threshold"].update(payload)
            publish_json("device/config/alarm_threshold", {"result": "ok", "set": device_state["alarm_threshold"]}, qos=2)
    elif topic == "device/config/led":
        if isinstance(payload, str) and payload.startswith("BRIGHTNESS:"):
            level = int(payload.split(":", 1)[1])
            device_state["led_brightness"] = level
            device_state["status"]["led"] = "on" if level > 0 else "off"
            publish_json("device/config/led", {"result": "ok", "brightness": level}, qos=2)
    elif topic == "device/config/buzzer":
        if payload in ["ENABLE", "DISABLE"]:
            device_state["buzzer"] = payload
            publish_json("device/config/buzzer", {"result": "ok", "buzzer": payload}, qos=2)
    # Commands
    elif topic == "device/commands/calibrate":
        # Simulate calibration
        publish_json("device/commands/calibrate", {"result": "calibration started"}, qos=2)
        time.sleep(1) # Simulate delay
        publish_json("device/commands/calibrate", {"result": "calibration complete"}, qos=2)
    elif topic == "device/commands/restart":
        publish_json("device/commands/restart", {"result": "device restarting"}, qos=2)
        time.sleep(1)
        publish_json("device/commands/restart", {"result": "device online"}, qos=2)
    elif topic == "device/commands/reset":
        publish_json("device/commands/reset", {"result": "device resetting"}, qos=2)
        time.sleep(2)
        # Reset state
        device_state["temperature"] = 22.0
        device_state["humidity"] = 50.0
        device_state["airquality"] = {"PM2.5": 12, "CO2": 400}
        device_state["battery"] = 100
        device_state["status"] = {"status": "normal", "led": "on"}
        publish_json("device/commands/reset", {"result": "reset complete"}, qos=2)

# ---- Main ----

def main():
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    # Start telemetry threads
    threading.Thread(target=simulate_telemetry, daemon=True).start()
    threading.Thread(target=simulate_airquality, daemon=True).start()
    # Loop forever
    client.loop_forever()

if __name__ == "__main__":
    main()