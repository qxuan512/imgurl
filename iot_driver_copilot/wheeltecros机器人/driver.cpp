#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <map>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include "mqtt/async_client.h"

// --- Configuration Constants ---
#define INSTRUCTION_PATH "/etc/edgedevice/config/instructions"
#define EDGEDEVICE_CRD_API_VERSION "shifu.edgenesis.io/v1alpha1"
#define EDGEDEVICE_CRD_PLURAL "edgedevices"
#define EDGEDEVICE_CRD_KIND "EdgeDevice"
#define EDGEDEVICE_CRD_GROUP "shifu.edgenesis.io"
#define EDGEDEVICE_CRD_STATUS_PATH "/status"
#define KUBE_TOKEN_PATH "/var/run/secrets/kubernetes.io/serviceaccount/token"
#define KUBE_CA_PATH "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt"
#define KUBE_API_HOST_ENV "KUBERNETES_SERVICE_HOST"
#define KUBE_API_PORT_ENV "KUBERNETES_SERVICE_PORT"
#define KUBE_API_PREFIX "/apis/" EDGEDEVICE_CRD_GROUP "/" EDGEDEVICE_CRD_API_VERSION "/namespaces/"

// --- Utilities ---
std::string get_env(const std::string& var) {
    const char* val = std::getenv(var.c_str());
    if (!val) {
        std::cerr << "Missing environment variable: " << var << std::endl;
        exit(EXIT_FAILURE);
    }
    return std::string(val);
}

// --- Kubernetes CRD Client ---
class KubeClient {
public:
    KubeClient() {
        kube_host = get_env(KUBE_API_HOST_ENV);
        kube_port = get_env(KUBE_API_PORT_ENV);
        std::ifstream token_file(KUBE_TOKEN_PATH);
        std::getline(token_file, kube_token, '\0');
        curl_global_init(CURL_GLOBAL_ALL);
    }

    ~KubeClient() {
        curl_global_cleanup();
    }

    nlohmann::json get_edgedevice(const std::string& ns, const std::string& name) {
        std::string url = "https://" + kube_host + ":" + kube_port +
            KUBE_API_PREFIX + ns + "/" + EDGEDEVICE_CRD_PLURAL + "/" + name;
        return http_get(url);
    }

    void patch_edgedevice_status(const std::string& ns, const std::string& name, const std::string& phase) {
        std::string url = "https://" + kube_host + ":" + kube_port +
            KUBE_API_PREFIX + ns + "/" + EDGEDEVICE_CRD_PLURAL + "/" + name + EDGEDEVICE_CRD_STATUS_PATH;
        nlohmann::json status_obj;
        status_obj["status"]["edgeDevicePhase"] = phase;
        std::string patch = status_obj.dump();
        http_patch(url, patch);
    }

private:
    std::string kube_host, kube_port, kube_token;

    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    nlohmann::json http_get(const std::string& url) {
        CURL* curl = curl_easy_init();
        std::string response, header_auth = "Authorization: Bearer " + kube_token;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, header_auth.c_str());
        headers = curl_slist_append(headers, "Accept: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, KUBE_CA_PATH);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "Failed to GET from kube api: " << url << std::endl;
            return {};
        }
        return nlohmann::json::parse(response);
    }

    void http_patch(const std::string& url, const std::string& patch) {
        CURL* curl = curl_easy_init();
        std::string response, header_auth = "Authorization: Bearer " + kube_token;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, header_auth.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/merge-patch+json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, KUBE_CA_PATH);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, patch.c_str());

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "Failed to PATCH kube api: " << url << std::endl;
        }
    }
};

// --- Instruction Loader ---
struct ApiInstruction {
    std::map<std::string, std::string> protocolPropertyList;
};

class InstructionLoader {
public:
    InstructionLoader(const std::string& path) {
        YAML::Node config = YAML::LoadFile(path);
        for (YAML::const_iterator it = config.begin(); it != config.end(); ++it) {
            std::string api = it->first.as<std::string>();
            const YAML::Node& props = it->second["protocolPropertyList"];
            ApiInstruction instr;
            for (YAML::const_iterator pit = props.begin(); pit != props.end(); ++pit) {
                instr.protocolPropertyList[pit->first.as<std::string>()] = pit->second.as<std::string>();
            }
            api_map[api] = instr;
        }
    }
    const ApiInstruction* get_api(const std::string& name) const {
        auto it = api_map.find(name);
        if (it != api_map.end()) return &it->second;
        return nullptr;
    }
private:
    std::map<std::string, ApiInstruction> api_map;
};

// --- MQTT Client Wrapper ---
class MQTTClientWrapper {
public:
    MQTTClientWrapper(const std::string& address, const std::string& client_id)
    : cli(address, client_id), connected(false) {}

    bool connect() {
        mqtt::connect_options connOpts;
        try {
            mqtt::token_ptr conntok = cli.connect(connOpts);
            conntok->wait();
            connected = true;
            return true;
        } catch (const mqtt::exception& exc) {
            connected = false;
            return false;
        }
    }

    void disconnect() {
        try { cli.disconnect()->wait(); } catch (...) {}
        connected = false;
    }

    bool is_connected() const { return connected; }

    void subscribe(const std::string& topic, int qos,
        std::function<void(const std::string&)> cb) {
        cli.set_callback([cb](mqtt::const_message_ptr msg) {
            cb(msg->to_string());
        });
        cli.subscribe(topic, qos)->wait();
    }

    void publish(const std::string& topic, const std::string& payload, int qos) {
        cli.publish(topic, payload.c_str(), payload.size(), qos, false);
    }

private:
    mqtt::async_client cli;
    std::atomic<bool> connected;
};

// --- DeviceShifu Main Class ---
class DeviceShifu {
public:
    DeviceShifu()
        : ns(get_env("EDGEDEVICE_NAMESPACE")),
          name(get_env("EDGEDEVICE_NAME")),
          mqtt_broker(get_env("MQTT_BROKER_ADDRESS")),
          kube_client(),
          instructions(INSTRUCTION_PATH),
          mqtt_client(mqtt_broker, "deviceshifu-" + name),
          terminate(false)
    {
        fetch_device_address();
    }

    void run() {
        std::thread status_thread([this] { monitor_status(); });

        // Start MQTT connection and update status
        if (mqtt_client.connect()) {
            kube_client.patch_edgedevice_status(ns, name, "Running");
        } else {
            kube_client.patch_edgedevice_status(ns, name, "Failed");
        }

        // API: SUBSCRIBE to device/sensors/odom
        std::thread odom_thread([this]() {
            mqtt_client.subscribe("device/sensors/odom", 1, [this](const std::string& payload) {
                handle_odom(payload);
            });
        });

        // API: SUBSCRIBE to device/sensors/imu
        std::thread imu_thread([this]() {
            mqtt_client.subscribe("device/sensors/imu", 1, [this](const std::string& payload) {
                handle_imu(payload);
            });
        });

        // API: SUBSCRIBE to device/sensors/laser
        std::thread laser_thread([this]() {
            mqtt_client.subscribe("device/sensors/laser", 1, [this](const std::string& payload) {
                handle_laser(payload);
            });
        });

        // API: PUBLISH to device/commands/tts
        std::thread tts_thread([this]() {
            handle_tts_publish();
        });

        // API: PUBLISH to device/commands/cmd_vel
        std::thread cmdvel_thread([this]() {
            handle_cmdvel_publish();
        });

        // Wait for signal to terminate
        wait_for_exit();

        terminate = true;
        mqtt_client.disconnect();
        status_thread.join();
        odom_thread.detach();
        imu_thread.detach();
        laser_thread.detach();
        tts_thread.detach();
        cmdvel_thread.detach();
    }

private:
    std::string ns, name, mqtt_broker, device_address;
    KubeClient kube_client;
    InstructionLoader instructions;
    MQTTClientWrapper mqtt_client;
    std::atomic<bool> terminate;
    std::mutex data_mtx;
    std::condition_variable data_cv;
    std::vector<std::string> odom_data, imu_data, laser_data;

    void fetch_device_address() {
        nlohmann::json edgedevice = kube_client.get_edgedevice(ns, name);
        if (edgedevice.contains("spec") && edgedevice["spec"].contains("address"))
            device_address = edgedevice["spec"]["address"];
        else
            device_address = "";
    }

    void monitor_status() {
        std::string last_phase = "Unknown";
        while (!terminate) {
            std::string phase = "Unknown";
            if (!mqtt_client.is_connected()) {
                phase = "Pending";
                if (!mqtt_client.connect())
                    phase = "Failed";
                else
                    phase = "Running";
            } else {
                phase = "Running";
            }
            if (phase != last_phase) {
                kube_client.patch_edgedevice_status(ns, name, phase);
                last_phase = phase;
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        // On shutdown, set to Unknown
        kube_client.patch_edgedevice_status(ns, name, "Unknown");
    }

    void handle_odom(const std::string& payload) {
        std::lock_guard<std::mutex> lk(data_mtx);
        odom_data.push_back(payload);
        // Data is available for user to use (e.g. process, store, etc.)
    }

    void handle_imu(const std::string& payload) {
        std::lock_guard<std::mutex> lk(data_mtx);
        imu_data.push_back(payload);
        // Data is available for user to use
    }

    void handle_laser(const std::string& payload) {
        std::lock_guard<std::mutex> lk(data_mtx);
        laser_data.push_back(payload);
        // Data is available for user to use
    }

    void handle_tts_publish() {
        while (!terminate) {
            std::string tts_json;
            // The real DeviceShifu would get input from REST/gRPC/etc, here we mock
            std::getline(std::cin, tts_json);
            if (!tts_json.empty()) {
                mqtt_client.publish("device/commands/tts", tts_json, 1);
            }
        }
    }

    void handle_cmdvel_publish() {
        while (!terminate) {
            std::string cmdvel_json;
            // The real DeviceShifu would get input from REST/gRPC/etc, here we mock
            std::getline(std::cin, cmdvel_json);
            if (!cmdvel_json.empty()) {
                mqtt_client.publish("device/commands/cmd_vel", cmdvel_json, 1);
            }
        }
    }

    void wait_for_exit() {
        static std::atomic<bool> signaled{false};
        auto signal_handler = [](int) { signaled = true; };
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        while (!signaled) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
};

int main() {
    DeviceShifu shifu;
    shifu.run();
    return 0;
}