import os
import json
import time
import threading
import random
import sys
import signal
import paho.mqtt.client as mqtt

# Environment Variables
MQTT_BROKER = os.environ.get("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD")
DEVICE_ID = os.environ.get("DEVICE_ID", "smart_env_device_001")
PUBLISH_INTERVAL_30S = int(os.environ.get("PUBLISH_INTERVAL_30S", 30))
PUBLISH_INTERVAL_60S = int(os.environ.get("PUBLISH_INTERVAL_60S", 60))

client = mqtt.Client(client_id=DEVICE_ID, clean_session=True)

if MQTT_USERNAME and MQTT_PASSWORD:
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

# Device State
state = {
    "temperature": 22.5,
    "humidity": 45,
    "airquality": {"PM2.5": 12, "CO2": 400},
    "battery": 87,
    "status": {"status": "normal", "led": "on"},
    "led_brightness": 75,
    "buzzer": "DISABLE",
    "alarm_threshold": {"PM2.5": 35, "CO2": 800},
    "alarm_active": False
}

# Graceful exit
def signal_handler(sig, frame):
    client.disconnect()
    sys.exit(0)

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

# MQTT Callbacks
def on_connect(client, userdata, flags, rc):
    # Subscribe to command/config/topics (QoS as per API)
    client.subscribe("device/config/alarm_threshold", qos=2)
    client.subscribe("device/config/led", qos=2)
    client.subscribe("device/config/buzzer", qos=2)
    client.subscribe("device/commands/calibrate", qos=2)
    client.subscribe("device/commands/restart", qos=2)
    client.subscribe("device/commands/reset", qos=2)

def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode()
    # Handle configuration and commands
    if topic == "device/config/alarm_threshold":
        try:
            data = json.loads(payload)
            state["alarm_threshold"].update(data)
            # Optionally publish confirmation
            client.publish("device/config/alarm_threshold/ack",
                payload=json.dumps({"result": "ok", "threshold": state["alarm_threshold"]}),
                qos=2,
                retain=False)
        except Exception:
            client.publish("device/config/alarm_threshold/ack",
                payload=json.dumps({"result": "error"}),
                qos=2)
    elif topic == "device/config/led":
        if payload.startswith("BRIGHTNESS:"):
            try:
                value = int(payload.split(":")[1])
                state["led_brightness"] = value
                state["status"]["led"] = "on" if value > 0 else "off"
                client.publish("device/config/led/ack",
                    payload=json.dumps({"result": "ok", "led_brightness": value}),
                    qos=2)
            except Exception:
                client.publish("device/config/led/ack",
                    payload=json.dumps({"result": "error"}),
                    qos=2)
    elif topic == "device/config/buzzer":
        if payload in ("ENABLE", "DISABLE"):
            state["buzzer"] = payload
            client.publish("device/config/buzzer/ack",
                payload=json.dumps({"result": "ok", "buzzer": payload}),
                qos=2)
        else:
            client.publish("device/config/buzzer/ack",
                payload=json.dumps({"result": "error"}),
                qos=2)
    elif topic == "device/commands/calibrate":
        if payload.strip().upper() == "CALIBRATE":
            # Simulate calibration
            client.publish("device/commands/calibrate/ack",
                payload=json.dumps({"result": "calibrated"}),
                qos=2)
    elif topic == "device/commands/restart":
        if payload.strip().upper() == "RESTART":
            client.publish("device/commands/restart/ack",
                payload=json.dumps({"result": "restarting"}),
                qos=2)
            time.sleep(1)
            os.execv(sys.executable, ['python'] + sys.argv)
    elif topic == "device/commands/reset":
        if payload.strip().upper() == "RESET":
            # Simulate reset
            state["temperature"] = 22.5
            state["humidity"] = 45
            state["airquality"] = {"PM2.5": 12, "CO2": 400}
            state["battery"] = 100
            state["status"] = {"status": "normal", "led": "on"}
            state["led_brightness"] = 75
            state["buzzer"] = "DISABLE"
            state["alarm_threshold"] = {"PM2.5": 35, "CO2": 800}
            state["alarm_active"] = False
            client.publish("device/commands/reset/ack",
                payload=json.dumps({"result": "reset"}),
                qos=2)

client.on_connect = on_connect
client.on_message = on_message

def simulate_sensor():
    # Simulate changing sensor values
    state["temperature"] = round(20 + random.uniform(-2, 3), 1)
    state["humidity"] = int(40 + random.uniform(-5, 10))
    state["airquality"]["PM2.5"] = int(10 + random.uniform(-2, 7))
    state["airquality"]["CO2"] = int(400 + random.uniform(-20, 50))
    state["battery"] = max(0, state["battery"] - random.uniform(0.01, 0.1))
    # Alarm check
    alarm = None
    for k in ("PM2.5", "CO2"):
        if state["airquality"][k] > state["alarm_threshold"].get(k, 99999):
            alarm = {"alarm": "threshold exceeded", "sensor": k, "value": state["airquality"][k]}
            state["alarm_active"] = True
            break
    if alarm:
        client.publish(
            "device/alerts/alarm",
            payload=json.dumps(alarm),
            qos=2
        )
    else:
        state["alarm_active"] = False

def publish_telemetry():
    while True:
        simulate_sensor()
        # Publish temperature
        temp_payload = json.dumps({"temperature": state["temperature"], "unit": "C"})
        client.publish("device/sensors/temperature", payload=temp_payload, qos=0)
        # Publish humidity
        hum_payload = json.dumps({"humidity": state["humidity"], "unit": "%"})
        client.publish("device/sensors/humidity", payload=hum_payload, qos=0)
        # Publish battery
        bat_payload = json.dumps({"battery": int(state["battery"])})
        client.publish("device/sensors/battery", payload=bat_payload, qos=0)
        # Publish status
        status_payload = json.dumps(state["status"])
        client.publish("device/sensors/status", payload=status_payload, qos=1)
        # Wait 30s for next batch
        time.sleep(PUBLISH_INTERVAL_30S)

def publish_airquality():
    while True:
        air_payload = json.dumps(state["airquality"])
        client.publish("device/sensors/airquality", payload=air_payload, qos=0)
        time.sleep(PUBLISH_INTERVAL_60S)

def main():
    client.will_set("device/sensors/status", payload=json.dumps({"status": "offline"}), qos=1, retain=True)
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    # Start background publishing threads
    t1 = threading.Thread(target=publish_telemetry, daemon=True)
    t2 = threading.Thread(target=publish_airquality, daemon=True)
    t1.start()
    t2.start()
    client.loop_forever()

if __name__ == "__main__":
    main()