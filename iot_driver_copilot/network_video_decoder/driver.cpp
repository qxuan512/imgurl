#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <map>
#include <condition_variable>
#include <json/json.h>
#include "HCNetSDK.h"
#include "MQTTClient.h"

#define QOS_1 1
#define TIMEOUT 10000L

// Helper: Get env var with optional default
std::string getenv_def(const char* name, const char* def = nullptr) {
    const char* val = std::getenv(name);
    return val ? val : (def ? def : "");
}

// HCNetSDK management
class HikvisionDecoder {
public:
    HikvisionDecoder(const std::string& ip, int port, const std::string& user, const std::string& pass)
        : m_lUserID(-1), m_ip(ip), m_port(port), m_user(user), m_pass(pass) {
        NET_DVR_Init();
    }
    ~HikvisionDecoder() {
        if (m_lUserID >= 0) {
            NET_DVR_Logout(m_lUserID);
        }
        NET_DVR_Cleanup();
    }
    bool login() {
        NET_DVR_USER_LOGIN_INFO loginInfo = {0};
        NET_DVR_DEVICEINFO_V40 devInfo = {0};
        strncpy(loginInfo.sDeviceAddress, m_ip.c_str(), sizeof(loginInfo.sDeviceAddress) - 1);
        loginInfo.wPort = m_port;
        strncpy(loginInfo.sUserName, m_user.c_str(), sizeof(loginInfo.sUserName) - 1);
        strncpy(loginInfo.sPassword, m_pass.c_str(), sizeof(loginInfo.sPassword) - 1);
        loginInfo.bUseAsynLogin = 0;
        m_lUserID = NET_DVR_Login_V40(&loginInfo, &devInfo);
        return m_lUserID >= 0;
    }
    void logout() {
        if (m_lUserID >= 0) {
            NET_DVR_Logout(m_lUserID);
            m_lUserID = -1;
        }
    }
    // Example: Enable/Disable decoder channel
    bool setDecoderChannel(int channel, bool enable) {
        NET_DVR_MATRIX_DEC_CHAN_ENABLE decEnable = {0};
        decEnable.dwSize = sizeof(decEnable);
        decEnable.dwEnable = enable ? 1 : 0;
        LONG ret = NET_DVR_SetDVRConfig(m_lUserID, NET_DVR_MATRIX_DEC_CHAN_ENABLE, channel, &decEnable, sizeof(decEnable));
        return ret == TRUE;
    }
    // Example: Reboot device
    bool reboot() {
        return NET_DVR_RebootDVR(m_lUserID) == TRUE;
    }
    // Example: Set config - expects config in JSON
    bool setConfig(const Json::Value& cfg) {
        // Only implements time, network, user as examples
        bool ok = true;
        if (cfg.isMember("time")) {
            NET_DVR_TIME time = {0};
            auto t = cfg["time"];
            time.dwYear = t.get("year", 2023).asUInt();
            time.dwMonth = t.get("month", 1).asUInt();
            time.dwDay = t.get("day", 1).asUInt();
            time.dwHour = t.get("hour", 0).asUInt();
            time.dwMinute = t.get("minute", 0).asUInt();
            time.dwSecond = t.get("second", 0).asUInt();
            ok &= NET_DVR_SetDVRConfig(m_lUserID, NET_DVR_SET_TIMECFG, 0, &time, sizeof(time));
        }
        // ... Implement other config as needed
        return ok;
    }
    // Gather status for telemetry
    Json::Value getStatus() {
        Json::Value status;
        NET_DVR_WORKSTATE_V40 workState = {0};
        DWORD dwReturned = 0;
        if (NET_DVR_GetDVRWorkState_V40(m_lUserID, &workState)) {
            status["device_health"] = (bool)workState.struDeviceStatic.struDeviceStaticInfo.byDeviceStatus;
            status["decoder_channel_status"] = Json::arrayValue;
            for (int i = 0; i < workState.dwDecChanNum; ++i) {
                Json::Value ch;
                ch["channel"] = i + 1;
                ch["status"] = (unsigned)workState.pDecChanStatus[i].byDecodeStatus;
                status["decoder_channel_status"].append(ch);
            }
            status["alarm_in_status"] = (unsigned)workState.struDeviceStatic.struDeviceStaticInfo.byAlarmInStatus;
            status["alarm_out_status"] = (unsigned)workState.struDeviceStatic.struDeviceStaticInfo.byAlarmOutStatus;
        } else {
            status["error"] = (int)NET_DVR_GetLastError();
        }
        return status;
    }
private:
    LONG m_lUserID;
    std::string m_ip;
    int m_port;
    std::string m_user, m_pass;
};

// MQTT Client wrapper
class MQTTWrapper {
public:
    MQTTWrapper(const std::string& broker, int port, const std::string& client_id)
        : m_broker(broker), m_port(port), m_client_id(client_id), m_connected(false) {
        MQTTClient_create(&m_client, (m_broker + ":" + std::to_string(m_port)).c_str(),
                          m_client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);
        MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
        conn_opts.keepAliveInterval = 20;
        conn_opts.cleansession = 1;
        if (MQTTClient_connect(m_client, &conn_opts) == MQTTCLIENT_SUCCESS) {
            m_connected = true;
        }
    }
    ~MQTTWrapper() {
        if (m_connected)
            MQTTClient_disconnect(m_client, 1000);
        MQTTClient_destroy(&m_client);
    }
    bool publish(const std::string& topic, const std::string& payload, int qos) {
        MQTTClient_message pubmsg = MQTTClient_message_initializer;
        pubmsg.payload = (void*)payload.c_str();
        pubmsg.payloadlen = (int)payload.size();
        pubmsg.qos = qos;
        pubmsg.retained = 0;
        MQTTClient_deliveryToken token;
        return MQTTClient_publishMessage(m_client, topic.c_str(), &pubmsg, &token) == MQTTCLIENT_SUCCESS &&
            MQTTClient_waitForCompletion(m_client, token, TIMEOUT) == MQTTCLIENT_SUCCESS;
    }
    bool subscribe(const std::string& topic, int qos) {
        return MQTTClient_subscribe(m_client, topic.c_str(), qos) == MQTTCLIENT_SUCCESS;
    }
    void setCallback(MQTTClient_messageArrived* msgArrived, void* context) {
        MQTTClient_setCallbacks(m_client, context, NULL, msgArrived, NULL);
    }
    MQTTClient m_client;
    bool m_connected;
private:
    std::string m_broker;
    int m_port;
    std::string m_client_id;
};

// Global references
std::atomic<bool> g_running{true};
std::mutex g_status_mutex;
Json::Value g_last_status;

// MQTT message arrived callback
int messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message) {
    HikvisionDecoder* decoder = static_cast<HikvisionDecoder*>(context);
    std::string topic(topicName, topicLen > 0 ? topicLen : strlen(topicName));
    std::string payload((char*)message->payload, message->payloadlen);
    Json::Value resp;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    Json::Value root;
    std::istringstream iss(payload);
    bool parseOk = Json::parseFromStream(rbuilder, iss, &root, &errs);
    if (topic == "device/commands/decoder" && parseOk) {
        std::string action = root.get("action", "").asString();
        int channel = root.get("channel", -1).asInt();
        bool ok = false;
        if (action == "enable" && channel > 0)
            ok = decoder->setDecoderChannel(channel, true);
        else if (action == "disable" && channel > 0)
            ok = decoder->setDecoderChannel(channel, false);
        resp["result"] = ok ? "success" : "fail";
        resp["action"] = action;
        resp["channel"] = channel;
    } else if (topic == "device/commands/reboot" && parseOk) {
        if (root.get("command", "") == "reboot") {
            bool ok = decoder->reboot();
            resp["result"] = ok ? "success" : "fail";
        }
    } else if (topic == "device/commands/config" && parseOk) {
        bool ok = decoder->setConfig(root);
        resp["result"] = ok ? "success" : "fail";
    }
    if (!resp.isNull()) {
        Json::StreamWriterBuilder wbuilder;
        std::string ans = Json::writeString(wbuilder, resp);
        MQTTWrapper* mqtt = (MQTTWrapper*)(((void**)context)[1]);
        mqtt->publish("device/commands/response", ans, QOS_1);
    }
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

void telemetryLoop(HikvisionDecoder* decoder, MQTTWrapper* mqtt, int interval_sec) {
    while (g_running) {
        Json::Value status = decoder->getStatus();
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            g_last_status = status;
        }
        Json::StreamWriterBuilder wbuilder;
        std::string payload = Json::writeString(wbuilder, status);
        mqtt->publish("device/status", payload, QOS_1);
        std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
    }
}

int main() {
    // Environment: device config
    std::string dev_ip   = getenv_def("DEVICE_IP", "192.168.1.64");
    int dev_port         = std::stoi(getenv_def("DEVICE_PORT", "8000"));
    std::string dev_user = getenv_def("DEVICE_USER", "admin");
    std::string dev_pass = getenv_def("DEVICE_PASSWORD", "12345");
    // MQTT config
    std::string mqtt_host = getenv_def("MQTT_BROKER", "tcp://127.0.0.1");
    int mqtt_port         = std::stoi(getenv_def("MQTT_PORT", "1883"));
    std::string mqtt_clientid = getenv_def("MQTT_CLIENT_ID", "hikvision_decoder_driver");
    int telemetry_interval = std::stoi(getenv_def("TELEMETRY_INTERVAL", "5"));

    HikvisionDecoder decoder(dev_ip, dev_port, dev_user, dev_pass);
    if (!decoder.login()) {
        std::cerr << "Failed to login to device at " << dev_ip << ":" << dev_port << std::endl;
        return 1;
    }
    MQTTWrapper mqtt(mqtt_host, mqtt_port, mqtt_clientid);
    if (!mqtt.m_connected) {
        std::cerr << "Failed to connect to MQTT broker " << mqtt_host << ":" << mqtt_port << std::endl;
        return 2;
    }

    // Subscribe to command topics
    mqtt.subscribe("device/commands/decoder", QOS_1);
    mqtt.subscribe("device/commands/reboot", QOS_1);
    mqtt.subscribe("device/commands/config", QOS_1);

    // Set callback
    void* context[2] = {&decoder, &mqtt};
    mqtt.setCallback(messageArrived, context);

    // Start telemetry thread
    std::thread telemetry_thread(telemetryLoop, &decoder, &mqtt, telemetry_interval);

    // Also support subscribe to device/status (for other clients)
    mqtt.subscribe("device/status", QOS_1);

    std::cout << "MQTT driver for Hikvision Decoder running. Press Ctrl+C to exit." << std::endl;
    // Run main loop (wait for exit)
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    telemetry_thread.join();
    decoder.logout();
    return 0;
}