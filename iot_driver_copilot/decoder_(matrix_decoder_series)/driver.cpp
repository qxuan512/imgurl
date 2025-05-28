#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <functional>
#include <condition_variable>

// External Libraries
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include <paho.mqtt.cpp/async_client.h>

// Kubernetes
#include <kubernetes/client/client.h> // Placeholder: use official or supported k8s C++ client

// HTTP server: use Pistache for simplicity
#include <pistache/endpoint.h>
#include <pistache/router.h>
#include <pistache/http.h>

// Logging
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

using json = nlohmann::json;
using namespace std::chrono_literals;

using std::string;
using std::map;
using std::vector;
using std::thread;
using std::mutex;
using std::lock_guard;
using std::unique_lock;
using std::condition_variable;

using std::atomic_bool;
using std::atomic_int;

//////////////////////////////////////////
// Utility Functions
//////////////////////////////////////////

inline string getenv_or_default(const string &var, const string &def) {
    const char *val = std::getenv(var.c_str());
    return val ? string(val) : def;
}

inline int getenv_or_default_int(const string &var, int def) {
    const char *val = std::getenv(var.c_str());
    return val ? std::stoi(val) : def;
}

//////////////////////////////////////////
// ShifuClient: Handles K8s interaction
//////////////////////////////////////////

class ShifuClient {
public:
    ShifuClient(
        const string &device_name,
        const string &namespace_,
        const string &config_mount_path,
        std::shared_ptr<spdlog::logger> logger)
        : device_name_(device_name),
          namespace_(namespace_),
          config_mount_path_(config_mount_path),
          logger_(logger) {
        _init_k8s_client();
    }

    void _init_k8s_client() {
        try {
            // Initialize k8s client, in-cluster or local kubeconfig
            // Placeholder: Use kubernetes::client::Client from cpp-k8s-client
            // If in cluster: use default, else fall back to KUBECONFIG
            logger_->info("Initializing Kubernetes client...");
            // k8s_client_ = std::make_shared<kubernetes::client::Client>();
            // Actual implementation depends on library
        } catch (const std::exception &e) {
            logger_->error("Failed to initialize Kubernetes client: {}", e.what());
            // Continue as read-only driver if no client
        }
    }

    json get_edge_device() {
        // Placeholder: fetch EdgeDevice CR
        try {
            // auto resource = k8s_client_->get_edgedevice(device_name_, namespace_);
            // return resource;
            return json(); // Return empty for now
        } catch (const std::exception &e) {
            logger_->warn("get_edge_device: {}", e.what());
            return json();
        }
    }

    string get_device_address() {
        // Placeholder: extract address from EdgeDevice CR status
        try {
            json edge_device = get_edge_device();
            if (!edge_device.is_null() && edge_device.contains("status") && edge_device["status"].contains("address")) {
                return edge_device["status"]["address"];
            }
        } catch (const std::exception &e) {
            logger_->warn("get_device_address: {}", e.what());
        }
        return "";
    }

    void update_device_status(const string &status) {
        // Placeholder: PATCH or UPDATE EdgeDevice status subresource
        try {
            logger_->info("Updating device status in K8s: {}", status);
            // k8s_client_->update_edgedevice_status(device_name_, namespace_, status);
        } catch (const std::exception &e) {
            logger_->warn("update_device_status: {}", e.what());
        }
    }

    string read_mounted_config_file(const string &filename) {
        string path = config_mount_path_ + "/" + filename;
        std::ifstream file(path);
        if (!file) {
            logger_->warn("Cannot open config file: {}", path);
            return "";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    YAML::Node get_instruction_config() {
        string content = read_mounted_config_file("instructions");
        if (content.empty()) {
            logger_->warn("Instruction config is empty.");
            return YAML::Node();
        }
        try {
            return YAML::Load(content);
        } catch (const std::exception &e) {
            logger_->error("YAML parsing error: {}", e.what());
            return YAML::Node();
        }
    }

private:
    string device_name_;
    string namespace_;
    string config_mount_path_;
    std::shared_ptr<spdlog::logger> logger_;
    // std::shared_ptr<kubernetes::client::Client> k8s_client_;
};

//////////////////////////////////////////
// Device SDK Abstraction (Stub)
//////////////////////////////////////////

class DecoderSDK {
public:
    explicit DecoderSDK(std::shared_ptr<spdlog::logger> logger)
        : connected_(false), logger_(logger) {}

    bool connect(const string &ip, int port, const string &username, const string &password) {
        // Simulate device login
        logger_->info("Connecting to decoder device at {}:{}...", ip, port);
        std::this_thread::sleep_for(500ms);
        connected_ = true;
        logger_->info("Decoder device connected.");
        return true;
    }
    void disconnect() {
        logger_->info("Disconnecting from decoder device...");
        connected_ = false;
    }
    bool is_connected() const { return connected_; }

    // Simulated SDK commands (replace with actual SDK calls)
    json get_status() {
        // Return dummy device status
        json j = {
            {"deviceStatus", "online"},
            {"channels", {
                {"channel1", {{"status", "active"}}},
                {"channel2", {{"status", "inactive"}}}
            }},
            {"alarms", {{"code", 0}, {"description", "none"}}}
        };
        return j;
    }

    json decode_control(const json &payload) {
        // Simulate decode start/stop
        string action = payload.value("action", "");
        logger_->info("DecoderSDK decode_control action: {}", action);
        if (action == "start") {
            return {{"result", "started"}};
        } else if (action == "stop") {
            return {{"result", "stopped"}};
        }
        return {{"result", "unknown_action"}};
    }

    json config_update(const json &payload) {
        logger_->info("DecoderSDK config_update: {}", payload.dump());
        return {{"result", "ok"}};
    }

    json login(const json &payload) {
        logger_->info("DecoderSDK login: {}", payload.dump());
        return {{"result", "login_success"}};
    }

    json logout(const json &payload) {
        logger_->info("DecoderSDK logout: {}", payload.dump());
        return {{"result", "logout_success"}};
    }

private:
    bool connected_;
    std::shared_ptr<spdlog::logger> logger_;
};

//////////////////////////////////////////
// Main DeviceShifu Driver
//////////////////////////////////////////

class DeviceShifuDriver {
public:
    DeviceShifuDriver()
      : shutdown_flag_(false),
        logger_(spdlog::stdout_color_mt("console")) {
        configure();
        shifu_client_ = std::make_unique<ShifuClient>(
            device_name_, edgedevice_namespace_, config_mount_path_, logger_);
        decoder_sdk_ = std::make_unique<DecoderSDK>(logger_);
        mqtt_client_ = nullptr;
    }

    void configure() {
        // Set up configuration from environment
        device_name_ = getenv_or_default("EDGEDEVICE_NAME", "deviceshifu-decoder");
        edgedevice_namespace_ = getenv_or_default("EDGEDEVICE_NAMESPACE", "devices");
        config_mount_path_ = getenv_or_default("CONFIG_MOUNT_PATH", "/etc/edgedevice/config");
        mqtt_broker_ = getenv_or_default("MQTT_BROKER", "127.0.0.1");
        mqtt_port_ = getenv_or_default_int("MQTT_BROKER_PORT", 1883);
        mqtt_username_ = getenv_or_default("MQTT_BROKER_USERNAME", "");
        mqtt_password_ = getenv_or_default("MQTT_BROKER_PASSWORD", "");
        mqtt_topic_prefix_ = getenv_or_default("MQTT_TOPIC_PREFIX", "shifu");
        http_host_ = getenv_or_default("HTTP_HOST", "0.0.0.0");
        http_port_ = getenv_or_default_int("HTTP_PORT", 8080);
        log_level_ = getenv_or_default("LOG_LEVEL", "info");
        if (log_level_ == "debug") {
            logger_->set_level(spdlog::level::debug);
        } else if (log_level_ == "info") {
            logger_->set_level(spdlog::level::info);
        } else if (log_level_ == "warn") {
            logger_->set_level(spdlog::level::warn);
        } else {
            logger_->set_level(spdlog::level::info);
        }
    }

    void run() {
        logger_->info("Starting DeviceShifu MQTT Driver for '{}'", device_name_);
        auto config_node = shifu_client_->get_instruction_config();
        if (!config_node) {
            logger_->error("Instruction config not found. Exiting.");
            return;
        }

        parse_instructions(config_node);

        if (!connect_device()) {
            logger_->error("Failed to connect to decoder device. Exiting.");
            return;
        }

        connect_mqtt();

        start_http_server();

        start_periodic_publishers();

        // Signal handling for graceful shutdown
        std::signal(SIGINT, signal_handler_static);
        std::signal(SIGTERM, signal_handler_static);

        // Main event loop
        while (!shutdown_flag_) {
            std::this_thread::sleep_for(1s);
        }
        shutdown();
    }

    static void signal_handler_static(int signum) {
        instance_->signal_handler(signum);
    }

    void signal_handler(int signum) {
        logger_->warn("Received signal {}, shutting down...", signum);
        shutdown_flag_ = true;
        cv_shutdown_.notify_all();
    }

    void shutdown() {
        logger_->info("Shutting down DeviceShifu MQTT driver...");
        if (mqtt_client_) {
            try {
                mqtt_client_->disconnect()->wait();
            } catch (...) {
                logger_->warn("MQTT disconnect error");
            }
        }
        if (http_endpoint_) {
            http_endpoint_->shutdown();
        }
        if (decoder_sdk_) {
            decoder_sdk_->disconnect();
        }
        for (auto &t : publisher_threads_) {
            if (t.joinable())
                t.join();
        }
        logger_->info("Shutdown complete.");
    }

private:
    // Configuration
    string device_name_;
    string edgedevice_namespace_;
    string config_mount_path_;
    string mqtt_broker_;
    int mqtt_port_;
    string mqtt_username_;
    string mqtt_password_;
    string mqtt_topic_prefix_;
    string http_host_;
    int http_port_;
    string log_level_;

    // State
    std::unique_ptr<ShifuClient> shifu_client_;
    std::unique_ptr<DecoderSDK> decoder_sdk_;
    std::shared_ptr<spdlog::logger> logger_;

    // MQTT
    std::unique_ptr<mqtt::async_client> mqtt_client_;
    mqtt::connect_options mqtt_conn_opts_;
    std::atomic_bool mqtt_connected_{false};
    std::mutex mqtt_mutex_;

    // Publishers
    struct InstructionInfo {
        string name;
        string method;
        string path;
        string mode;
        int publish_interval_ms;
        int qos;
    };
    map<string, InstructionInfo> instructions_;
    map<string, json> latest_data_;
    std::mutex data_mutex_;

    vector<thread> publisher_threads_;
    atomic_bool shutdown_flag_;
    condition_variable cv_shutdown_;

    // HTTP
    std::shared_ptr<Pistache::Http::Endpoint> http_endpoint_;

    // Singleton instance for signal handling
    static DeviceShifuDriver *instance_;

    //////////////////////////////////
    // Device/Instruction Handling
    //////////////////////////////////

    bool connect_device() {
        // Fetch device address and credentials (simulate)
        string address = shifu_client_->get_device_address();
        if (address.empty())
            address = "127.0.0.1";
        int port = 8000; // Simulate
        string username = "admin";
        string password = "admin123";
        return decoder_sdk_->connect(address, port, username, password);
    }

    void parse_instructions(const YAML::Node &config_node) {
        try {
            for (const auto &inst : config_node) {
                string name = inst.first.as<string>();
                const auto &node = inst.second;
                InstructionInfo info;
                info.name = name;
                info.method = node["method"] ? node["method"].as<string>() : "PUBLISH";
                info.path = node["path"] ? node["path"].as<string>() : "";
                info.mode = node["mode"] ? node["mode"].as<string>() : "publisher";
                info.publish_interval_ms = node["publishIntervalMS"] ? node["publishIntervalMS"].as<int>() : 0;
                info.qos = node["qos"] ? node["qos"].as<int>() : 0;
                instructions_[name] = info;
            }
        } catch (const std::exception &e) {
            logger_->error("parse_instructions: {}", e.what());
        }
    }

    void start_periodic_publishers() {
        for (const auto &pair : instructions_) {
            const auto &info = pair.second;
            if (info.mode == "publisher" && info.publish_interval_ms > 0) {
                publisher_threads_.emplace_back(&DeviceShifuDriver::publish_topic_periodically, this, info);
            }
        }
    }

    void publish_topic_periodically(InstructionInfo info) {
        logger_->info("Starting periodic publisher for '{}' every {} ms", info.name, info.publish_interval_ms);
        while (!shutdown_flag_) {
            try {
                publish_instruction(info);
            } catch (const std::exception &e) {
                logger_->warn("Publish thread error [{}]: {}", info.name, e.what());
            }
            std::unique_lock<std::mutex> lk(data_mutex_);
            cv_shutdown_.wait_for(lk, std::chrono::milliseconds(info.publish_interval_ms), [this]() { return shutdown_flag_; });
        }
        logger_->info("Publisher thread for '{}' exiting", info.name);
    }

    void publish_instruction(const InstructionInfo &info) {
        json payload;
        if (info.name == "status") {
            payload = decoder_sdk_->get_status();
        }
        // Add more mappings as needed.

        {
            lock_guard<mutex> lock(data_mutex_);
            latest_data_[info.name] = payload;
        }
        string topic = mqtt_topic_prefix_ + "/" + device_name_ + "/" + info.name;
        publish_mqtt(topic, payload.dump(), info.qos);
    }

    //////////////////////////////////
    // MQTT Handling
    //////////////////////////////////

    void connect_mqtt() {
        string client_id = device_name_ + "-" + std::to_string(std::time(nullptr));
        string server_uri = "tcp://" + mqtt_broker_ + ":" + std::to_string(mqtt_port_);

        mqtt_client_ = std::make_unique<mqtt::async_client>(server_uri, client_id);

        mqtt_conn_opts_.set_keep_alive_interval(20);
        mqtt_conn_opts_.set_clean_session(true);
        if (!mqtt_username_.empty())
            mqtt_conn_opts_.set_user_name(mqtt_username_);
        if (!mqtt_password_.empty())
            mqtt_conn_opts_.set_password(mqtt_password_);

        mqtt_client_->set_connected_handler([this](const string &) {
            mqtt_connected_ = true;
            logger_->info("MQTT connected");
            subscribe_control_topics();
        });
        mqtt_client_->set_connection_lost_handler([this](const string &) {
            mqtt_connected_ = false;
            logger_->warn("MQTT connection lost");
            reconnect_mqtt();
        });
        mqtt_client_->set_message_callback([this](mqtt::const_message_ptr msg) {
            handle_mqtt_message(msg);
        });

        try {
            mqtt_client_->connect(mqtt_conn_opts_)->wait();
        } catch (const mqtt::exception &e) {
            logger_->error("MQTT connect failed: {}", e.what());
        }
    }

    void reconnect_mqtt() {
        while (!shutdown_flag_ && !mqtt_connected_) {
            logger_->info("Attempting to reconnect to MQTT...");
            try {
                mqtt_client_->reconnect()->wait();
                logger_->info("Reconnected to MQTT.");
                return;
            } catch (...) {
                logger_->warn("MQTT reconnect failed, retrying in 2s...");
                std::this_thread::sleep_for(2s);
            }
        }
    }

    void subscribe_control_topics() {
        // Subscribe to command/control topics
        string control_topic = mqtt_topic_prefix_ + "/" + device_name_ + "/control/#";
        mqtt_client_->subscribe(control_topic, 1)->wait();
        logger_->info("Subscribed to control topic: {}", control_topic);

        // Subscribe to all SUBSCRIBE mode instructions
        for (const auto &pair : instructions_) {
            if (pair.second.method == "SUBSCRIBE") {
                string topic = mqtt_topic_prefix_ + "/" + device_name_ + "/" + pair.second.name;
                mqtt_client_->subscribe(topic, pair.second.qos)->wait();
                logger_->info("Subscribed to instruction topic: {}", topic);
            }
        }
    }

    void handle_mqtt_message(mqtt::const_message_ptr msg) {
        string topic = msg->get_topic();
        string payload = msg->to_string();
        logger_->info("MQTT received: topic='{}' payload='{}'", topic, payload);

        try {
            json j = json::parse(payload);
            // Handle control commands
            if (topic.find("/control/") != string::npos) {
                // Extract command
                string cmd = topic.substr(topic.find("/control/") + 9);
                handle_control_command(cmd, j);
            } else {
                // Handle instruction SUBSCRIBE topics
                for (const auto &pair : instructions_) {
                    string t = mqtt_topic_prefix_ + "/" + device_name_ + "/" + pair.second.name;
                    if (topic == t) {
                        handle_instruction(pair.second, j);
                        break;
                    }
                }
            }
        } catch (const std::exception &e) {
            logger_->warn("MQTT message parse error: {}", e.what());
        }
    }

    void handle_control_command(const string &cmd, const json &payload) {
        logger_->info("Handling control command: {} payload: {}", cmd, payload.dump());
        if (cmd == "login") {
            auto resp = decoder_sdk_->login(payload);
            publish_mqtt(mqtt_topic_prefix_ + "/" + device_name_ + "/status", resp.dump(), 1);
        } else if (cmd == "logout") {
            auto resp = decoder_sdk_->logout(payload);
            publish_mqtt(mqtt_topic_prefix_ + "/" + device_name_ + "/status", resp.dump(), 1);
        } else if (cmd == "decode") {
            auto resp = decoder_sdk_->decode_control(payload);
            publish_mqtt(mqtt_topic_prefix_ + "/" + device_name_ + "/decode/ack", resp.dump(), 1);
        } else if (cmd == "config") {
            auto resp = decoder_sdk_->config_update(payload);
            publish_mqtt(mqtt_topic_prefix_ + "/" + device_name_ + "/config/ack", resp.dump(), 1);
        }
        // Extend with more commands as needed
    }

    void handle_instruction(const InstructionInfo &info, const json &payload) {
        logger_->info("Handling instruction {} with payload {}", info.name, payload.dump());
        // For SUBSCRIBE mode, update latest_data and respond if needed
        {
            lock_guard<mutex> lock(data_mutex_);
            latest_data_[info.name] = payload;
        }
        // Optionally, perform device action here
    }

    void publish_mqtt(const string &topic, const string &payload, int qos) {
        if (!mqtt_connected_)
            return;
        try {
            mqtt::message_ptr msg = mqtt::make_message(topic, payload);
            msg->set_qos(qos);
            mqtt_client_->publish(msg);
            logger_->debug("Published MQTT: topic='{}', payload='{}'", topic, payload);
        } catch (const std::exception &e) {
            logger_->warn("MQTT publish error: {}", e.what());
        }
    }

    //////////////////////////////////
    // HTTP Server (Pistache)
    //////////////////////////////////

    void start_http_server() {
        using namespace Pistache;

        Address addr(http_host_, Port(http_port_));
        http_endpoint_ = std::make_shared<Http::Endpoint>(addr);

        auto opts = Http::Endpoint::options().threads(1);
        http_endpoint_->init(opts);

        Rest::Router router;
        setup_routes(router);

        http_endpoint_->setHandler(router.handler());
        thread([this]() {
            http_endpoint_->serve();
        }).detach();
        logger_->info("HTTP server started at {}:{}", http_host_, http_port_);
    }

    void setup_routes(Pistache::Rest::Router &router) {
        using namespace Pistache::Rest;

        Routes::Get(router, "/health", Routes::bind(&DeviceShifuDriver::route_health, this));
        Routes::Get(router, "/status", Routes::bind(&DeviceShifuDriver::route_status, this));
    }

    void route_health(const Pistache::Rest::Request &, Pistache::Http::ResponseWriter response) {
        response.send(Pistache::Http::Code::Ok, "{\"status\":\"healthy\"}\n", MIME(Application, Json));
    }

    void route_status(const Pistache::Rest::Request &, Pistache::Http::ResponseWriter response) {
        json status;
        {
            lock_guard<mutex> lock(data_mutex_);
            status["device"] = device_name_;
            status["latest_data"] = latest_data_;
            status["mqtt_connected"] = mqtt_connected_;
            status["device_connected"] = decoder_sdk_->is_connected();
        }
        response.send(Pistache::Http::Code::Ok, status.dump(), MIME(Application, Json));
    }
};

DeviceShifuDriver *DeviceShifuDriver::instance_ = nullptr;


//////////////////////////////////////////
// Main Entrypoint
//////////////////////////////////////////

int main() {
    DeviceShifuDriver driver;
    DeviceShifuDriver::instance_ = &driver;
    driver.run();
    return 0;
}