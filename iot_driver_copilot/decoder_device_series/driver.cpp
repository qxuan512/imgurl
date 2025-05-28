#include <cstdlib>
#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <sstream>
#include <json/json.h> // Requires jsoncpp. No 3rd party command, pure header usage assumed.
#include "MQTTClient.h"

// ----- Device SDK/Protocol Simulation (to be replaced with actual device SDK) -----
class HikvisionDecoderDevice {
public:
    HikvisionDecoderDevice(const std::string& ip, int port, const std::string& user, const std::string& pass)
        : ip_(ip), port_(port), user_(user), pass_(pass) {
        // Simulate connection.
    }

    bool enableChannel(int channel) {
        std::lock_guard<std::mutex> lk(dev_mutex_);
        channel_states_[channel] = true;
        return true;
    }
    bool disableChannel(int channel) {
        std::lock_guard<std::mutex> lk(dev_mutex_);
        channel_states_[channel] = false;
        return true;
    }

    bool startDecoding() {
        std::lock_guard<std::mutex> lk(dev_mutex_);
        decoding_ = true;
        return true;
    }
    bool stopDecoding() {
        std::lock_guard<std::mutex> lk(dev_mutex_);
        decoding_ = false;
        return true;
    }

    Json::Value getStatus() {
        std::lock_guard<std::mutex> lk(dev_mutex_);
        Json::Value status;
        status["device"] = "DS-6300D";
        status["decoding"] = decoding_;
        Json::Value channels(Json::arrayValue);
        for (const auto& kv : channel_states_) {
            Json::Value ch;
            ch["channel"] = kv.first;
            ch["enabled"] = kv.second;
            channels.append(ch);
        }
        status["channels"] = channels;
        status["sdk_version"] = "5.3.1";
        status["build_info"] = "Build2024-06";
        return status;
    }

    std::vector<Json::Value> popAlarms() {
        std::lock_guard<std::mutex> lk(dev_mutex_);
        std::vector<Json::Value> ret = alarms_;
        alarms_.clear();
        return ret;
    }
    void simulateAlarm(const std::string& msg) {
        std::lock_guard<std::mutex> lk(dev_mutex_);
        Json::Value alarm;
        alarm["type"] = "alarm";
        alarm["message"] = msg;
        alarm["timestamp"] = static_cast<Json::UInt64>(std::chrono::system_clock::now().time_since_epoch().count());
        alarms_.push_back(alarm);
    }
private:
    std::string ip_;
    int port_;
    std::string user_;
    std::string pass_;
    std::map<int, bool> channel_states_;
    bool decoding_ = false;
    std::vector<Json::Value> alarms_;
    std::mutex dev_mutex_;
};
// -------------------------------------------------------------------------------

// -------------- MQTT Config via Environment Variables --------------------------
std::string getenv_or_default(const char* env, const std::string& def) {
    const char* val = std::getenv(env);
    return val ? val : def;
}
int getenv_or_default_int(const char* env, int def) {
    const char* val = std::getenv(env);
    return val ? std::atoi(val) : def;
}
// ------------------------------------------------------------------------------

// --------------- MQTT Callback and Logic --------------------------------------
#define QOS1 1
#define TIMEOUT 10000L

class MQTTDriver {
public:
    MQTTDriver(HikvisionDecoderDevice* device)
        : device_(device), running_(true) {
        broker_ = getenv_or_default("MQTT_BROKER", "tcp://localhost:1883");
        client_id_ = getenv_or_default("MQTT_CLIENT_ID", "hikvision_decoder_driver");
        username_ = getenv_or_default("MQTT_USERNAME", "");
        password_ = getenv_or_default("MQTT_PASSWORD", "");
        status_topic_ = "device/telemetry/status";
        alarm_topic_ = "device/telemetry/alarm";
        cmd_channel_topic_ = "device/commands/channel";
        cmd_decode_topic_ = "device/commands/decode";
    }

    void start() {
        MQTTClient_create(&client_, broker_.c_str(), client_id_.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);
        MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
        if (!username_.empty()) {
            conn_opts.username = username_.c_str();
            conn_opts.password = password_.c_str();
        }
        conn_opts.keepAliveInterval = 20;
        conn_opts.cleansession = 1;

        MQTTClient_setCallbacks(client_, this, NULL, &MQTTDriver::msgarrvd, NULL);

        int rc;
        if ((rc = MQTTClient_connect(client_, &conn_opts)) != MQTTCLIENT_SUCCESS) {
            std::cerr << "Failed to connect to MQTT Broker, return code " << rc << std::endl;
            exit(EXIT_FAILURE);
        }

        // Subscribe to command topics
        MQTTClient_subscribe(client_, cmd_channel_topic_.c_str(), QOS1);
        MQTTClient_subscribe(client_, cmd_decode_topic_.c_str(), QOS1);

        // Start telemetry threads
        status_thread_ = std::thread(&MQTTDriver::statusLoop, this);
        alarm_thread_ = std::thread(&MQTTDriver::alarmLoop, this);
    }

    void stop() {
        running_ = false;
        if (status_thread_.joinable()) status_thread_.join();
        if (alarm_thread_.joinable()) alarm_thread_.join();
        MQTTClient_disconnect(client_, 1000);
        MQTTClient_destroy(&client_);
    }

    static int msgarrvd(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
        MQTTDriver* driver = static_cast<MQTTDriver*>(context);
        std::string topic(topicName, topicLen > 0 ? topicLen : strlen(topicName));
        std::string payload((char*)message->payload, message->payloadlen);

        if (topic == driver->cmd_channel_topic_) {
            driver->processChannelCmd(payload);
        } else if (topic == driver->cmd_decode_topic_) {
            driver->processDecodeCmd(payload);
        }
        MQTTClient_freeMessage(&message);
        MQTTClient_free(topicName);
        return 1;
    }

    void processChannelCmd(const std::string& payload) {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(payload, root)) return;
        if (!root.isMember("state")) return;
        std::string state = root["state"].asString();
        if (state == "enable" && root.isMember("channel")) {
            int channel = root["channel"].asInt();
            if (device_->enableChannel(channel)) {
                // Optionally publish status.
            }
        } else if (state == "disable" && root.isMember("channel")) {
            int channel = root["channel"].asInt();
            if (device_->disableChannel(channel)) {
                // Optionally publish status.
            }
        }
    }

    void processDecodeCmd(const std::string& payload) {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(payload, root)) return;
        if (!root.isMember("action")) return;
        std::string action = root["action"].asString();
        if (action == "start") {
            device_->startDecoding();
        } else if (action == "stop") {
            device_->stopDecoding();
        }
    }

    void statusLoop() {
        while (running_) {
            Json::Value status = device_->getStatus();
            Json::FastWriter writer;
            std::string msg = writer.write(status);
            MQTTClient_message pubmsg = MQTTClient_message_initializer;
            pubmsg.payload = (void*)msg.c_str();
            pubmsg.payloadlen = (int)msg.length();
            pubmsg.qos = QOS1;
            pubmsg.retained = 0;
            MQTTClient_publishMessage(client_, status_topic_.c_str(), &pubmsg, NULL);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    void alarmLoop() {
        while (running_) {
            std::vector<Json::Value> alarms = device_->popAlarms();
            for (const auto& alarm : alarms) {
                Json::FastWriter writer;
                std::string msg = writer.write(alarm);
                MQTTClient_message pubmsg = MQTTClient_message_initializer;
                pubmsg.payload = (void*)msg.c_str();
                pubmsg.payloadlen = (int)msg.length();
                pubmsg.qos = QOS1;
                pubmsg.retained = 0;
                MQTTClient_publishMessage(client_, alarm_topic_.c_str(), &pubmsg, NULL);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

private:
    HikvisionDecoderDevice* device_;
    MQTTClient client_;
    std::string broker_;
    std::string client_id_;
    std::string username_;
    std::string password_;
    std::string status_topic_;
    std::string alarm_topic_;
    std::string cmd_channel_topic_;
    std::string cmd_decode_topic_;
    std::thread status_thread_;
    std::thread alarm_thread_;
    bool running_;
};
// ------------------------------------------------------------------------------

// ---------------------- Main Function -----------------------------------------
int main() {
    // Device params from env
    std::string dev_ip = getenv_or_default("DEVICE_IP", "192.168.1.64");
    int dev_port = getenv_or_default_int("DEVICE_PORT", 8000);
    std::string dev_user = getenv_or_default("DEVICE_USER", "admin");
    std::string dev_pass = getenv_or_default("DEVICE_PASSWORD", "12345");

    // Create device abstraction
    HikvisionDecoderDevice device(dev_ip, dev_port, dev_user, dev_pass);

    // Simulate alarm injection thread (for demonstration)
    std::thread alarm_sim([&device]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            device.simulateAlarm("Simulated device alarm event");
        }
    });
    alarm_sim.detach();

    // MQTT Driver
    MQTTDriver driver(&device);
    driver.start();

    // Wait for termination (Ctrl-C)
    std::mutex wait_mutex;
    std::unique_lock<std::mutex> lk(wait_mutex);
    std::condition_variable cv;
    cv.wait(lk);

    driver.stop();
    return 0;
}
// ------------------------------------------------------------------------------