```cpp
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <thread>
#include <sstream>
#include <fstream>
#include <streambuf>
#include <mutex>
#include <regex>
#include <algorithm>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "tinyxml2.h"

// ==================== Environment Variable Access ===========================
std::string get_env(const std::string &key, const std::string &default_val = "") {
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : default_val;
}

// ==================== Device SDK/Protocol Layer (Stubs) =====================
// In a real implementation, these would interface with the device via HCNetSDK or similar APIs.

struct ChannelConfig {
    int id;
    bool enabled;
    std::string name;
    std::map<std::string, std::string> settings;
};

struct DisplayConfig {
    std::string layout;
    int scenes;
    std::vector<std::string> windows;
};

struct DeviceStatus {
    std::string decoder_state;
    std::string alarm;
    std::string remote_play;
    std::string firmware_upgrade;
    int loop_polling;
    std::string operational_state;
};

std::mutex device_mutex;
std::vector<ChannelConfig> mock_channels = {
    {1, true,  "Channel 1", {{"resolution","1080p"}}},
    {2, false, "Channel 2", {{"resolution","720p"}}},
    {3, true,  "Channel 3", {{"resolution","1080p"}}}
};

DisplayConfig mock_display = {"4x4", 2, {"window1", "window2"}};
DeviceStatus mock_status = {"active", "none", "stopped", "idle", 1, "running"};

std::vector<ChannelConfig> get_channels(const std::map<std::string, std::string>& filters) {
    std::lock_guard<std::mutex> lock(device_mutex);
    std::vector<ChannelConfig> result;
    for (const auto &ch : mock_channels) {
        bool ok = true;
        if (filters.count("id")) {
            ok &= std::to_string(ch.id) == filters.at("id");
        }
        if (filters.count("enabled")) {
            ok &= ((ch.enabled && filters.at("enabled") == "true") || (!ch.enabled && filters.at("enabled") == "false"));
        }
        if (ok) result.push_back(ch);
    }
    return result;
}

bool update_channel(int id, bool enabled, const std::map<std::string, std::string>& settings) {
    std::lock_guard<std::mutex> lock(device_mutex);
    for (auto& ch : mock_channels) {
        if (ch.id == id) {
            ch.enabled = enabled;
            for (const auto& kv : settings) ch.settings[kv.first] = kv.second;
            return true;
        }
    }
    return false;
}

bool update_channels_bulk(const std::vector<std::pair<int,bool>>& updates) {
    std::lock_guard<std::mutex> lock(device_mutex);
    for (auto& up : updates) {
        bool found = false;
        for (auto& ch : mock_channels) {
            if (ch.id == up.first) {
                ch.enabled = up.second;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

DisplayConfig get_display() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return mock_display;
}

bool update_display(const DisplayConfig& config) {
    std::lock_guard<std::mutex> lock(device_mutex);
    mock_display = config;
    return true;
}

DeviceStatus get_status() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return mock_status;
}

bool send_control_command(const std::string& type, const std::map<std::string, std::string>& params) {
    std::lock_guard<std::mutex> lock(device_mutex);
    if (type == "reset") {
        mock_status.operational_state = "rebooting";
        return true;
    }
    // Simulate other controls...
    return true;
}

bool decode_control(const std::string& action) {
    std::lock_guard<std::mutex> lock(device_mutex);
    if (action == "start") mock_status.decoder_state = "decoding";
    else if (action == "stop") mock_status.decoder_state = "idle";
    else return false;
    return true;
}

bool reboot_device() {
    std::lock_guard<std::mutex> lock(device_mutex);
    mock_status.operational_state = "rebooting";
    return true;
}

// =================== JSON Serialization/Parsing =============================
// (Simple, minimal. For production use a robust library.)

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string channel_to_json(const ChannelConfig& ch) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"id\":" << ch.id << ",";
    oss << "\"enabled\":" << (ch.enabled ? "true" : "false") << ",";
    oss << "\"name\":\"" << json_escape(ch.name) << "\",";
    oss << "\"settings\":{";
    bool first=true;
    for (const auto& kv : ch.settings) {
        if (!first) oss << ",";
        oss << "\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
        first=false;
    }
    oss << "}}";
    return oss.str();
}

std::string channels_to_json(const std::vector<ChannelConfig>& chs) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i=0; i<chs.size(); ++i) {
        if (i) oss << ",";
        oss << channel_to_json(chs[i]);
    }
    oss << "]";
    return oss.str();
}

std::string display_to_json(const DisplayConfig& d) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"layout\":\"" << json_escape(d.layout) << "\",";
    oss << "\"scenes\":" << d.scenes << ",";
    oss << "\"windows\":[";
    for (size_t i=0; i<d.windows.size(); ++i) {
        if (i) oss << ",";
        oss << "\"" << json_escape(d.windows[i]) << "\"";
    }
    oss << "]}";
    return oss.str();
}

std::string status_to_json(const DeviceStatus& s) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"decoder_state\":\"" << json_escape(s.decoder_state) << "\",";
    oss << "\"alarm\":\"" << json_escape(s.alarm) << "\",";
    oss << "\"remote_play\":\"" << json_escape(s.remote_play) << "\",";
    oss << "\"firmware_upgrade\":\"" << json_escape(s.firmware_upgrade) << "\",";
    oss << "\"loop_polling\":" << s.loop_polling << ",";
    oss << "\"operational_state\":\"" << json_escape(s.operational_state) << "\"";
    oss << "}";
    return oss.str();
}

// Very simple JSON parser for our expected input
std::map<std::string, std::string> parse_json_body(const std::string& body) {
    std::map<std::string, std::string> m;
    std::regex rx("\"([^\"]+)\":\\s*(\"[^\"]+\"|\\d+|true|false|null)");
    auto begin = std::sregex_iterator(body.begin(), body.end(), rx);
    auto end = std::sregex_iterator();
    for (auto i = begin; i != end; ++i) {
        std::string k = (*i)[1].str();
        std::string v = (*i)[2].str();
        if (!v.empty() && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size()-2);
        m[k]=v;
    }
    return m;
}

// ================ HTTP Server & Route Handlers ==============================

void run_http_server(const std::string& host, int port) {
    httplib::Server svr;

    // GET /channels
    svr.Get(R"(/channels)", [](const httplib::Request& req, httplib::Response& res) {
        std::map<std::string, std::string> filters;
        if (req.has_param("id")) filters["id"] = req.get_param_value("id");
        if (req.has_param("enabled")) filters["enabled"] = req.get_param_value("enabled");
        auto channels = get_channels(filters);
        res.set_content(channels_to_json(channels), "application/json");
    });

    // PUT /channels (bulk enable/disable)
    svr.Put(R"(/channels)", [](const httplib::Request& req, httplib::Response& res) {
        // Expect JSON: [{"id":1,"enabled":true},...]
        try {
            std::vector<std::pair<int,bool>> updates;
            std::regex rx(R"(\{[^\}]*"id"\s*:\s*(\d+)[^\}]*"enabled"\s*:\s*(true|false)[^\}]*\})");
            auto begin = std::sregex_iterator(req.body.begin(), req.body.end(), rx);
            auto end = std::sregex_iterator();
            for (auto i = begin; i != end; ++i) {
                int id = std::stoi((*i)[1]);
                bool enabled = ((*i)[2] == "true");
                updates.push_back({id, enabled});
            }
            if (!updates.empty() && update_channels_bulk(updates)) {
                res.set_content(R"({"status":"success"})", "application/json");
            } else {
                res.status = 400;
                res.set_content(R"({"error":"Failed to update channels"})", "application/json");
            }
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid input"})", "application/json");
        }
    });

    // PUT /channels/{id}
    svr.Put(R"(/channels/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        auto m = parse_json_body(req.body);
        bool enabled = (m.count("enabled") && (m["enabled"] == "true"));
        auto settings = m;
        settings.erase("enabled");
        if (update_channel(id, enabled, settings)) {
            res.set_content(R"({"status":"success"})", "application/json");
        } else {
            res.status = 404;
            res.set_content(R"({"error":"Channel not found"})", "application/json");
        }
    });

    // POST /control
    svr.Post(R"(/control)", [](const httplib::Request& req, httplib::Response& res) {
        auto m = parse_json_body(req.body);
        if (!m.count("type")) {
            res.status = 400;
            res.set_content(R"({"error":"Missing command type"})", "application/json");
            return;
        }
        std::string type = m["type"];
        m.erase("type");
        if (send_control_command(type, m)) {
            res.set_content(R"({"status":"success"})", "application/json");
        } else {
            res.status = 400;
            res.set_content(R"({"error":"Command failed"})", "application/json");
        }
    });

    // POST /commands/decode
    svr.Post(R"(/commands/decode)", [](const httplib::Request& req, httplib::Response& res) {
        auto m = parse_json_body(req.body);
        if (!m.count("action")) {
            res.status = 400;
            res.set_content(R"({"error":"Missing action"})", "application/json");
            return;
        }
        if (decode_control(m["action"])) {
            res.set_content(R"({"status":"success"})", "application/json");
        } else {
            res.status = 400;
            res.set_content(R"({"error":"Invalid action"})", "application/json");
        }
    });

    // POST /commands/reboot
    svr.Post(R"(/commands/reboot)", [](const httplib::Request& req, httplib::Response& res) {
        if (reboot_device()) {
            res.set_content(R"({"status":"rebooting"})", "application/json");
        } else {
            res.status = 500;
            res.set_content(R"({"error":"Failed to reboot"})", "application/json");
        }
    });

    // GET /status
    svr.Get(R"(/status)", [](const httplib::Request& req, httplib::Response& res) {
        auto status = get_status();
        res.set_content(status_to_json(status), "application/json");
    });

    // GET /display
    svr.Get(R"(/display)", [](const httplib::Request& req, httplib::Response& res) {
        auto display = get_display();
        res.set_content(display_to_json(display), "application/json");
    });

    // PUT /display
    svr.Put(R"(/display)", [](const httplib::Request& req, httplib::Response& res) {
        auto m = parse_json_body(req.body);
        DisplayConfig conf = get_display();
        if (m.count("layout")) conf.layout = m["layout"];
        if (m.count("scenes")) conf.scenes = std::stoi(m["scenes"]);
        // Windows as comma-separated string for demo
        if (m.count("windows")) {
            conf.windows.clear();
            std::stringstream ss(m["windows"]);
            std::string item;
            while(std::getline(ss, item, ',')) conf.windows.push_back(item);
        }
        if (update_display(conf)) {
            res.set_content(display_to_json(conf), "application/json");
        } else {
            res.status = 400;
            res.set_content(R"({"error":"Failed to update display"})", "application/json");
        }
    });

    std::cout << "HTTP server listening on " << host << ":" << port << std::endl;
    svr.listen(host.c_str(), port);
}

// ==================== Main Entrypoint =======================================

int main(int argc, char* argv[]) {
    std::string host = get_env("HTTP_HOST", "0.0.0.0");
    int port = std::stoi(get_env("HTTP_PORT", "8080"));

    // Device connection info (used in real SDK logic)
    std::string dev_ip = get_env("DEVICE_IP", "192.168.1.64");
    std::string dev_user = get_env("DEVICE_USER", "admin");
    std::string dev_pass = get_env("DEVICE_PASS", "12345");

    run_http_server(host, port);
    return 0;
}
```
**Dependencies:**  
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) (for HTTP server)  
- [tinyxml2](https://github.com/leethomason/tinyxml2) (stubs included, not used in current code but referenced for XML handling if needed)  

**Environment variables required:**  
- `HTTP_HOST` (default: `0.0.0.0`)  
- `HTTP_PORT` (default: `8080`)  
- `DEVICE_IP`, `DEVICE_USER`, `DEVICE_PASS` for device connection

**Build notes:**  
- Link with pthread and OpenSSL if using HTTPS (`-lssl -lcrypto -lpthread`).
- Add `cpp-httplib.h` and `tinyxml2.h/.cpp` to your project.