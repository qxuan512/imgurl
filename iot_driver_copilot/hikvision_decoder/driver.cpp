#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <map>
#include <vector>
#include <atomic>
#include <chrono>
#include <condition_variable>

// JSON
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// YAML
#include <yaml-cpp/yaml.h>

// MQTT
#include <mqtt/async_client.h>

// Flask-like HTTP server (using cpp-httplib)
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

// Kubernetes API (using Kubernetes C++ client, kubernetes-client/c++ or client-cpp)
// For demo, we stub/fake the Kubernetes related calls.

#define DEFAULT_CONFIG_MOUNT_PATH "/etc/edgedevice/config"
#define DEFAULT_EDGEDEVICE_NAME "deviceshifu-hikvisiondecoder"
#define DEFAULT_EDGEDEVICE_NAMESPACE "devices"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_TOPIC_PREFIX "shifu"
#define DEFAULT_HTTP_HOST "0.0.0.0"
#define DEFAULT_HTTP_PORT 8080
#define DEFAULT_LOG_LEVEL "INFO"

std::atomic<bool> shutdown_flag{false};
std::mutex data_mutex;
std::condition_variable shutdown_cv;

class Logger {
public:
    enum Level { DEBUG, INFO, WARNING, ERROR, CRITICAL };
    Level level;
    Logger() {
        std::string lvl = getenv("LOG_LEVEL") ? getenv("LOG_LEVEL") : DEFAULT_LOG_LEVEL;
        if (lvl == "DEBUG") level = DEBUG;
        else if (lvl == "INFO") level = INFO;
        else if (lvl == "WARNING") level = WARNING;
        else if (lvl == "ERROR") level = ERROR;
        else if (lvl == "CRITICAL") level = CRITICAL;
        else level = INFO;
    }
    void log(Level l, const std::string& msg) {
        if (l >= level) {
            static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
            std::cerr << "[" << names[l] << "] " << msg << std::endl;
        }
    }
    void debug(const std::string& m)    { log(DEBUG, m); }
    void info(const std::string& m)     { log(INFO, m); }
    void warning(const std::string& m)  { log(WARNING, m); }
    void error(const std::string& m)    { log(ERROR, m); }
    void critical(const std::string& m) { log(CRITICAL, m); }
};
Logger logger;

// --- Kubernetes ShifuClient stub ---
class ShifuClient {
public:
    std::string edgedevice_name;
    std::string edgedevice_namespace;
    std::string config_mount_path;

    ShifuClient() {
        edgedevice_name = getenv("EDGEDEVICE_NAME") ? getenv("EDGEDEVICE_NAME") : DEFAULT_EDGEDEVICE_NAME;
        edgedevice_namespace = getenv("EDGEDEVICE_NAMESPACE") ? getenv("EDGEDEVICE_NAMESPACE") : DEFAULT_EDGEDEVICE_NAMESPACE;
        config_mount_path = getenv("CONFIG_MOUNT_PATH") ? getenv("CONFIG_MOUNT_PATH") : DEFAULT_CONFIG_MOUNT_PATH;
    }

    void _init_k8s_client() {
        // For demo: simulate k8s client init
        logger.info("Initialized Kubernetes client (stub)");
    }

    json get_edge_device() {
        // For demo: simulate get EdgeDevice CR
        json j;
        j["metadata"]["name"] = edgedevice_name;
        j["metadata"]["namespace"] = edgedevice_namespace;
        j["spec"]["address"] = "192.168.1.100";
        return j;
    }

    std::string get_device_address() {
        json dev = get_edge_device();
        try {
            return dev["spec"]["address"];
        } catch (...) {
            logger.error("Device address not found in EdgeDevice CR");
            return "";
        }
    }

    void update_device_status(const std::string& status) {
        // For demo: simulate k8s status update
        logger.info("Device status updated to: " + status);
    }

    std::string read_mounted_config_file(const std::string& filename) {
        std::ifstream f(config_mount_path + "/" + filename);
        if (!f.is_open()) {
            logger.error("Could not open config file: " + filename);
            return "";
        }
        std::stringstream buffer;
        buffer << f.rdbuf();
        return buffer.str();
    }

    YAML::Node get_instruction_config() {
        std::string instructions = read_mounted_config_file("instructions");
        if (instructions.empty()) return YAML::Node();
        try {
            return YAML::Load(instructions);
        } catch (const std::exception& e) {
            logger.error(std::string("Failed to parse YAML: ") + e.what());
            return YAML::Node();
        }
    }
};

// --- Device (Hikvision Decoder) Native Protocol Abstraction (Stub) ---
class HikvisionDecoderDevice {
public:
    std::string device_address;
    HikvisionDecoderDevice(const std::string& addr) : device_address(addr) {
        // TODO: Connect to device using SDK or RTSP
    }
    bool connect() {
        // TODO: Connect to Hikvision device
        logger.info("Connecting to Hikvision Decoder at: " + device_address);
        return true;
    }
    void disconnect() {
        logger.info("Disconnecting from Hikvision Decoder");
    }
    json get_status() {
        // Simulate device status
        json status;
        status["deviceName"] = "Hikvision Decoder";
        status["channels"] = {{"1", "online"}, {"2", "offline"}};
        status["firmware"] = "v3.2.1";
        status["alarm"] = "none";
        status["network"] = {{"ip", device_address}, {"status", "connected"}};
        status["error"] = 0;
        return status;
    }
    json get_decoder_channel_status() {
        json ch;
        ch["channel"] = 1;
        ch["status"] = "enabled";
        return ch;
    }
    bool send_command(const std::string& cmd, const json& payload) {
        logger.info("Sending command '" + cmd + "' to decoder: " + payload.dump());
        return true;
    }
};

// --- MQTT Driver Implementation ---
class DeviceShifuDriver {
public:
    ShifuClient shifu_client;
    std::unique_ptr<HikvisionDecoderDevice> device;
    std::map<std::string, json> latest_data;
    std::map<std::string, int> publish_intervals_ms;
    std::map<std::string, std::thread> publisher_threads;

    std::unique_ptr<mqtt::async_client> mqtt_client;
    std::string mqtt_broker;
    int mqtt_port;
    std::string mqtt_username, mqtt_password;
    std::string mqtt_topic_prefix;
    std::string device_name;

    std::string http_host;
    int http_port;

    DeviceShifuDriver() {
        // Get env vars
        mqtt_broker = getenv("MQTT_BROKER") ? getenv("MQTT_BROKER") : "localhost";
        mqtt_port = getenv("MQTT_BROKER_PORT") ? std::stoi(getenv("MQTT_BROKER_PORT")) : DEFAULT_MQTT_PORT;
        mqtt_username = getenv("MQTT_BROKER_USERNAME") ? getenv("MQTT_BROKER_USERNAME") : "";
        mqtt_password = getenv("MQTT_BROKER_PASSWORD") ? getenv("MQTT_BROKER_PASSWORD") : "";
        mqtt_topic_prefix = getenv("MQTT_TOPIC_PREFIX") ? getenv("MQTT_TOPIC_PREFIX") : DEFAULT_MQTT_TOPIC_PREFIX;
        http_host = getenv("HTTP_HOST") ? getenv("HTTP_HOST") : DEFAULT_HTTP_HOST;
        http_port = getenv("HTTP_PORT") ? std::stoi(getenv("HTTP_PORT")) : DEFAULT_HTTP_PORT;
        device_name = shifu_client.edgedevice_name;
    }

    void _parse_publish_intervals(const YAML::Node& instruction_config) {
        if (!instruction_config) return;
        for (auto it = instruction_config.begin(); it != instruction_config.end(); ++it) {
            std::string instr = it->first.as<std::string>();
            auto prop = it->second;
            if (prop["mode"] && prop["mode"].as<std::string>() == "publisher") {
                int interval = prop["publishIntervalMS"] ? prop["publishIntervalMS"].as<int>() : 1000;
                publish_intervals_ms[instr] = interval;
            }
        }
    }

    void _start_scheduled_publishers() {
        for (const auto& kv : publish_intervals_ms) {
            std::string instr = kv.first;
            int interval = kv.second;
            publisher_threads[instr] = std::thread([this, instr, interval]() { 
                this->_publish_topic_periodically(instr, interval);
            });
        }
    }

    void _publish_topic_periodically(const std::string& instr, int interval_ms) {
        while (!shutdown_flag) {
            try {
                json data;
                if (instr == "status" || instr == "telemetry/status") {
                    data = device->get_status();
                } else if (instr == "decoder") {
                    data = device->get_decoder_channel_status();
                } else {
                    data = {{"info", "Unknown instruction"}};
                }
                {
                    std::lock_guard<std::mutex> lock(data_mutex);
                    latest_data[instr] = data;
                }
                std::string topic = mqtt_topic_prefix + "/" + device_name + "/" + instr;
                mqtt::message_ptr pubmsg = mqtt::make_message(topic, data.dump(), 1, false);
                mqtt_client->publish(pubmsg)->wait();
                logger.debug("Published topic: " + topic + " payload: " + data.dump());
            } catch (const std::exception& e) {
                logger.error(std::string("Error in scheduled publisher: ") + e.what());
            }
            std::unique_lock<std::mutex> lk(data_mutex);
            shutdown_cv.wait_for(lk, std::chrono::milliseconds(interval_ms), []() { return shutdown_flag.load(); });
        }
    }

    void connect_mqtt() {
        std::string address = "tcp://" + mqtt_broker + ":" + std::to_string(mqtt_port);
        std::string client_id = device_name + "-" + std::to_string(time(nullptr));
        mqtt_client.reset(new mqtt::async_client(address, client_id));
        mqtt::connect_options connOpts;
        if (!mqtt_username.empty()) connOpts.set_user_name(mqtt_username);
        if (!mqtt_password.empty()) connOpts.set_password(mqtt_password);
        connOpts.set_keep_alive_interval(20);
        connOpts.set_clean_session(true);

        mqtt_client->set_connected_handler([this](const std::string&) {
            logger.info("MQTT connected");
            subscribe_control_topics();
            report_status("connected");
        });
        mqtt_client->set_connection_lost_handler([this](const std::string&) {
            logger.warning("MQTT connection lost");
            report_status("disconnected");
        });
        mqtt_client->set_message_callback([this](mqtt::const_message_ptr msg) {
            this->handle_mqtt_message(msg);
        });

        while (!shutdown_flag) {
            try {
                mqtt_client->connect(connOpts)->wait();
                break;
            } catch (const mqtt::exception& e) {
                logger.error(std::string("MQTT connection failed: ") + e.what());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
    }

    void subscribe_control_topics() {
        std::vector<std::string> control_topics = {
            mqtt_topic_prefix + "/" + device_name + "/command/remotePlayback",
            mqtt_topic_prefix + "/" + device_name + "/command/decoder"
        };
        for (const auto& t : control_topics) {
            mqtt_client->subscribe(t, 1)->wait();
            logger.info("Subscribed to control topic: " + t);
        }
    }

    void handle_mqtt_message(mqtt::const_message_ptr msg) {
        std::string topic = msg->get_topic();
        std::string payload = msg->to_string();
        logger.debug("Received MQTT message. Topic: " + topic + " Payload: " + payload);
        try {
            json data = json::parse(payload);
            if (topic.find("/command/remotePlayback") != std::string::npos) {
                device->send_command("remotePlayback", data);
            } else if (topic.find("/command/decoder") != std::string::npos) {
                device->send_command("decoder", data);
            }
        } catch (const std::exception& e) {
            logger.error(std::string("Failed to handle MQTT message: ") + e.what());
        }
    }

    void report_status(const std::string& status) {
        std::string topic = mqtt_topic_prefix + "/" + device_name + "/status";
        json status_msg;
        status_msg["status"] = status;
        status_msg["timestamp"] = std::time(nullptr);
        try {
            mqtt::message_ptr pubmsg = mqtt::make_message(topic, status_msg.dump(), 1, false);
            mqtt_client->publish(pubmsg)->wait();
            logger.info("Published status: " + status);
        } catch (const std::exception& e) {
            logger.error(std::string("Failed to publish status: ") + e.what());
        }
    }

    void setup_http_server() {
        httplib::Server svr;
        svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            json resp;
            resp["status"] = "ok";
            res.set_content(resp.dump(), "application/json");
        });
        svr.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            json resp;
            resp["device"] = device_name;
            resp["status"] = "running";
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                resp["latest_data"] = latest_data;
            }
            res.set_content(resp.dump(), "application/json");
        });
        svr.set_exception_handler([](const auto&, auto& res, std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
        });
        svr.listen(http_host.c_str(), http_port);
    }

    static void signal_handler(int signum) {
        logger.info("Signal received: " + std::to_string(signum));
        shutdown_flag = true;
        shutdown_cv.notify_all();
    }

    void shutdown() {
        logger.info("Shutting down...");
        shutdown_flag = true;
        shutdown_cv.notify_all();
        try {
            if (mqtt_client) mqtt_client->disconnect()->wait();
        } catch (...) {}
        if (device) device->disconnect();
        for (auto& kv : publisher_threads) {
            if (kv.second.joinable()) kv.second.join();
        }
        logger.info("Shutdown complete.");
    }

    void run() {
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        shifu_client._init_k8s_client();
        std::string dev_addr = shifu_client.get_device_address();
        device.reset(new HikvisionDecoderDevice(dev_addr));
        if (!device->connect()) {
            logger.critical("Failed to connect to Hikvision Decoder device");
            return;
        }

        YAML::Node instruction_config = shifu_client.get_instruction_config();
        _parse_publish_intervals(instruction_config);

        std::thread http_thread([this]() {
            try { setup_http_server(); }
            catch (const std::exception& e) {
                logger.error(std::string("HTTP server error: ") + e.what());
            }
        });
        http_thread.detach();

        connect_mqtt();
        _start_scheduled_publishers();

        report_status("running");

        // Main loop: wait for shutdown
        while (!shutdown_flag) {
            std::unique_lock<std::mutex> lk(data_mutex);
            shutdown_cv.wait_for(lk, std::chrono::seconds(1), []() { return shutdown_flag.load(); });
        }
        shutdown();
    }
};

// ---- Main Entrypoint ----
int main(int argc, char* argv[]) {
    try {
        DeviceShifuDriver driver;
        driver.run();
    } catch (const std::exception& e) {
        logger.critical(std::string("Fatal error: ") + e.what());
        return 1;
    }
    return 0;
}