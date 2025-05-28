#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <functional>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include <paho.mqtt.cpp/async_client.h>
#include <kubernetes/client.h>
#include <httplib.h>

using json = nlohmann::json;

// Logging utility
enum LogLevel {DEBUG, INFO, WARNING, ERROR};
static LogLevel CURRENT_LOG_LEVEL = INFO;

#define LOG(level, msg) \
    if (level >= CURRENT_LOG_LEVEL) { \
        std::cerr << "[" << #level << "] " << msg << std::endl; \
    }

// Utility to get env vars with default
std::string getenvOrDefault(const std::string& key, const std::string& def) {
    const char* val = std::getenv(key.c_str());
    if (!val) return def;
    return std::string(val);
}

// --- ShifuClient ---

class ShifuClient {
public:
    ShifuClient() {
        try {
            init_k8s_client();
        } catch (const std::exception& e) {
            LOG(ERROR, "Failed to initialize kubernetes client: " << e.what());
            throw;
        }
        edge_device_name_ = getenvOrDefault("EDGEDEVICE_NAME", "deviceshifu-NetworkVideoDecoder");
        edge_device_namespace_ = getenvOrDefault("EDGEDEVICE_NAMESPACE", "devices");
        config_mount_path_ = getenvOrDefault("CONFIG_MOUNT_PATH", "/etc/edgedevice/config");
    }

    // Load mounted config file (instructions, driverProperties)
    std::string read_mounted_config_file(const std::string& filename) {
        std::string path = config_mount_path_ + "/" + filename;
        std::ifstream f(path);
        if (!f) {
            LOG(ERROR, "Failed to open config file: " << path);
            return "";
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // Parse instructions YAML from file into map
    YAML::Node get_instruction_config() {
        std::string content = read_mounted_config_file("instructions");
        if (content.empty()) {
            LOG(WARNING, "Instructions config is empty");
            return YAML::Node();
        }
        try {
            YAML::Node node = YAML::Load(content);
            return node;
        } catch (const std::exception& e) {
            LOG(ERROR, "YAML parse failed: " << e.what());
            return YAML::Node();
        }
    }

    // Device address from EdgeDevice CR
    std::string get_device_address() {
        try {
            auto dev = get_edge_device();
            if (dev.contains("spec") && dev["spec"].contains("address")) {
                return dev["spec"]["address"];
            }
        } catch (const std::exception& e) {
            LOG(ERROR, "Failed to get device address: " << e.what());
        }
        return "";
    }

    // Get EdgeDevice object from k8s
    json get_edge_device() {
        try {
            std::string result = k8s_client_->getNamespacedCustomObject(
                "devices.kubeedge.io", "v1alpha2", edge_device_namespace_, "edgedevices", edge_device_name_
            );
            return json::parse(result);
        } catch (const std::exception& e) {
            LOG(ERROR, "K8S get_edge_device failed: " << e.what());
            throw;
        }
    }

    // Update EdgeDevice status
    void update_device_status(const std::string& status, const std::string& msg = "") {
        try {
            auto dev = get_edge_device();
            dev["status"]["devicePhase"] = status;
            if (!msg.empty())
                dev["status"]["message"] = msg;
            std::string body = dev.dump();
            k8s_client_->replaceNamespacedCustomObjectStatus(
                "devices.kubeedge.io", "v1alpha2", edge_device_namespace_, "edgedevices", edge_device_name_, body
            );
        } catch (const std::exception& e) {
            LOG(ERROR, "Failed to update device status: " << e.what());
        }
    }

private:
    void init_k8s_client() {
        try {
            // Try inCluster, then fallback to local kubeconfig
            std::string kubeconfig = getenvOrDefault("KUBECONFIG", "");
            if (!kubeconfig.empty()) {
                k8s_client_ = std::make_unique<kubernetes::Client>(kubeconfig);
            } else {
                k8s_client_ = std::make_unique<kubernetes::Client>();
            }
        } catch (const std::exception& e) {
            LOG(ERROR, "K8S client init error: " << e.what());
            throw;
        }
    }

    std::string edge_device_name_;
    std::string edge_device_namespace_;
    std::string config_mount_path_;
    std::unique_ptr<kubernetes::Client> k8s_client_;
};

// --- Device Driver ---

class DeviceShifuDriver {
public:
    DeviceShifuDriver() :
        shutdown_flag(false),
        latest_data_mutex(),
        latest_data(),
        shifu_client(),
        mqtt_connected(false),
        mqtt_reconnect_delay(1000),
        mqtt_client(nullptr)
    {
        parse_env();
        parse_config();
        client_id = device_name + "-" + std::to_string(std::time(nullptr));
    }

    ~DeviceShifuDriver() {
        shutdown();
    }

    void run() {
        LOG(INFO, "Driver starting...");
        shifu_client.update_device_status("Online");

        // Start HTTP server in background
        http_thread = std::thread(&DeviceShifuDriver::start_http_server, this);

        // Start MQTT client & connection monitor
        mqtt_thread = std::thread(&DeviceShifuDriver::mqtt_worker, this);

        // Start periodic publishers per config
        start_scheduled_publishers();

        // Signal handling
        std::signal(SIGINT, DeviceShifuDriver::signal_handler);
        std::signal(SIGTERM, DeviceShifuDriver::signal_handler);

        // Wait for threads to finish
        http_thread.join();
        mqtt_thread.join();
        for (auto& th : publisher_threads) {
            if (th.joinable()) th.join();
        }
        LOG(INFO, "Driver stopped.");
    }

    static void signal_handler(int) {
        LOG(INFO, "Shutdown signal received");
        instance()->shutdown_flag = true;
        instance()->shutdown();
    }

    void shutdown() {
        if (shutdown_flag.exchange(true)) return;
        LOG(INFO, "Shutting down...");
        try {
            if (mqtt_client && mqtt_connected) {
                mqtt_client->disconnect()->wait();
            }
        } catch (...) {}
        if (http_server) http_server->stop();
    }

    // --- MQTT ---

    void mqtt_worker() {
        while (!shutdown_flag) {
            try {
                connect_mqtt();
                while (mqtt_connected && !shutdown_flag) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            } catch (const std::exception& e) {
                LOG(ERROR, "MQTT worker: " << e.what());
            }
            if (!shutdown_flag) {
                LOG(WARNING, "MQTT connection lost. Reconnecting in " << mqtt_reconnect_delay << "ms");
                std::this_thread::sleep_for(std::chrono::milliseconds(mqtt_reconnect_delay));
            }
        }
    }

    void connect_mqtt() {
        std::string address = "tcp://" + mqtt_broker + ":" + std::to_string(mqtt_port);

        mqtt_client = std::make_unique<mqtt::async_client>(address, client_id);

        mqtt::connect_options connOpts;
        if (!mqtt_user.empty()) {
            connOpts.set_user_name(mqtt_user);
            connOpts.set_password(mqtt_pass);
        }
        connOpts.set_clean_session(true);

        mqtt_client->set_connected_handler([this](const std::string&) {
            LOG(INFO, "MQTT connected");
            mqtt_connected = true;
            // Subscribe to control/command topics
            for (const auto& sub : mqtt_subscribe_topics) {
                mqtt_client->subscribe(sub, 1)->wait();
                LOG(INFO, "Subscribed: " << sub);
            }
            // Status reporting
            publish_status("online");
        });

        mqtt_client->set_connection_lost_handler([this](const std::string&) {
            LOG(WARNING, "MQTT connection lost");
            mqtt_connected = false;
            publish_status("offline");
        });

        mqtt_client->set_message_callback([this](mqtt::const_message_ptr msg) {
            handle_mqtt_message(msg);
        });

        try {
            mqtt_client->connect(connOpts)->wait();
        } catch (const std::exception& e) {
            LOG(ERROR, "MQTT connect failed: " << e.what());
            mqtt_connected = false;
            throw;
        }
    }

    void publish_status(const std::string& status) {
        if (!mqtt_connected) return;
        std::string topic = topic_prefix + "/" + device_name + "/status";
        json payload = {
            {"status", status},
            {"timestamp", std::time(nullptr)}
        };
        auto msg = mqtt::make_message(topic, payload.dump());
        msg->set_qos(1);
        mqtt_client->publish(msg);
    }

    void handle_mqtt_message(mqtt::const_message_ptr msg) {
        LOG(INFO, "MQTT message received: " << msg->get_topic());
        // Handle control/maintenance/display/decode commands here
        // For demo, just log and echo to latest_data
        std::lock_guard<std::mutex> lock(latest_data_mutex);
        latest_data[msg->get_topic()] = json::parse(msg->to_string());
    }

    void publish_topic(const std::string& topic, const json& payload, int qos=1) {
        if (!mqtt_connected) return;
        try {
            auto msg = mqtt::make_message(topic, payload.dump());
            msg->set_qos(qos);
            mqtt_client->publish(msg);
        } catch (const std::exception& e) {
            LOG(ERROR, "MQTT publish failed: " << e.what());
        }
    }

    // --- HTTP Server ---

    void start_http_server() {
        http_server = std::make_unique<httplib::Server>();

        http_server->Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("{\"health\": \"ok\"}", "application/json");
        });

        http_server->Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            json status_json;
            status_json["mqtt_connected"] = this->mqtt_connected;
            std::lock_guard<std::mutex> lock(latest_data_mutex);
            status_json["latest_data"] = latest_data;
            res.set_content(status_json.dump(), "application/json");
        });

        LOG(INFO, "HTTP server listening on " << http_host << ":" << http_port);
        http_server->listen(http_host.c_str(), http_port);
    }

    // --- Periodic Publisher ---

    void start_scheduled_publishers() {
        for (const auto& instr : instruction_publishers) {
            publisher_threads.emplace_back(
                &DeviceShifuDriver::publish_topic_periodically,
                this, instr.first, instr.second.interval_ms, instr.second.topic, instr.second.qos
            );
        }
    }

    void publish_topic_periodically(std::string instruction, int interval_ms, std::string topic, int qos) {
        while (!shutdown_flag) {
            // Simulate device data retrieval
            json payload = retrieve_device_data(instruction);
            {
                std::lock_guard<std::mutex> lock(latest_data_mutex);
                latest_data[topic] = payload;
            }
            publish_topic(topic, payload, qos);
            for (int i = 0; i < interval_ms / 100; ++i) {
                if (shutdown_flag) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    json retrieve_device_data(const std::string& instruction) {
        // Replace this with actual device SDK/logic
        json data;
        data["instruction"] = instruction;
        data["timestamp"] = std::time(nullptr);
        data["status"] = "OK";
        return data;
    }

    // --- Config Parsing ---

    void parse_env() {
        device_name = getenvOrDefault("EDGEDEVICE_NAME", "deviceshifu-NetworkVideoDecoder");
        device_ns = getenvOrDefault("EDGEDEVICE_NAMESPACE", "devices");
        config_mount_path = getenvOrDefault("CONFIG_MOUNT_PATH", "/etc/edgedevice/config");
        mqtt_broker = getenvOrDefault("MQTT_BROKER", "localhost");
        mqtt_port = std::stoi(getenvOrDefault("MQTT_BROKER_PORT", "1883"));
        mqtt_user = getenvOrDefault("MQTT_BROKER_USERNAME", "");
        mqtt_pass = getenvOrDefault("MQTT_BROKER_PASSWORD", "");
        topic_prefix = getenvOrDefault("MQTT_TOPIC_PREFIX", "shifu");
        http_host = getenvOrDefault("HTTP_HOST", "0.0.0.0");
        http_port = std::stoi(getenvOrDefault("HTTP_PORT", "8080"));
        std::string log_level = getenvOrDefault("LOG_LEVEL", "INFO");
        if (log_level == "DEBUG") CURRENT_LOG_LEVEL = DEBUG;
        else if (log_level == "INFO") CURRENT_LOG_LEVEL = INFO;
        else if (log_level == "WARNING") CURRENT_LOG_LEVEL = WARNING;
        else if (log_level == "ERROR") CURRENT_LOG_LEVEL = ERROR;
    }

    void parse_config() {
        YAML::Node instructions = shifu_client.get_instruction_config();
        // Fallback: if no config, use API info
        if (!instructions) {
            // Hardcoded from API info
            add_publisher("maintenance", "device/commands/maintenance", 10000, 1);
            add_publisher("display", "device/commands/display", 10000, 1);
            add_publisher("decode", "device/commands/decode", 10000, 1);
            add_subscriber("device/telemetry/status", 1);
        } else {
            // Parse instructions for detailed config
            for (const auto& it : instructions) {
                std::string name = it.first.as<std::string>();
                YAML::Node desc = it.second;
                std::string mode = desc["protocolProperties"]["mode"].as<std::string>("publisher");
                int interval = desc["protocolProperties"]["publishIntervalMS"].as<int>(10000);
                std::string topic = desc["mqttTopic"].as<std::string>(topic_prefix + "/" + device_name + "/" + name);
                int qos = desc["protocolProperties"]["qos"].as<int>(1);
                if (mode == "publisher") {
                    add_publisher(name, topic, interval, qos);
                } else if (mode == "subscriber") {
                    add_subscriber(topic, qos);
                }
            }
        }
    }

    void add_publisher(const std::string& name, const std::string& topic, int interval_ms, int qos) {
        instruction_publishers[name] = {topic, interval_ms, qos};
    }

    void add_subscriber(const std::string& topic, int qos) {
        mqtt_subscribe_topics.push_back(topic);
    }

    // --- Singleton for signal handling ---
    static DeviceShifuDriver* instance(DeviceShifuDriver* inst = nullptr) {
        static DeviceShifuDriver* driver_instance = nullptr;
        if (inst) driver_instance = inst;
        return driver_instance;
    }

private:
    struct PublisherConfig {
        std::string topic;
        int interval_ms;
        int qos;
    };

    std::string device_name, device_ns, config_mount_path, mqtt_broker, mqtt_user, mqtt_pass, topic_prefix, http_host, client_id;
    int mqtt_port, http_port;
    std::atomic<bool> shutdown_flag;

    std::mutex latest_data_mutex;
    std::unordered_map<std::string, json> latest_data;

    ShifuClient shifu_client;

    std::vector<std::thread> publisher_threads;
    std::thread http_thread, mqtt_thread;

    std::vector<std::string> mqtt_subscribe_topics;
    std::map<std::string, PublisherConfig> instruction_publishers;

    std::unique_ptr<mqtt::async_client> mqtt_client;
    std::atomic<bool> mqtt_connected;
    int mqtt_reconnect_delay;
    std::unique_ptr<httplib::Server> http_server;
};

// --- Main ---

int main() {
    try {
        auto driver = std::make_unique<DeviceShifuDriver>();
        DeviceShifuDriver::instance(driver.get());
        driver->run();
    } catch (const std::exception& e) {
        LOG(ERROR, "Fatal error: " << e.what());
        return 1;
    }
    return 0;
}