#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <map>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstring>
#include <json/json.h>

#include "mqtt/async_client.h"

// Mock SDK API functions and data structures
// In real implementation, link against the actual Hikvision SDK
namespace hikvision_sdk {
struct DeviceStatus {
    int channel_count;
    std::vector<bool> channel_enabled;
    std::string version;
    std::string build;
    bool is_decoding;
    std::string last_alarm;
};

struct DeviceConfig {
    std::string display_config;
    std::string network_config;
};

DeviceStatus global_status = {
    8,
    std::vector<bool>(8, true),
    "V1.2.3",
    "Build20240501",
    false,
    ""
};

DeviceConfig global_config = {
    "DefaultDisplay",
    "192.168.1.100"
};

std::mutex device_mutex;

bool enable_channel(int channel, bool enable) {
    std::lock_guard<std::mutex> lock(device_mutex);
    if(channel < 0 || channel >= global_status.channel_count) return false;
    global_status.channel_enabled[channel] = enable;
    return true;
}

bool set_display_config(const std::string& config) {
    std::lock_guard<std::mutex> lock(device_mutex);
    global_config.display_config = config;
    return true;
}

bool set_network_config(const std::string& config) {
    std::lock_guard<std::mutex> lock(device_mutex);
    global_config.network_config = config;
    return true;
}

bool start_decoding() {
    std::lock_guard<std::mutex> lock(device_mutex);
    global_status.is_decoding = true;
    return true;
}

bool stop_decoding() {
    std::lock_guard<std::mutex> lock(device_mutex);
    global_status.is_decoding = false;
    return true;
}

bool reboot() {
    std::lock_guard<std::mutex> lock(device_mutex);
    global_status.is_decoding = false;
    return true;
}

bool restore_defaults() {
    std::lock_guard<std::mutex> lock(device_mutex);
    global_status = DeviceStatus{8, std::vector<bool>(8, true), "V1.2.3", "Build20240501", false, ""};
    global_config = DeviceConfig{"DefaultDisplay", "192.168.1.100"};
    return true;
}

DeviceStatus get_status() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return global_status;
}

DeviceConfig get_config() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return global_config;
}

void inject_alarm(const std::string& alarm) {
    std::lock_guard<std::mutex> lock(device_mutex);
    global_status.last_alarm = alarm;
}

} // namespace hikvision_sdk

// MQTT driver implementation
class DecoderDeviceMqttDriver {
public:
    DecoderDeviceMqttDriver() :
        mqtt_broker(getenvvar("MQTT_BROKER", "tcp://localhost:1883")),
        mqtt_client_id(getenvvar("MQTT_CLIENT_ID", "hikvision_decoder_driver")),
        mqtt_username(getenvvar("MQTT_USERNAME", "")),
        mqtt_password(getenvvar("MQTT_PASSWORD", "")),
        mqtt_qos(1),
        mqtt_client(mqtt_broker, mqtt_client_id),
        running(true)
    {
        connect_mqtt();
        subscribe_topics();
        telemetry_thread = std::thread(&DecoderDeviceMqttDriver::telemetry_loop, this);
    }

    ~DecoderDeviceMqttDriver() {
        running = false;
        if (telemetry_thread.joinable()) telemetry_thread.join();
        mqtt_client.disconnect()->wait();
    }

    void loop() {
        // Wait for exit (Ctrl+C)
        std::unique_lock<std::mutex> lk(exit_mutex);
        exit_cv.wait(lk);
    }

private:
    const std::string mqtt_broker;
    const std::string mqtt_client_id;
    const std::string mqtt_username;
    const std::string mqtt_password;

    const int mqtt_qos;
    mqtt::async_client mqtt_client;
    mqtt::connect_options conn_opts;
    std::atomic<bool> running;
    std::thread telemetry_thread;

    std::mutex exit_mutex;
    std::condition_variable exit_cv;

    void connect_mqtt() {
        conn_opts.set_clean_session(true);
        if (!mqtt_username.empty())
            conn_opts.set_user_name(mqtt_username);
        if (!mqtt_password.empty())
            conn_opts.set_password(mqtt_password);

        while (true) {
            try {
                mqtt_client.connect(conn_opts)->wait();
                break;
            } catch (const mqtt::exception& exc) {
                std::cerr << "MQTT connect failed: " << exc.what() << ", retrying in 2s\n";
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }

    void subscribe_topics() {
        // Subscribe to command topics
        mqtt_client.set_callback([this](mqtt::const_message_ptr msg) {
            handle_mqtt_message(msg);
        });

        // Subscriptions for telemetry topics (they are output, so not actually needed as subscriptions)
        // But for completeness, we subscribe so that if the driver is expected to process inbound device telemetry, it can be extended

        // For this driver, we mostly act as publisher on telemetry topics.
        // Subscribe to command topics:
        mqtt_client.subscribe("device/commands/channel", mqtt_qos)->wait();
        mqtt_client.subscribe("device/commands/control", mqtt_qos)->wait();
        mqtt_client.subscribe("device/commands/decode", mqtt_qos)->wait();
        mqtt_client.subscribe("device/commands/config", mqtt_qos)->wait();
    }

    void handle_mqtt_message(mqtt::const_message_ptr msg) {
        const std::string& topic = msg->get_topic();
        const std::string& payload = msg->to_string();

        if (topic == "device/commands/channel") {
            handle_channel_command(payload);
        } else if (topic == "device/commands/control") {
            handle_control_command(payload);
        } else if (topic == "device/commands/decode") {
            handle_decode_command(payload);
        } else if (topic == "device/commands/config") {
            handle_config_command(payload);
        }
    }

    void handle_channel_command(const std::string& payload) {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(payload, root)) return;

        int channel = root.get("channel", -1).asInt();
        std::string state = root.get("state", "").asString();

        bool result = false;
        if (channel >= 0 && (state == "enable" || state == "disable")) {
            result = hikvision_sdk::enable_channel(channel, state == "enable");
        }

        // Optionally, publish status update
        publish_device_status();
    }

    void handle_control_command(const std::string& payload) {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(payload, root)) return;

        std::string command = root.get("command", "").asString();
        bool result = false;

        if (command == "start_decoding") {
            result = hikvision_sdk::start_decoding();
        } else if (command == "stop_decoding") {
            result = hikvision_sdk::stop_decoding();
        } else if (command == "reboot") {
            result = hikvision_sdk::reboot();
        } else if (command == "restore_defaults") {
            result = hikvision_sdk::restore_defaults();
        } else if (command == "enable_channel" || command == "disable_channel") {
            int channel = root.get("channel", -1).asInt();
            if (channel >= 0) {
                result = hikvision_sdk::enable_channel(channel, command == "enable_channel");
            }
        } else if (command == "inject_alarm") {
            std::string alarm = root.get("alarm", "manual_test_alarm").asString();
            hikvision_sdk::inject_alarm(alarm);
        }
        // Optionally, publish status update and alarm
        publish_device_status();
        if (command == "inject_alarm") {
            publish_alarm();
        }
    }

    void handle_decode_command(const std::string& payload) {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(payload, root)) return;

        std::string action = root.get("action", "").asString();

        if (action == "start") {
            hikvision_sdk::start_decoding();
        } else if (action == "stop") {
            hikvision_sdk::stop_decoding();
        }
        // Optionally, publish status update
        publish_device_status();
    }

    void handle_config_command(const std::string& payload) {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(payload, root)) return;

        std::string config_type = root.get("type", "").asString();
        std::string value = root.get("value", "").asString();

        if (config_type == "display") {
            hikvision_sdk::set_display_config(value);
        } else if (config_type == "network") {
            hikvision_sdk::set_network_config(value);
        }
        // Optionally, publish status update
        publish_device_status();
    }

    void telemetry_loop() {
        // Periodically publish telemetry
        while (running) {
            publish_device_status();
            publish_alarm();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    void publish_device_status() {
        hikvision_sdk::DeviceStatus status = hikvision_sdk::get_status();
        hikvision_sdk::DeviceConfig config = hikvision_sdk::get_config();

        Json::Value root;
        root["channel_count"] = status.channel_count;
        Json::Value ch_en(Json::arrayValue);
        for (bool enabled : status.channel_enabled) ch_en.append(enabled ? "enabled" : "disabled");
        root["channel_enabled"] = ch_en;
        root["version"] = status.version;
        root["build"] = status.build;
        root["is_decoding"] = status.is_decoding;
        root["display_config"] = config.display_config;
        root["network_config"] = config.network_config;

        std::string payload = toJsonString(root);

        mqtt::message_ptr msg = mqtt::make_message("device/telemetry/status", payload, mqtt_qos, false);
        mqtt_client.publish(msg);
    }

    void publish_alarm() {
        hikvision_sdk::DeviceStatus status = hikvision_sdk::get_status();
        if (status.last_alarm.empty()) return;

        Json::Value root;
        root["alarm"] = status.last_alarm;
        root["timestamp"] = static_cast<Json::UInt64>(std::chrono::system_clock::now().time_since_epoch().count());
        std::string payload = toJsonString(root);

        mqtt::message_ptr msg = mqtt::make_message("device/telemetry/alarm", payload, mqtt_qos, false);
        mqtt_client.publish(msg);

        // Clear the alarm after publishing
        hikvision_sdk::inject_alarm("");
    }

    static std::string getenvvar(const char* name, const char* def = "") {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string(def);
    }

    static std::string toJsonString(const Json::Value& value) {
        Json::StreamWriterBuilder wbuilder;
        wbuilder["indentation"] = "";
        return Json::writeString(wbuilder, value);
    }
};

int main() {
    DecoderDeviceMqttDriver driver;
    driver.loop();
    return 0;
}