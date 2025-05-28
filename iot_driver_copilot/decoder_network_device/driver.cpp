```cpp
#include <iostream>
#include <string>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cstring>
#include <nlohmann/json.hpp>
#include <httplib.h>

// For demonstration, HCNetSDK is not included. In real implementation, include HCNetSDK.h and link with the SDK.
using json = nlohmann::json;

// Configuration via environment variables
struct Config {
    std::string device_ip;
    int device_port;
    std::string device_user;
    std::string device_password;
    int http_port;
    std::string http_host;

    static Config from_env() {
        Config cfg;
        cfg.device_ip = getenv("DEVICE_IP") ? getenv("DEVICE_IP") : "192.168.1.100";
        cfg.device_port = getenv("DEVICE_PORT") ? std::stoi(getenv("DEVICE_PORT")) : 8000;
        cfg.device_user = getenv("DEVICE_USER") ? getenv("DEVICE_USER") : "admin";
        cfg.device_password = getenv("DEVICE_PASSWORD") ? getenv("DEVICE_PASSWORD") : "12345";
        cfg.http_host = getenv("HTTP_HOST") ? getenv("HTTP_HOST") : "0.0.0.0";
        cfg.http_port = getenv("HTTP_PORT") ? std::stoi(getenv("HTTP_PORT")) : 8080;
        return cfg;
    }
};

// ---- Device SDK Abstraction (Simulated) ----
class DeviceSession {
public:
    std::string token;
    bool logged_in = false;
    std::mutex mtx;
    std::map<std::string, json> configs;
    json status;
    DeviceSession() {
        // Simulate some configs and status
        configs["channel"] = json::parse(R"({"channels":[{"id":1,"enabled":true},{"id":2,"enabled":false}]})");
        configs["display"] = json::parse(R"({"resolution":"1920x1080","layout":"2x2"})");
        status = json::parse(R"({"error_code":0,"alarm_state":"none","network":{"ip":"192.168.1.100","mac":"AA:BB:CC:DD:EE:FF"},"health":"ok"})");
    }

    bool login(const std::string& user, const std::string& pwd) {
        std::lock_guard<std::mutex> lock(mtx);
        if(user=="admin" && pwd=="12345") {
            logged_in = true;
            token = std::to_string(std::time(nullptr));
            return true;
        }
        return false;
    }
    void logout() {
        std::lock_guard<std::mutex> lock(mtx);
        logged_in = false;
        token.clear();
    }
    bool is_logged_in(const std::string& t) {
        std::lock_guard<std::mutex> lock(mtx);
        return logged_in && t == token;
    }
    json get_config(const std::string& type) {
        std::lock_guard<std::mutex> lock(mtx);
        if(configs.count(type)) return configs[type];
        return json::object();
    }
    void set_config(const std::string& type, const json& val) {
        std::lock_guard<std::mutex> lock(mtx);
        configs[type] = val;
    }
    json get_status() {
        std::lock_guard<std::mutex> lock(mtx);
        return status;
    }
    void set_status(const json& val) {
        std::lock_guard<std::mutex> lock(mtx);
        status = val;
    }
    // Simulate decode, upgrade, reboot, etc.
    bool decode(const std::string& action, const std::string& mode) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return action=="start" || action=="stop";
    }
    bool upgrade(const json& params) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        status["upgrade"] = "completed";
        return true;
    }
    bool reboot(const std::string& action) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        status["health"] = "rebooted";
        return true;
    }
};

// ---- HTTP Server Implementation ----
class DriverServer {
    Config config;
    DeviceSession device;
    httplib::Server server;
    std::mutex session_mtx;

    // --- Helper Methods ---
    std::string get_token(const httplib::Request& req) {
        if (req.has_header("Authorization")) {
            std::string auth = req.get_header_value("Authorization");
            if (auth.substr(0, 7) == "Bearer ") return auth.substr(7);
        }
        if (req.has_param("token")) return req.get_param_value("token");
        return "";
    }
    bool check_auth(const httplib::Request& req, httplib::Response& res) {
        std::string token = get_token(req);
        if (!device.is_logged_in(token)) {
            res.status = 401;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Unauthorized","code":401})";
            return false;
        }
        return true;
    }
    // --- HTTP Handlers ---
    void handle_login(const httplib::Request& req, httplib::Response& res) {
        try {
            json j = json::parse(req.body);
            std::string user = j.value("username", "");
            std::string pwd = j.value("password", "");
            if(device.login(user, pwd)) {
                json resp = {{"token", device.token}};
                res.status = 200;
                res.set_header("Content-Type", "application/json");
                res.body = resp.dump();
            } else {
                res.status = 401;
                res.set_header("Content-Type", "application/json");
                res.body = R"({"error":"Invalid credentials"})";
            }
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Invalid request body"})";
        }
    }
    void handle_logout(const httplib::Request& req, httplib::Response& res) {
        if(!check_auth(req,res)) return;
        device.logout();
        res.status = 200;
        res.set_header("Content-Type", "application/json");
        res.body = R"({"result":"Logged out"})";
    }
    void handle_get_config(const httplib::Request& req, httplib::Response& res) {
        if(!check_auth(req,res)) return;
        std::string type = req.has_param("type") ? req.get_param_value("type") : "channel";
        json cfg = device.get_config(type);
        if(cfg.empty()) {
            res.status = 404;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Config type not found"})";
        } else {
            res.status = 200;
            res.set_header("Content-Type", "application/json");
            res.body = cfg.dump();
        }
    }
    void handle_put_config(const httplib::Request& req, httplib::Response& res) {
        if(!check_auth(req,res)) return;
        std::string type = req.has_param("type") ? req.get_param_value("type") : "";
        if(type.empty()) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Config type required"})";
            return;
        }
        try {
            json j = json::parse(req.body);
            device.set_config(type, j);
            res.status = 200;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"result":"Config updated"})";
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Invalid request body"})";
        }
    }
    void handle_decode(const httplib::Request& req, httplib::Response& res) {
        if(!check_auth(req,res)) return;
        try {
            json j = json::parse(req.body);
            std::string action = j.value("action", "");
            std::string mode = j.value("mode", "");
            if(action.empty() || mode.empty()) {
                res.status = 400;
                res.set_header("Content-Type", "application/json");
                res.body = R"({"error":"Missing action or mode"})";
                return;
            }
            bool ok = device.decode(action, mode);
            if(ok) {
                res.status = 200;
                res.set_header("Content-Type", "application/json");
                res.body = R"({"result":"Decode command accepted"})";
            } else {
                res.status = 400;
                res.set_header("Content-Type", "application/json");
                res.body = R"({"error":"Invalid decode command"})";
            }
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Invalid request body"})";
        }
    }
    void handle_reboot(const httplib::Request& req, httplib::Response& res) {
        if(!check_auth(req,res)) return;
        try {
            std::string action = "reboot";
            if(!req.body.empty()) {
                json j = json::parse(req.body);
                action = j.value("action", "reboot");
            }
            if(action!="reboot" && action!="shutdown") action="reboot";
            bool ok = device.reboot(action);
            if(ok) {
                res.status = 200;
                res.set_header("Content-Type", "application/json");
                res.body = R"({"result":"Device ")+action+R"("})";
            } else {
                res.status = 500;
                res.set_header("Content-Type", "application/json");
                res.body = R"({"error":"Failed to execute command"})";
            }
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Invalid request body"})";
        }
    }
    void handle_upgrade(const httplib::Request& req, httplib::Response& res) {
        if(!check_auth(req,res)) return;
        try {
            json j = json::parse(req.body);
            bool ok = device.upgrade(j);
            if(ok) {
                res.status = 200;
                res.set_header("Content-Type", "application/json");
                res.body = R"({"result":"Upgrade started"})";
            } else {
                res.status = 500;
                res.set_header("Content-Type", "application/json");
                res.body = R"({"error":"Upgrade failed"})";
            }
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Invalid request body"})";
        }
    }
    void handle_status(const httplib::Request& req, httplib::Response& res) {
        if(!check_auth(req,res)) return;
        json s = device.get_status();
        res.status = 200;
        res.set_header("Content-Type", "application/json");
        res.body = s.dump();
    }

public:
    DriverServer(const Config& cfg) : config(cfg) {
        // RESTful mappings
        server.Post("/login", [&](const httplib::Request& req, httplib::Response& res) { handle_login(req, res); });
        server.Post("/logout", [&](const httplib::Request& req, httplib::Response& res) { handle_logout(req, res); });
        server.Get("/config", [&](const httplib::Request& req, httplib::Response& res) { handle_get_config(req, res); });
        server.Put("/config", [&](const httplib::Request& req, httplib::Response& res) { handle_put_config(req, res); });
        server.Post("/decode", [&](const httplib::Request& req, httplib::Response& res) { handle_decode(req, res); });
        server.Post("/reboot", [&](const httplib::Request& req, httplib::Response& res) { handle_reboot(req, res); });
        server.Post("/upgrade", [&](const httplib::Request& req, httplib::Response& res) { handle_upgrade(req, res); });
        server.Get("/status", [&](const httplib::Request& req, httplib::Response& res) { handle_status(req, res); });
        // 404 fallback
        server.set_not_found_handler([](const httplib::Request&, httplib::Response& res) {
            res.status = 404;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Not found"})";
        });
    }
    void run() {
        std::cout << "[Driver] Listening on " << config.http_host << ":" << config.http_port << std::endl;
        server.listen(config.http_host.c_str(), config.http_port);
    }
};

int main() {
    Config cfg = Config::from_env();
    DriverServer server(cfg);
    server.run();
    return 0;
}
```
*Dependencies:*
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) (header-only, for HTTP server)
- [nlohmann/json](https://github.com/nlohmann/json) (header-only, for JSON parsing)

*Compile example:*
```
g++ -std=c++17 -O2 -o driver main.cpp -lpthread
```

*Environment variables used:*
- DEVICE_IP, DEVICE_PORT, DEVICE_USER, DEVICE_PASSWORD, HTTP_HOST, HTTP_PORT

*Endpoints:*
- `POST /login`      (body: {"username":"...","password":"..."})
- `POST /logout`
- `GET /config?type=...`
- `PUT /config?type=...`  (body: config JSON)
- `POST /decode`     (body: {"action":"start/stop","mode":"dynamic/passive"})
- `POST /reboot`     (body: {"action":"reboot/shutdown"})
- `POST /upgrade`    (body: upgrade JSON)
- `GET /status`

*Authorization:*
- Use `Authorization: Bearer <token>` header or `token=...` query param after login.