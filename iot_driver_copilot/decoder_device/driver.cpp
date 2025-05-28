#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <chrono>
#include <condition_variable>
#include <vector>
#include <memory>

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <mqtt/async_client.h>

#include <kubernetes/client.h> // Placeholder: use your actual Kubernetes API client lib

#include <crow.h> // For minimal HTTP server

using json = nlohmann::json;
using namespace std::chrono_literals;

// ----- Logging -----
enum class LogLevel { DEBUG, INFO, WARNING, ERROR };

static LogLevel g_log_level = LogLevel::INFO;

#define LOG_DEBUG(msg)   if (g_log_level <= LogLevel::DEBUG)   std::cout << "[DEBUG] " << msg << std::endl
#define LOG_INFO(msg)    if (g_log_level <= LogLevel::INFO)    std::cout << "[INFO] " << msg << std::endl
#define LOG_WARNING(msg) if (g_log_level <= LogLevel::WARNING) std::cout << "[WARNING] " << msg << std::endl
#define LOG_ERROR(msg)   if (g_log_level <= LogLevel::ERROR)   std::cerr << "[ERROR] " << msg << std::endl

// ----- Utility -----
std::string getenv_or_default(const char* var, const std::string& def) {
    const char* val = std::getenv(var);
    return val ? std::string(val) : def;
}

int getenv_or_default_int(const char* var, int def) {
    const char* val = std::getenv(var);
    if (!val) return def;
    try { return std::stoi(val); }
    catch (...) { return def; }
}

LogLevel parse_log_level(const std::string& lvl) {
    if (lvl == "DEBUG") return LogLevel::DEBUG;
    if (lvl == "INFO") return LogLevel::INFO;
    if (lvl == "WARNING") return LogLevel::WARNING;
    if (lvl == "ERROR") return LogLevel::ERROR;
    return LogLevel::INFO;
}

// ----- ShifuClient -----
class ShifuClient {
public:
    ShifuClient(const std::string& device_name, const std::string& namespace_, const std::string& config_mount_path)
        : device_name_(device_name), namespace_(namespace_), config_mount_path_(config_mount_path) {
        _init_k8s_client();
    }

    void _init_k8s_client() {
        try {
            // Try in-cluster config, fall back to local config
            if (!k8s_client_.load_incluster_config()) {
                LOG_INFO("Falling back to local kubeconfig");
                k8s_client_.load_kube_config();
            }
            initialized_ = true;
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to initialize k8s client: " << e.what());
            initialized_ = false;
        }
    }

    json get_edge_device() {
        if (!initialized_) return json();
        try {
            return k8s_client_.get_namespaced_custom_object("deviceshifu.edge.cerebra.dev", 
                                                           "v1alpha1", namespace_, "edgedevices", device_name_);
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to get EdgeDevice CR: " << e.what());
            return json();
        }
    }

    std::string get_device_address() {
        try {
            auto ed = get_edge_device();
            if (ed.contains("spec") && ed["spec"].contains("address")) {
                return ed["spec"]["address"].get<std::string>();
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to parse device address: " << e.what());
        }
        return "";
    }

    void update_device_status(const std::string& status) {
        try {
            json status_obj = { {"status", { {"deviceHealth", status} }} };
            k8s_client_.patch_namespaced_custom_object_status("deviceshifu.edge.cerebra.dev", 
                "v1alpha1", namespace_, "edgedevices", device_name_, status_obj);
            LOG_INFO("Updated device status to: " << status);
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to update device status: " << e.what());
        }
    }

    std::string read_mounted_config_file(const std::string& filename) {
        std::string path = config_mount_path_ + "/" + filename;
        std::ifstream f(path);
        if (!f) {
            LOG_ERROR("Cannot open config file: " << path);
            return "";
        }
        std::stringstream buffer;
        buffer << f.rdbuf();
        return buffer.str();
    }

    YAML::Node get_instruction_config() {
        try {
            std::string content = read_mounted_config_file("instructions");
            if (content.empty()) return YAML::Node();
            YAML::Node config = YAML::Load(content);
            return config;
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to parse instructions config: " << e.what());
            return YAML::Node();
        }
    }

private:
    std::string device_name_;
    std::string namespace_;
    std::string config_mount_path_;
    K8SClient k8s_client_;
    bool initialized_ = false;
};


// ----- Device Driver Class -----
class DeviceShifuMQTTDriver {
public:
    DeviceShifuMQTTDriver()
        : shutdown_flag_(false), 
          device_name_(getenv_or_default("EDGEDEVICE_NAME", "deviceshifu-networkvideodecoder")),
          namespace_(getenv_or_default("EDGEDEVICE_NAMESPACE", "devices")),
          config_mount_path_(getenv_or_default("CONFIG_MOUNT_PATH", "/etc/edgedevice/config")),
          mqtt_broker_(getenv_or_default("MQTT_BROKER", "")),
          mqtt_broker_port_(getenv_or_default_int("MQTT_BROKER_PORT", 1883)),
          mqtt_user_(getenv_or_default("MQTT_BROKER_USERNAME", "")),
          mqtt_pass_(getenv_or_default("MQTT_BROKER_PASSWORD", "")),
          mqtt_prefix_(getenv_or_default("MQTT_TOPIC_PREFIX", "shifu")),
          http_host_(getenv_or_default("HTTP_HOST", "0.0.0.0")),
          http_port_(getenv_or_default_int("HTTP_PORT", 8080)),
          log_level_(parse_log_level(getenv_or_default("LOG_LEVEL", "INFO"))),
          shifu_client_(device_name_, namespace_, config_mount_path_),
          mqtt_client_(nullptr),
          connection_ok_(false)
    {
        g_log_level = log_level_;
    }

    void run() {
        LOG_INFO("Starting DeviceShifuMQTTDriver for device: " << device_name_);
        if (!parse_instruction_config()) {
            LOG_ERROR("Instruction config missing or invalid. Exiting.");
            return;
        }
        setup_signal_handlers();
        connect_mqtt();
        start_http_server();
        start_scheduled_publishers();

        // Main loop: monitor MQTT connection
        while (!shutdown_flag_) {
            if (!mqtt_client_ || !connection_ok_) {
                LOG_WARNING("MQTT not connected, attempting reconnect...");
                connect_mqtt();
            }
            std::this_thread::sleep_for(2s);
        }

        shutdown();
    }

    void shutdown() {
        LOG_INFO("Shutting down DeviceShifuMQTTDriver...");
        shutdown_flag_ = true;

        try {
            if (mqtt_client_) {
                mqtt_client_->disconnect()->wait();
                mqtt_client_.reset();
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Error disconnecting MQTT: " << e.what());
        }

        // Stop HTTP server
        if (http_thread_.joinable()) {
            crow::stop();
            http_thread_.join();
        }

        // Stop publisher threads
        for (auto& th : publisher_threads_) {
            if (th.joinable()) th.join();
        }

        shifu_client_.update_device_status("Offline");
        LOG_INFO("DeviceShifuMQTTDriver shutdown complete.");
    }

private:
    std::atomic<bool> shutdown_flag_;
    std::string device_name_;
    std::string namespace_;
    std::string config_mount_path_;
    std::string mqtt_broker_;
    int mqtt_broker_port_;
    std::string mqtt_user_;
    std::string mqtt_pass_;
    std::string mqtt_prefix_;
    std::string http_host_;
    int http_port_;
    LogLevel log_level_;

    ShifuClient shifu_client_;

    std::unique_ptr<mqtt::async_client> mqtt_client_;
    std::atomic<bool> connection_ok_;
    std::mutex data_mutex_;
    std::map<std::string, json> latest_data_;

    YAML::Node instruction_config_;
    std::vector<std::thread> publisher_threads_;

    std::thread http_thread_;

    void setup_signal_handlers() {
        std::signal(SIGINT, DeviceShifuMQTTDriver::signal_handler);
        std::signal(SIGTERM, DeviceShifuMQTTDriver::signal_handler);
    }

    static void signal_handler(int signum) {
        LOG_WARNING("Received signal " << signum << ", shutting down...");
        instance()->shutdown_flag_ = true;
    }

    static DeviceShifuMQTTDriver* instance(DeviceShifuMQTTDriver* inst = nullptr) {
        static DeviceShifuMQTTDriver* singleton = nullptr;
        if (inst) singleton = inst;
        return singleton;
    }

    bool parse_instruction_config() {
        instruction_config_ = shifu_client_.get_instruction_config();
        if (!instruction_config_ || !instruction_config_["instructions"]) {
            LOG_ERROR("No instructions found in config.");
            return false;
        }
        return true;
    }

    void connect_mqtt() {
        if (mqtt_broker_.empty()) {
            LOG_ERROR("MQTT_BROKER not specified.");
            return;
        }
        std::string client_id = device_name_ + "-" + std::to_string(std::time(nullptr));
        std::string address = "tcp://" + mqtt_broker_ + ":" + std::to_string(mqtt_broker_port_);
        mqtt_client_ = std::make_unique<mqtt::async_client>(address, client_id);

        auto connOpts = mqtt::connect_options_builder()
            .clean_session()
            .automatic_reconnect(true)
            .finalize();

        if (!mqtt_user_.empty())
            connOpts.set_user_name(mqtt_user_);
        if (!mqtt_pass_.empty())
            connOpts.set_password(mqtt_pass_);

        auto self = this;
        class callback : public virtual mqtt::callback, public virtual mqtt::iaction_listener {
            DeviceShifuMQTTDriver* driver_;
        public:
            callback(DeviceShifuMQTTDriver* driver) : driver_(driver) {}
            void connected(const std::string&) override {
                driver_->connection_ok_ = true;
                driver_->on_connected();
            }
            void connection_lost(const std::string& cause) override {
                driver_->connection_ok_ = false;
                LOG_WARNING("MQTT connection lost: " << cause);
            }
            void message_arrived(mqtt::const_message_ptr msg) override {
                driver_->handle_mqtt_message(msg);
            }
            void delivery_complete(mqtt::delivery_token_ptr) override {}
            void on_failure(const mqtt::token&) override {
                driver_->connection_ok_ = false;
                LOG_ERROR("MQTT connection failed.");
            }
            void on_success(const mqtt::token&) override {}
        };
        mqtt_client_->set_callback(callback(this));

        try {
            mqtt_client_->connect(connOpts)->wait();
            connection_ok_ = true;
            LOG_INFO("Connected to MQTT broker at " << address);
            shifu_client_.update_device_status("Online");
        } catch (const std::exception& e) {
            connection_ok_ = false;
            LOG_ERROR("MQTT connect failed: " << e.what());
        }
    }

    void on_connected() {
        // Subscribe to control commands
        std::string ctrl_topic = mqtt_prefix_ + "/" + device_name_ + "/control/#";
        try {
            mqtt_client_->subscribe(ctrl_topic, 1)->wait();
            LOG_INFO("Subscribed to: " << ctrl_topic);
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to subscribe control topic: " << e.what());
        }
        // Subscribe to device telemetry (if necessary)
        for (const auto& instr : instruction_config_["instructions"]) {
            if (instr.second["protocol"].as<std::string>() == "MQTT" &&
                instr.second["mode"].as<std::string>() == "subscriber") {
                std::string topic = mqtt_prefix_ + "/" + device_name_ + "/" + instr.first.as<std::string>();
                int qos = instr.second["qos"] ? instr.second["qos"].as<int>() : 1;
                try {
                    mqtt_client_->subscribe(topic, qos)->wait();
                    LOG_INFO("Subscribed to: " << topic);
                } catch (const std::exception& e) {
                    LOG_ERROR("Failed to subscribe topic: " << e.what());
                }
            }
        }
    }

    void handle_mqtt_message(mqtt::const_message_ptr msg) {
        try {
            LOG_DEBUG("MQTT message arrived: " << msg->get_topic() << " payload: " << msg->to_string());
            // Route to appropriate handler
            std::string topic = msg->get_topic();
            std::string payload = msg->to_string();

            // Handle control commands
            if (topic.find("/control/") != std::string::npos) {
                json cmd = json::parse(payload, nullptr, false);
                if (!cmd.is_discarded()) {
                    // Example: handle control command
                    // TODO: Send command to device via SDK, react accordingly
                    LOG_INFO("Received control command: " << cmd.dump());
                    // For demo, echo status
                    publish_status("Received command: " + cmd.dump());
                }
            }
            // Handle telemetry subscriptions
            else if (topic.find("/telemetry/") != std::string::npos) {
                std::lock_guard<std::mutex> lk(data_mutex_);
                latest_data_[topic] = json::parse(payload, nullptr, false);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in handle_mqtt_message: " << e.what());
        }
    }

    void start_http_server() {
        auto self = this;
        http_thread_ = std::thread([this, self](){
            crow::SimpleApp app;
            CROW_ROUTE(app, "/health")([self](){
                return crow::response(200, R"({"status":"ok"})");
            });
            CROW_ROUTE(app, "/status")([self](){
                json j;
                j["device_name"] = self->device_name_;
                j["connected"] = self->connection_ok_;
                {
                    std::lock_guard<std::mutex> lk(self->data_mutex_);
                    j["latest_data"] = self->latest_data_;
                }
                return crow::response(200, j.dump());
            });
            app.bindaddr(http_host_).port(http_port_).run();
        });
    }

    void publish_status(const std::string& msg) {
        json j;
        j["status"] = msg;
        publish_mqtt(mqtt_prefix_ + "/" + device_name_ + "/status", j, 1);
    }

    void publish_mqtt(const std::string& topic, const json& payload, int qos = 1) {
        if (!connection_ok_ || !mqtt_client_) return;
        try {
            auto msg = mqtt::make_message(topic, payload.dump());
            msg->set_qos(qos);
            mqtt_client_->publish(msg);
            LOG_DEBUG("Published to " << topic << ": " << payload.dump());
        } catch (const std::exception& e) {
            LOG_ERROR("MQTT publish error: " << e.what());
        }
    }

    void start_scheduled_publishers() {
        for (const auto& inst : instruction_config_["instructions"]) {
            std::string name = inst.first.as<std::string>();
            const YAML::Node& props = inst.second;
            std::string mode = props["mode"] ? props["mode"].as<std::string>() : "";
            int interval = props["publishIntervalMS"] ? props["publishIntervalMS"].as<int>() : 0;
            std::string protocol = props["protocol"] ? props["protocol"].as<std::string>() : "";
            if (mode == "publisher" && protocol == "MQTT" && interval > 0) {
                publisher_threads_.emplace_back(&DeviceShifuMQTTDriver::publish_topic_periodically, this, name, interval, props);
                publisher_threads_.back().detach();
            }
        }
    }

    void publish_topic_periodically(std::string instruction, int interval_ms, YAML::Node props) {
        std::string topic = mqtt_prefix_ + "/" + device_name_ + "/" + instruction;
        int qos = props["qos"] ? props["qos"].as<int>() : 1;
        while (!shutdown_flag_) {
            try {
                json data = get_device_data_for_instruction(instruction, props);
                {
                    std::lock_guard<std::mutex> lk(data_mutex_);
                    latest_data_[topic] = data;
                }
                publish_mqtt(topic, data, qos);
            } catch (const std::exception& e) {
                LOG_ERROR("Error in periodic publisher for " << instruction << ": " << e.what());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }

    json get_device_data_for_instruction(const std::string& instruction, const YAML::Node& props) {
        // TODO: Integrate with actual device SDK (HCNetSDK) or RTSP to retrieve real data.
        // For now, return dummy data for demo.
        json data;
        if (instruction == "status" || instruction.find("telemetry") != std::string::npos) {
            data["device_status"] = "online";
            data["channels"] = { { "id", 1 }, { "status", "active" } };
            data["alarm"] = false;
            data["upgrade_progress"] = 100;
            data["timestamp"] = std::time(nullptr);
        } else if (instruction == "commands" || instruction.find("control") != std::string::npos) {
            data["result"] = "success";
            data["timestamp"] = std::time(nullptr);
        } else {
            data["data"] = "unknown";
            data["timestamp"] = std::time(nullptr);
        }
        return data;
    }
};


// ---- Main Entrypoint ----
int main() {
    auto driver = std::make_unique<DeviceShifuMQTTDriver>();
    DeviceShifuMQTTDriver::instance(driver.get());
    driver->run();
    return 0;
}