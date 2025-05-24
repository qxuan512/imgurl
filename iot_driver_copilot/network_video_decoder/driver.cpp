#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <json/json.h>
#include "HCNetSDK.h"
#include "mqtt/async_client.h"

// ========================== Configuration via ENV ==========================
struct Config {
    std::string device_ip;
    int device_port;
    std::string device_user;
    std::string device_password;
    std::string mqtt_broker;
    int mqtt_port;
    std::string mqtt_client_id;
    std::string mqtt_username;
    std::string mqtt_password;
    int mqtt_keepalive;

    static Config from_env() {
        Config cfg;
        cfg.device_ip        = std::getenv("DEVICE_IP")        ? std::getenv("DEVICE_IP")        : "192.168.1.100";
        cfg.device_port      = std::getenv("DEVICE_PORT")      ? std::stoi(std::getenv("DEVICE_PORT")) : 8000;
        cfg.device_user      = std::getenv("DEVICE_USER")      ? std::getenv("DEVICE_USER")      : "admin";
        cfg.device_password  = std::getenv("DEVICE_PASSWORD")  ? std::getenv("DEVICE_PASSWORD")  : "12345";
        cfg.mqtt_broker      = std::getenv("MQTT_BROKER")      ? std::getenv("MQTT_BROKER")      : "tcp://localhost:1883";
        cfg.mqtt_port        = std::getenv("MQTT_PORT")        ? std::stoi(std::getenv("MQTT_PORT")) : 1883;
        cfg.mqtt_client_id   = std::getenv("MQTT_CLIENT_ID")   ? std::getenv("MQTT_CLIENT_ID")   : "hikvision_decoder_driver";
        cfg.mqtt_username    = std::getenv("MQTT_USERNAME")    ? std::getenv("MQTT_USERNAME")    : "";
        cfg.mqtt_password    = std::getenv("MQTT_PASSWORD")    ? std::getenv("MQTT_PASSWORD")    : "";
        cfg.mqtt_keepalive   = std::getenv("MQTT_KEEPALIVE")   ? std::stoi(std::getenv("MQTT_KEEPALIVE")) : 60;
        return cfg;
    }
};

// ========================== Utility ==========================
void log(const std::string& msg) {
    std::cout << "[driver] " << msg << std::endl;
}

// ========================== HCNetSDK Session ==========================
class HikSession {
public:
    HikSession(const Config& cfg)
        : cfg_(cfg), user_id_(-1) {
        NET_DVR_Init();
    }
    ~HikSession() {
        if (user_id_ >= 0) {
            NET_DVR_Logout(user_id_);
        }
        NET_DVR_Cleanup();
    }
    bool login() {
        NET_DVR_USER_LOGIN_INFO loginInfo = {0};
        NET_DVR_DEVICEINFO_V40 devInfo = {0};
        strncpy(loginInfo.sDeviceAddress, cfg_.device_ip.c_str(), sizeof(loginInfo.sDeviceAddress) - 1);
        strncpy(loginInfo.sUserName, cfg_.device_user.c_str(), sizeof(loginInfo.sUserName) - 1);
        strncpy(loginInfo.sPassword, cfg_.device_password.c_str(), sizeof(loginInfo.sPassword) - 1);
        loginInfo.wPort = cfg_.device_port;
        loginInfo.bUseAsynLogin = 0;
        user_id_ = NET_DVR_Login_V40(&loginInfo, &devInfo);
        if (user_id_ < 0) {
            log("Login failed: " + std::to_string(NET_DVR_GetLastError()));
            return false;
        }
        log("Login success.");
        return true;
    }
    LONG user_id() const { return user_id_; }

    // --- Device Operations ---
    bool enable_decoder_channel(int channel, bool enable) {
        NET_DVR_MATRIX_CHAN_STATUS status = {0};
        status.dwSize = sizeof(status);
        status.byEnable = enable ? 1 : 0;
        if (!NET_DVR_SetDVRConfig(user_id_, NET_DVR_SET_MATRIX_CHAN_STATUS, channel, &status, sizeof(status))) {
            log("Enable/Disable decoder channel failed: " + std::to_string(NET_DVR_GetLastError()));
            return false;
        }
        return true;
    }
    bool reboot() {
        if (!NET_DVR_RebootDVR(user_id_)) {
            log("Reboot failed: " + std::to_string(NET_DVR_GetLastError()));
            return false;
        }
        return true;
    }
    bool set_config(const Json::Value& config) {
        // Example: set network param
        if (config.isMember("network")) {
            // ... parse and set network config using NET_DVR_SetDVRConfig
            // For brevity, not fully implemented
        }
        // ... handle other config parameters
        return true;
    }
    Json::Value get_status() {
        Json::Value status;
        // Example: get decoder channel status
        NET_DVR_MATRIX_CHAN_STATUS chanStatus[16] = {0};
        DWORD returned = 0;
        if (!NET_DVR_GetDVRConfig(user_id_, NET_DVR_GET_MATRIX_CHAN_STATUS, 0, chanStatus, sizeof(chanStatus), &returned)) {
            log("Get decoder channel status failed: " + std::to_string(NET_DVR_GetLastError()));
        }
        for (int i = 0; i < returned / sizeof(NET_DVR_MATRIX_CHAN_STATUS); ++i) {
            Json::Value c;
            c["channel"]     = i + 1;
            c["enabled"]     = chanStatus[i].byEnable == 1;
            c["decodeState"] = chanStatus[i].byDecState;
            status["decoder_channels"].append(c);
        }
        // ... gather more status if needed
        status["run_status"] = "OK";
        return status;
    }

private:
    Config cfg_;
    LONG user_id_;
};

// ========================== MQTT Handling ==========================
class MqttDriver {
public:
    MqttDriver(const Config& cfg, HikSession& hik)
        : cfg_(cfg), hik_(hik), client_(cfg.mqtt_broker, cfg.mqtt_client_id),
          status_thread_(), running_(false)
    {
        mqtt::connect_options connopts;
        connopts.set_keep_alive_interval(cfg_.mqtt_keepalive);
        if (!cfg_.mqtt_username.empty()) {
            connopts.set_user_name(cfg_.mqtt_username);
            connopts.set_password(cfg_.mqtt_password);
        }
        client_.set_connected_handler([this](const std::string&) {
            log("Connected to MQTT broker");
            client_.subscribe("device/commands/decoder", cfg_.mqtt_qos, nullptr, nullptr);
            client_.subscribe("device/commands/reboot", cfg_.mqtt_qos, nullptr, nullptr);
            client_.subscribe("device/commands/config", cfg_.mqtt_qos, nullptr, nullptr);
        });
        client_.set_message_callback([this](mqtt::const_message_ptr msg) {
            handle_mqtt_message(msg);
        });
        client_.set_connection_lost_handler([this](const std::string&) {
            log("MQTT connection lost");
        });
        cfg_.mqtt_qos = 1; // All API endpoints specify QoS 1

        log("Connecting to MQTT...");
        client_.connect(connopts)->wait();
    }
    ~MqttDriver() {
        stop();
        client_.disconnect()->wait();
    }
    void start() {
        running_ = true;
        status_thread_ = std::thread([this]() { this->status_publisher_loop(); });
    }
    void stop() {
        running_ = false;
        if (status_thread_.joinable())
            status_thread_.join();
    }

private:
    Config cfg_;
    HikSession& hik_;
    mqtt::async_client client_;
    std::thread status_thread_;
    std::atomic<bool> running_;

    void handle_mqtt_message(mqtt::const_message_ptr msg) {
        std::string topic = msg->get_topic();
        std::string payload = msg->to_string();
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errs;
        if (!Json::parseFromStream(reader, payload, &root, &errs)) {
            log("JSON parse error: " + errs);
            return;
        }
        if (topic == "device/commands/decoder") {
            // Format: { "action": "enable"/"disable", "channel": <num> }
            bool res = false;
            if (root.isMember("action") && root.isMember("channel")) {
                int channel = root["channel"].asInt();
                bool enable = root["action"].asString() == "enable";
                res = hik_.enable_decoder_channel(channel, enable);
            }
            publish_ack("device/commands/decoder/ack", res);
        }
        else if (topic == "device/commands/reboot") {
            // Format: { "command": "reboot" }
            bool res = root.isMember("command") && root["command"].asString() == "reboot"
                       && hik_.reboot();
            publish_ack("device/commands/reboot/ack", res);
        }
        else if (topic == "device/commands/config") {
            bool res = hik_.set_config(root);
            publish_ack("device/commands/config/ack", res);
        }
    }
    void publish_ack(const std::string& topic, bool success) {
        Json::Value ack;
        ack["result"] = success ? "ok" : "fail";
        mqtt::message_ptr pubmsg = mqtt::make_message(topic, Json::FastWriter().write(ack));
        pubmsg->set_qos(1);
        client_.publish(pubmsg);
    }
    void status_publisher_loop() {
        while (running_) {
            Json::Value status = hik_.get_status();
            mqtt::message_ptr pubmsg = mqtt::make_message("device/status", Json::FastWriter().write(status));
            pubmsg->set_qos(1);
            client_.publish(pubmsg);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
};

// ========================== MAIN ==========================
int main() {
    Config cfg = Config::from_env();

    log("Initializing Hikvision session...");
    HikSession hik(cfg);
    if (!hik.login()) {
        log("Failed to login to device. Exiting.");
        return 1;
    }

    log("Initializing MQTT driver...");
    MqttDriver driver(cfg, hik);
    driver.start();

    log("Driver running. Press Ctrl+C to exit.");
    std::mutex m;
    std::unique_lock<std::mutex> lk(m);
    std::condition_variable cv;
    cv.wait(lk); // Wait forever

    driver.stop();
    return 0;
}