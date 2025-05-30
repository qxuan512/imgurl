```cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>

// YAML Parsing
#include <yaml-cpp/yaml.h>

// MQTT
#include "mqtt/async_client.h"

// Kubernetes In-Cluster API
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// DeviceShifu Constants
const std::string EDGEDEVICE_CRD_GROUP = "shifu.edgenesis.io";
const std::string EDGEDEVICE_CRD_VERSION = "v1alpha1";
const std::string EDGEDEVICE_CRD_PLURAL = "edgedevices";
const std::string EDGEDEVICE_STATUS_PHASE_PATH = "/status/edgeDevicePhase";

// DeviceShifu Phases
enum class EdgeDevicePhase { Pending, Running, Failed, Unknown };

std::string PhaseToString(EdgeDevicePhase phase) {
    switch (phase) {
        case EdgeDevicePhase::Pending: return "Pending";
        case EdgeDevicePhase::Running: return "Running";
        case EdgeDevicePhase::Failed: return "Failed";
        case EdgeDevicePhase::Unknown: return "Unknown";
    }
    return "Unknown";
}

// ========== Kubernetes In-Cluster API ==========

std::string GetServiceAccountToken() {
    std::ifstream token_file("/var/run/secrets/kubernetes.io/serviceaccount/token");
    if (!token_file) return "";
    std::stringstream buffer;
    buffer << token_file.rdbuf();
    return buffer.str();
}

std::string GetK8sCAPath() {
    return "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";
}

std::string GetK8sAPIServer() {
    const char* host = std::getenv("KUBERNETES_SERVICE_HOST");
    const char* port = std::getenv("KUBERNETES_SERVICE_PORT");
    if (!host || !port) return "";
    return "https://" + std::string(host) + ":" + std::string(port);
}

class K8sClient {
public:
    K8sClient() {
        ca_cert = GetK8sCAPath();
        base_url = GetK8sAPIServer();
        token = GetServiceAccountToken();
    }

    // Patch status.edgeDevicePhase
    bool PatchEdgeDevicePhase(const std::string& namespace_,
                              const std::string& name,
                              EdgeDevicePhase phase) {
        std::string url = base_url + "/apis/" + EDGEDEVICE_CRD_GROUP + "/" +
                          EDGEDEVICE_CRD_VERSION + "/namespaces/" + namespace_ +
                          "/" + EDGEDEVICE_CRD_PLURAL + "/" + name + "/status";
        std::string patch = R"({"status":{"edgeDevicePhase":")" + PhaseToString(phase) + R"("}})";
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/merge-patch+json");
        headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());

        CURL *curl = curl_easy_init();
        if (!curl) return false;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_cert.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, patch.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, patch.size());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK;
    }

    // Get EdgeDevice .spec.address
    std::string GetEdgeDeviceAddress(const std::string& namespace_, const std::string& name) {
        std::string url = base_url + "/apis/" + EDGEDEVICE_CRD_GROUP + "/" +
                          EDGEDEVICE_CRD_VERSION + "/namespaces/" + namespace_ +
                          "/" + EDGEDEVICE_CRD_PLURAL + "/" + name;
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());

        CURL *curl = curl_easy_init();
        if (!curl) return "";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_cert.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        std::string response;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            std::string* str = reinterpret_cast<std::string*>(userdata);
            size_t total = size * nmemb;
            str->append(ptr, total);
            return total;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) return "";

        try {
            auto j = json::parse(response);
            if (j.contains("spec") && j["spec"].contains("address")) {
                return j["spec"]["address"].get<std::string>();
            }
        } catch (...) {}
        return "";
    }

private:
    std::string ca_cert;
    std::string base_url;
    std::string token;
};

// ========== Configuration Loader ==========

struct APISettings {
    std::map<std::string, std::string> protocolPropertyList;
};

class InstructionConfig {
public:
    bool Load(const std::string& path) {
        try {
            YAML::Node root = YAML::LoadFile(path);
            for (auto it = root.begin(); it != root.end(); ++it) {
                std::string api = it->first.as<std::string>();
                APISettings settings;
                if (it->second["protocolPropertyList"]) {
                    for (auto p = it->second["protocolPropertyList"].begin();
                         p != it->second["protocolPropertyList"].end(); ++p) {
                        settings.protocolPropertyList[p->first.as<std::string>()] =
                            p->second.as<std::string>();
                    }
                }
                api_settings[api] = settings;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    const APISettings* GetSettings(const std::string& api) const {
        auto it = api_settings.find(api);
        if (it != api_settings.end()) return &(it->second);
        return nullptr;
    }

private:
    std::map<std::string, APISettings> api_settings;
};

// ========== MQTT Client Wrapper ==========

class MQTTClientWrapper {
public:
    MQTTClientWrapper(const std::string& broker, const std::string& client_id)
    : cli(broker, client_id), is_connected(false) {}

    bool Connect() {
        mqtt::connect_options connOpts;
        try {
            mqtt::token_ptr conntok = cli.connect(connOpts);
            conntok->wait();
            is_connected = true;
            return true;
        } catch (...) {
            is_connected = false;
            return false;
        }
    }

    void Disconnect() {
        try {
            if (is_connected) {
                cli.disconnect()->wait();
            }
            is_connected = false;
        } catch (...) {}
    }

    bool IsConnected() const {
        return is_connected;
    }

    // Subscribe to a topic and provide a callback
    bool Subscribe(const std::string& topic, int qos,
                   std::function<void(const std::string&)> cb) {
        if (!is_connected) return false;
        try {
            cli.set_callback([cb](mqtt::const_message_ptr msg) {
                cb(msg->to_string());
            });
            cli.subscribe(topic, qos)->wait();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool Publish(const std::string& topic, const std::string& payload, int qos) {
        if (!is_connected) return false;
        try {
            mqtt::message_ptr pubmsg = mqtt::make_message(topic, payload);
            pubmsg->set_qos(qos);
            cli.publish(pubmsg)->wait();
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    mqtt::async_client cli;
    std::atomic<bool> is_connected;
};

// ========== DeviceShifu APIs ==========

class DeviceShifu {
public:
    DeviceShifu()
    : stop_flag(false), phase(EdgeDevicePhase::Pending),
      edge_name(getenv_or_throw("EDGEDEVICE_NAME")),
      edge_ns(getenv_or_throw("EDGEDEVICE_NAMESPACE")),
      mqtt_broker(getenv_or_throw("MQTT_BROKER_ADDRESS")),
      mqtt_client(nullptr),
      config() {}

    ~DeviceShifu() {
        Stop();
    }

    void Start() {
        std::thread([this] { this->MonitorLoop(); }).detach();
    }

    void Stop() {
        stop_flag = true;
        if (mqtt_client) mqtt_client->Disconnect();
    }

    // ========== API Implementations ==========

    // Subscribe APIs
    void Subscribe_Odom(std::function<void(const std::string&)> cb) {
        SubscribeToTopic("device/sensors/odom", "SUBSCRIBE", cb);
    }
    void Subscribe_Imu(std::function<void(const std::string&)> cb) {
        SubscribeToTopic("device/sensors/imu", "SUBSCRIBE", cb);
    }
    void Subscribe_Laser(std::function<void(const std::string&)> cb) {
        SubscribeToTopic("device/sensors/laser", "SUBSCRIBE", cb);
    }

    // Publish APIs
    bool Publish_TTS(const std::string& json_payload) {
        return PublishToTopic("device/commands/tts", "PUBLISH", json_payload);
    }
    bool Publish_CmdVel(const std::string& json_payload) {
        return PublishToTopic("device/commands/cmd_vel", "PUBLISH", json_payload);
    }

    // ========== Status Management ==========

    EdgeDevicePhase GetPhase() const {
        return phase;
    }

private:
    std::atomic<bool> stop_flag;
    std::atomic<EdgeDevicePhase> phase;
    std::string edge_name;
    std::string edge_ns;
    std::string mqtt_broker;
    std::unique_ptr<MQTTClientWrapper> mqtt_client;
    InstructionConfig config;
    K8sClient k8s_client;
    std::mutex mtx;
    std::condition_variable cv;

    static std::string getenv_or_throw(const char* key) {
        const char* val = std::getenv(key);
        if (!val) {
            std::cerr << "Missing required environment variable: " << key << std::endl;
            std::exit(1);
        }
        return std::string(val);
    }

    void MonitorLoop() {
        // Load config
        if (!config.Load("/etc/edgedevice/config/instructions")) {
            std::cerr << "Failed to load instruction config." << std::endl;
        }
        // Get device address (not used for MQTT connection, but required by spec)
        std::string device_address = k8s_client.GetEdgeDeviceAddress(edge_ns, edge_name);
        // MQTT client
        mqtt_client = std::make_unique<MQTTClientWrapper>(mqtt_broker, edge_name + "-shifu");
        while (!stop_flag) {
            bool connected = mqtt_client->Connect();
            EdgeDevicePhase new_phase = connected ? EdgeDevicePhase::Running : EdgeDevicePhase::Failed;
            {
                std::unique_lock<std::mutex> lock(mtx);
                if (phase != new_phase) {
                    phase = new_phase;
                    k8s_client.PatchEdgeDevicePhase(edge_ns, edge_name, phase);
                }
            }
            int sleep_sec = connected ? 30 : 5;
            for (int i = 0; i < sleep_sec && !stop_flag; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        k8s_client.PatchEdgeDevicePhase(edge_ns, edge_name, EdgeDevicePhase::Unknown);
    }

    void SubscribeToTopic(const std::string& topic, const std::string& api, std::function<void(const std::string&)> cb) {
        const APISettings* settings = config.GetSettings(api);
        int qos = 1;
        if (settings) {
            auto it = settings->protocolPropertyList.find("qos");
            if (it != settings->protocolPropertyList.end()) {
                try { qos = std::stoi(it->second); } catch (...) {}
            }
        }
        if (mqtt_client && mqtt_client->IsConnected()) {
            mqtt_client->Subscribe(topic, qos, cb);
        }
    }

    bool PublishToTopic(const std::string& topic, const std::string& api, const std::string& payload) {
        const APISettings* settings = config.GetSettings(api);
        int qos = 1;
        if (settings) {
            auto it = settings->protocolPropertyList.find("qos");
            if (it != settings->protocolPropertyList.end()) {
                try { qos = std::stoi(it->second); } catch (...) {}
            }
        }
        if (mqtt_client && mqtt_client->IsConnected()) {
            return mqtt_client->Publish(topic, payload, qos);
        }
        return false;
    }
};

// ========== Main Entrypoint ==========

static DeviceShifu* global_shifu = nullptr;

void signal_handler(int signum) {
    if (global_shifu) {
        global_shifu->Stop();
    }
    std::_Exit(0);
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    DeviceShifu shifu;
    global_shifu = &shifu;
    shifu.Start();

    // Example usage loop (replace with real application logic)
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    return 0;
}
```
