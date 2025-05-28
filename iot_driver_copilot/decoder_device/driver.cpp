#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <condition_variable>

#include <json/json.h>
#include <yaml-cpp/yaml.h>
#include <paho.mqtt.cpp/async_client.h>
#include <cpprest/http_listener.h>
#include <cpprest/json.h>
#include <cpprest/uri.h>

// Kubernetes client includes (pseudo, to be replaced with actual C++ Kubernetes client or REST calls)
#include "k8s_client.hpp" // Assume this provides Kubernetes interaction

using namespace std;
using namespace web;
using namespace http;
using namespace http::experimental::listener;

// ----------- Logging -----------
enum class LogLevel { DEBUG, INFO, WARNING, ERROR };
LogLevel log_level = LogLevel::INFO;
mutex log_mutex;

void set_log_level(const string& lvl) {
    if (lvl == "DEBUG") log_level = LogLevel::DEBUG;
    else if (lvl == "INFO") log_level = LogLevel::INFO;
    else if (lvl == "WARNING") log_level = LogLevel::WARNING;
    else if (lvl == "ERROR") log_level = LogLevel::ERROR;
}

#define LOG(level, msg) \
    do { \
        lock_guard<mutex> lock(log_mutex); \
        if (log_level <= level) { \
            string lvlstr; \
            switch (level) { \
                case LogLevel::DEBUG: lvlstr = "DEBUG"; break; \
                case LogLevel::INFO: lvlstr = "INFO"; break; \
                case LogLevel::WARNING: lvlstr = "WARNING"; break; \
                case LogLevel::ERROR: lvlstr = "ERROR"; break; \
            } \
            cerr << "[" << lvlstr << "] " << __FUNCTION__ << ": " << msg << endl; \
        } \
    } while (0)

// ----------- Utility -----------
string getenv_or(const string& key, const string& default_val) {
    const char* val = getenv(key.c_str());
    return val ? string(val) : default_val;
}

// ----------- ShifuClient -----------
class ShifuClient {
public:
    ShifuClient() : initialized(false) {
        edge_device_name = getenv_or("EDGEDEVICE_NAME", "deviceshifu-networkvideodecoder");
        edge_device_namespace = getenv_or("EDGEDEVICE_NAMESPACE", "devices");
        config_mount_path = getenv_or("CONFIG_MOUNT_PATH", "/etc/edgedevice/config");
        init_k8s_client();
    }

    void init_k8s_client() {
        try {
            // This is a pseudo-implementation, use your preferred C++ K8s client or REST
            k8s_client = make_unique<K8sClient>(edge_device_namespace);
            initialized = true;
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "Failed to initialize K8s client: " << e.what());
        }
    }

    Json::Value get_edge_device() {
        if (!initialized) { init_k8s_client(); }
        try {
            return k8s_client->get_edge_device(edge_device_name);
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "Failed to get EdgeDevice resource: " << e.what());
            return Json::Value();
        }
    }

    string get_device_address() {
        Json::Value ed = get_edge_device();
        if (ed.isMember("spec") && ed["spec"].isMember("address")) {
            return ed["spec"]["address"].asString();
        }
        return "";
    }

    void update_device_status(const string& status, const string& reason = "") {
        try {
            k8s_client->update_edge_device_status(edge_device_name, status, reason);
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "Failed to update device status: " << e.what());
        }
    }

    string read_mounted_config_file(const string& filename) {
        string path = config_mount_path + "/" + filename;
        ifstream ifs(path);
        if (!ifs) {
            LOG(LogLevel::ERROR, "Failed to open config file: " << path);
            return "";
        }
        stringstream buffer;
        buffer << ifs.rdbuf();
        return buffer.str();
    }

    YAML::Node get_instruction_config() {
        string s = read_mounted_config_file("instructions");
        if (s.empty()) return YAML::Node();
        try {
            return YAML::Load(s);
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "Failed to parse YAML instructions: " << e.what());
            return YAML::Node();
        }
    }

    YAML::Node get_driver_properties() {
        string s = read_mounted_config_file("driverProperties");
        if (s.empty()) return YAML::Node();
        try {
            return YAML::Load(s);
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "Failed to parse YAML driverProperties: " << e.what());
            return YAML::Node();
        }
    }

private:
    string edge_device_name;
    string edge_device_namespace;
    string config_mount_path;
    unique_ptr<K8sClient> k8s_client;
    bool initialized;
};

// ----------- DecoderDevice Abstraction (SDK stub) -----------
class DecoderDevice {
public:
    DecoderDevice(const string& device_address) : address(device_address) {
        // Initialize SDK connection, login, etc.
        // Pseudo: this should be replaced with actual device SDK logic.
    }

    bool connect() {
        // Connect to device (login)
        return true;
    }

    void disconnect() {
        // Disconnect from device (logout)
    }

    // Gather telemetry/status data, convert to JSON
    Json::Value get_status() {
        Json::Value status;
        status["device"] = "Decoder Device";
        status["online"] = true;
        status["timestamp"] = static_cast<Json::UInt64>(time(nullptr));
        // Add more fields here from the actual device
        return status;
    }

    // Handle control commands, returns a JSON response
    Json::Value handle_command(const Json::Value& cmd) {
        Json::Value resp;
        // Implement command handling here, stub for now
        resp["result"] = "ok";
        resp["command"] = cmd;
        return resp;
    }

private:
    string address;
};

// ----------- Driver Implementation -----------
class DeviceShifuMQTTDriver {
public:
    DeviceShifuMQTTDriver() :
        shutdown_flag(false),
        shifu_client(),
        mqtt_broker(getenv_or("MQTT_BROKER", "localhost")),
        mqtt_port(stoi(getenv_or("MQTT_BROKER_PORT", "1883"))),
        mqtt_username(getenv_or("MQTT_BROKER_USERNAME", "")),
        mqtt_password(getenv_or("MQTT_BROKER_PASSWORD", "")),
        mqtt_topic_prefix(getenv_or("MQTT_TOPIC_PREFIX", "shifu")),
        http_host(getenv_or("HTTP_HOST", "0.0.0.0")),
        http_port(getenv_or("HTTP_PORT", "8080")),
        device_name(getenv_or("EDGEDEVICE_NAME", "deviceshifu-networkvideodecoder")),
        device_namespace(getenv_or("EDGEDEVICE_NAMESPACE", "devices")),
        client_id(device_name + "-" + to_string(time(nullptr))),
        latest_data_mutex(),
        status_thread(nullptr),
        http_server_thread(nullptr)
    {
        set_log_level(getenv_or("LOG_LEVEL", "INFO"));
        LOG(LogLevel::INFO, "Starting DeviceShifuMQTTDriver");

        // Parse instruction config
        parse_instruction_config();

        // Device connection
        string device_addr = shifu_client.get_device_address();
        decoder_device = make_unique<DecoderDevice>(device_addr);

        // MQTT client
        string mqtt_addr = "tcp://" + mqtt_broker + ":" + to_string(mqtt_port);
        mqtt_client = make_unique<mqtt::async_client>(mqtt_addr, client_id);

        // HTTP server setup
        http_listener_url = "http://" + http_host + ":" + http_port;
        http_listener = nullptr;

        // Thread management
        shutdown_cv = make_unique<condition_variable>();
    }

    ~DeviceShifuMQTTDriver() {
        shutdown();
    }

    void run() {
        signal(SIGINT, signal_handler_wrapper);
        signal(SIGTERM, signal_handler_wrapper);

        // Connect device
        if (!decoder_device->connect()) {
            LOG(LogLevel::ERROR, "Failed to connect to Decoder Device");
            shifu_client.update_device_status("Offline", "Device connection failed");
        } else {
            shifu_client.update_device_status("Online", "");
            LOG(LogLevel::INFO, "Connected to Decoder Device");
        }

        // Connect MQTT
        if (!connect_mqtt()) {
            LOG(LogLevel::ERROR, "Failed to connect to MQTT broker");
            shifu_client.update_device_status("Offline", "MQTT connection failed");
        } else {
            LOG(LogLevel::INFO, "Connected to MQTT broker");
        }

        // Start publisher threads
        start_scheduled_publishers();

        // Start HTTP server
        http_server_thread = new thread(&DeviceShifuMQTTDriver::run_http_server, this);

        // Connection monitor
        status_thread = new thread(&DeviceShifuMQTTDriver::connection_monitor, this);

        // Main loop: wait for shutdown
        unique_lock<mutex> lk(shutdown_mutex);
        shutdown_cv->wait(lk, [this]() { return shutdown_flag.load(); });

        LOG(LogLevel::INFO, "Main loop exiting");
    }

    void shutdown() {
        bool expected = false;
        if (!shutdown_flag.compare_exchange_strong(expected, true)) return;

        LOG(LogLevel::INFO, "Shutting down...");

        // Stop HTTP server
        if (http_listener) {
            http_listener->close().wait();
            delete http_listener;
            http_listener = nullptr;
        }
        if (http_server_thread) {
            http_server_thread->join();
            delete http_server_thread;
            http_server_thread = nullptr;
        }

        // Stop publisher threads
        for (auto& t : publisher_threads) {
            if (t.joinable()) t.join();
        }
        publisher_threads.clear();

        // Stop status thread
        if (status_thread) {
            status_thread->join();
            delete status_thread;
            status_thread = nullptr;
        }

        // Disconnect MQTT
        try {
            if (mqtt_client && mqtt_client->is_connected()) {
                mqtt_client->disconnect()->wait();
            }
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "Error disconnecting MQTT: " << e.what());
        }

        // Disconnect device
        decoder_device->disconnect();

        LOG(LogLevel::INFO, "Shutdown complete");
    }

    static void signal_handler_wrapper(int signal) {
        instance()->signal_handler(signal);
    }

    void signal_handler(int signal) {
        LOG(LogLevel::INFO, "Signal received: " << signal);
        shutdown();
        // Wake up main thread if needed
        shutdown_cv->notify_all();
    }

    static DeviceShifuMQTTDriver* instance_ptr;
    static DeviceShifuMQTTDriver* instance() { return instance_ptr; }

private:
    // Members
    atomic<bool> shutdown_flag;
    ShifuClient shifu_client;
    unique_ptr<DecoderDevice> decoder_device;

    string mqtt_broker;
    int mqtt_port;
    string mqtt_username;
    string mqtt_password;
    string mqtt_topic_prefix;
    string http_host;
    string http_port;
    string device_name;
    string device_namespace;
    string client_id;

    map<string, size_t> topic_publish_intervals;
    map<string, string> topic_mode;
    map<string, int> topic_qos;
    map<string, thread> publisher_threads;

    unique_ptr<mqtt::async_client> mqtt_client;
    unique_ptr<mqtt::connect_options> mqtt_conn_opts;
    mutex latest_data_mutex;
    map<string, Json::Value> latest_data;

    string http_listener_url;
    http_listener* http_listener;
    thread* http_server_thread;
    thread* status_thread;
    unique_ptr<condition_variable> shutdown_cv;
    mutex shutdown_mutex;

    // --- Configuration Parsing ---
    void parse_instruction_config() {
        YAML::Node instructions = shifu_client.get_instruction_config();
        if (!instructions || !instructions.IsMap()) {
            LOG(LogLevel::ERROR, "No valid instructions found");
            return;
        }
        for (auto it = instructions.begin(); it != instructions.end(); ++it) {
            string instr_name = it->first.as<string>();
            YAML::Node instr = it->second;
            string mode = instr["protocolProperty"]["mode"].as<string>("publisher");
            size_t interval = instr["protocolProperty"]["publishIntervalMs"].as<size_t>(1000);
            int qos = instr["protocolProperty"]["qos"].as<int>(1);
            topic_publish_intervals[instr_name] = interval;
            topic_mode[instr_name] = mode;
            topic_qos[instr_name] = qos;
        }
    }

    // --- MQTT ---
    bool connect_mqtt() {
        try {
            mqtt::connect_options conn_opts;
            conn_opts.set_keep_alive_interval(20);
            conn_opts.set_clean_session(true);
            if (!mqtt_username.empty()) {
                conn_opts.set_user_name(mqtt_username);
                conn_opts.set_password(mqtt_password);
            }
            mqtt_conn_opts = make_unique<mqtt::connect_options>(conn_opts);

            mqtt_client->set_connected_handler([this](const string&) {
                LOG(LogLevel::INFO, "MQTT connected");
                subscribe_topics();
                publish_status("Online");
            });
            mqtt_client->set_connection_lost_handler([this](const string&) {
                LOG(LogLevel::WARNING, "MQTT connection lost");
                publish_status("Offline");
            });
            mqtt_client->set_message_callback([this](mqtt::const_message_ptr msg) {
                handle_mqtt_message(msg);
            });

            mqtt_client->connect(*mqtt_conn_opts)->wait();
            return true;
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "MQTT connection failed: " << e.what());
            return false;
        }
    }

    void subscribe_topics() {
        // Subscribe to device commands
        string topic = mqtt_topic_prefix + "/" + device_name + "/device/commands/control";
        int qos = 1;
        try {
            mqtt_client->subscribe(topic, qos)->wait();
            LOG(LogLevel::INFO, "Subscribed to: " << topic);
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "Failed to subscribe: " << e.what());
        }
    }

    void publish_status(const string& status) {
        Json::Value payload;
        payload["status"] = status;
        payload["timestamp"] = static_cast<Json::UInt64>(time(nullptr));
        string topic = mqtt_topic_prefix + "/" + device_name + "/status";
        publish_mqtt(topic, payload, 1, true);
    }

    void publish_mqtt(const string& topic, const Json::Value& payload, int qos, bool retained = false) {
        Json::StreamWriterBuilder wbuilder;
        string js = Json::writeString(wbuilder, payload);

        auto msg = mqtt::make_message(topic, js);
        msg->set_qos(qos);
        msg->set_retained(retained);

        try {
            mqtt_client->publish(msg)->wait();
            LOG(LogLevel::DEBUG, "Published to " << topic << ": " << js);
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "MQTT publish failed: " << e.what());
        }
    }

    void handle_mqtt_message(mqtt::const_message_ptr msg) {
        try {
            string topic = msg->get_topic();
            string payload = msg->to_string();
            LOG(LogLevel::INFO, "MQTT message received: " << topic << " | " << payload);

            // Only handle control
            if (topic.find("/device/commands/control") != string::npos) {
                Json::Value jpayload;
                Json::CharReaderBuilder rbuilder;
                string errs;
                istringstream ss(payload);
                if (!Json::parseFromStream(rbuilder, ss, &jpayload, &errs)) {
                    LOG(LogLevel::ERROR, "Failed to parse command JSON: " << errs);
                    return;
                }
                Json::Value resp = decoder_device->handle_command(jpayload);
                string resp_topic = mqtt_topic_prefix + "/" + device_name + "/device/commands/control/response";
                publish_mqtt(resp_topic, resp, 1, false);
            }
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "Error handling MQTT message: " << e.what());
        }
    }

    // --- Telemetry Publisher ---
    void start_scheduled_publishers() {
        for (const auto& kv : topic_publish_intervals) {
            string topic = kv.first;
            size_t interval = kv.second;
            if (topic_mode[topic] == "publisher") {
                publisher_threads[topic] = thread(&DeviceShifuMQTTDriver::publish_topic_periodically, this, topic, interval, topic_qos[topic]);
            }
        }
    }

    void publish_topic_periodically(string topic, size_t interval_ms, int qos) {
        string mqtt_topic = mqtt_topic_prefix + "/" + device_name + "/" + topic;
        while (!shutdown_flag.load()) {
            Json::Value data = decoder_device->get_status();
            {
                lock_guard<mutex> lock(latest_data_mutex);
                latest_data[topic] = data;
            }
            publish_mqtt(mqtt_topic, data, qos, false);

            for (size_t i = 0; i < interval_ms / 100; ++i) {
                if (shutdown_flag.load()) break;
                this_thread::sleep_for(chrono::milliseconds(100));
            }
        }
    }

    // --- HTTP Server ---
    void run_http_server() {
        try {
            http_listener = new http_listener(http_listener_url);
            http_listener->support(methods::GET, [this](http_request request) {
                string path = request.relative_uri().path();
                if (path == "/health") {
                    json::value resp;
                    resp["status"] = json::value::string("ok");
                    request.reply(status_codes::OK, resp);
                } else if (path == "/status") {
                    lock_guard<mutex> lock(latest_data_mutex);
                    json::value resp;
                    for (const auto& kv : latest_data) {
                        Json::StreamWriterBuilder wbuilder;
                        string js = Json::writeString(wbuilder, kv.second);
                        resp[kv.first] = json::value::string(js);
                    }
                    request.reply(status_codes::OK, resp);
                } else {
                    request.reply(status_codes::NotFound, "Not found");
                }
            });
            http_listener->open().wait();
            LOG(LogLevel::INFO, "HTTP server running at " << http_listener_url);
            while (!shutdown_flag.load()) {
                this_thread::sleep_for(chrono::milliseconds(250));
            }
            http_listener->close().wait();
        } catch (const exception& e) {
            LOG(LogLevel::ERROR, "HTTP server error: " << e.what());
        }
    }

    // --- Connection Monitor ---
    void connection_monitor() {
        while (!shutdown_flag.load()) {
            bool dev_ok = decoder_device->connect();
            bool mqtt_ok = mqtt_client->is_connected();
            if (!dev_ok) {
                shifu_client.update_device_status("Offline", "Device connection lost");
            }
            if (!mqtt_ok) {
                shifu_client.update_device_status("Offline", "MQTT disconnected");
                // Try reconnect
                connect_mqtt();
            }
            this_thread::sleep_for(chrono::seconds(5));
        }
    }
};

// Global instance pointer for signal handling
DeviceShifuMQTTDriver* DeviceShifuMQTTDriver::instance_ptr = nullptr;

// ----------- Main Entrypoint -----------
int main(int argc, char* argv[]) {
    DeviceShifuMQTTDriver driver;
    DeviceShifuMQTTDriver::instance_ptr = &driver;
    driver.run();
    return 0;
}