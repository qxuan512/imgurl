```cpp
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#include <sstream>
#include <fstream>
#include <streambuf>
#include <functional>
#include <memory>
#include <chrono>

// Minimal HTTP server library (header-only, no external dependencies)
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

// Minimal JSON (header-only, no external dependencies)
#include "json.hpp"
using json = nlohmann::json;

// --- DEVICE SDK STUBS (replace with real SDK calls for actual integration) ---
class HikvisionSDKSession {
public:
    HikvisionSDKSession(const std::string& ip, uint16_t port)
        : ip_(ip), port_(port), logged_in_(false), token_("") {}

    bool login(const std::string& username, const std::string& password, std::string& out_token) {
        // Simulate device login, generate token
        if (username == "admin" && password == "12345") {
            token_ = "token_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            logged_in_ = true;
            out_token = token_;
            return true;
        }
        return false;
    }
    bool logout(const std::string& token) {
        if (logged_in_ && token == token_) {
            logged_in_ = false;
            token_.clear();
            return true;
        }
        return false;
    }
    bool is_logged_in() const { return logged_in_; }
    const std::string& get_token() const { return token_; }

    json get_status(const json& params) {
        // Simulate device status
        return {
            {"device", "Hikvision Network Decoder"},
            {"channels", {{"1", "online"}, {"2", "offline"}}},
            {"alarms", {{"1", false}, {"2", true}}},
            {"sdk_state", "ok"},
            {"error_codes", json::array()}
        };
    }

    json get_config(const json& params) {
        // Simulate device config
        return {
            {"channels", {{"1", {{"name", "Main"}, {"resolution", "1080p"}}}}},
            {"loop_decode", true},
            {"scene", "default"},
            {"window_management", {{"windows", 4}}}
        };
    }

    bool set_config(const json& config) {
        // Accept any config for simulation
        (void)config;
        return true;
    }

    bool start_decode(const json& params) {
        // Simulate decode start
        (void)params;
        return true;
    }
    bool stop_decode(const json& params) {
        // Simulate decode stop
        (void)params;
        return true;
    }

    bool reboot() {
        // Simulate reboot
        return true;
    }

    bool playback(const json& params) {
        // Simulate playback control
        (void)params;
        return true;
    }

private:
    std::string ip_;
    uint16_t port_;
    bool logged_in_;
    std::string token_;
};
// --- END DEVICE SDK STUBS ---

// --- ENVIRONMENT VARIABLE UTILS ---
static std::string getenv_str(const char* key, const std::string& def = "") {
    const char* v = std::getenv(key);
    return v ? std::string(v) : def;
}

static int getenv_int(const char* key, int def = 0) {
    const char* v = std::getenv(key);
    return v ? std::atoi(v) : def;
}

static uint16_t getenv_uint16(const char* key, uint16_t def = 0) {
    const char* v = std::getenv(key);
    return v ? static_cast<uint16_t>(std::atoi(v)) : def;
}

// --- SESSION MANAGEMENT ---
class SessionManager {
public:
    void set(const std::string& token, std::shared_ptr<HikvisionSDKSession> session) {
        std::lock_guard<std::mutex> lock(mtx_);
        sessions_[token] = session;
    }
    std::shared_ptr<HikvisionSDKSession> get(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = sessions_.find(token);
        return (it != sessions_.end()) ? it->second : nullptr;
    }
    void remove(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx_);
        sessions_.erase(token);
    }
private:
    std::mutex mtx_;
    std::map<std::string, std::shared_ptr<HikvisionSDKSession>> sessions_;
};

SessionManager g_sessions;

// --- AUTH MIDDLEWARE ---
bool check_auth(const httplib::Request& req, std::shared_ptr<HikvisionSDKSession>& session) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end())
        return false;
    std::string token = it->second;
    session = g_sessions.get(token);
    return session != nullptr && session->is_logged_in();
}

// --- MAIN SERVER LOGIC ---
int main() {
    std::string device_ip = getenv_str("DEVICE_IP", "192.168.1.100");
    uint16_t device_port = getenv_uint16("DEVICE_PORT", 8000);
    std::string server_host = getenv_str("HTTP_SERVER_HOST", "0.0.0.0");
    int server_port = getenv_int("HTTP_SERVER_PORT", 8080);

    httplib::Server svr;

    // POST /login
    svr.Post("/login", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto payload = json::parse(req.body);
            std::string username = payload.value("username", "");
            std::string password = payload.value("password", "");
            std::string token;
            auto session = std::make_shared<HikvisionSDKSession>(device_ip, device_port);
            if (session->login(username, password, token)) {
                g_sessions.set(token, session);
                res.status = 200;
                res.set_header("Content-Type", "application/json");
                res.body = json{{"token", token}}.dump();
            } else {
                res.status = 401;
                res.set_header("Content-Type", "application/json");
                res.body = json{{"error", "Invalid credentials"}}.dump();
            }
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Malformed request"}}.dump();
        }
    });

    // POST /logout
    svr.Post("/logout", [&](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<HikvisionSDKSession> session;
        if (!check_auth(req, session)) {
            res.status = 401;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Unauthorized"}}.dump();
            return;
        }
        std::string token = req.headers.find("Authorization")->second;
        if (session->logout(token)) {
            g_sessions.remove(token);
            res.status = 200;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"message", "Logged out"}}.dump();
        } else {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Logout failed"}}.dump();
        }
    });

    // GET /status
    svr.Get("/status", [&](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<HikvisionSDKSession> session;
        if (!check_auth(req, session)) {
            res.status = 401;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Unauthorized"}}.dump();
            return;
        }
        // Extract query params
        json params;
        for (auto& kv : req.params)
            params[kv.first] = kv.second;
        res.status = 200;
        res.set_header("Content-Type", "application/json");
        res.body = session->get_status(params).dump();
    });

    // GET /config
    svr.Get("/config", [&](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<HikvisionSDKSession> session;
        if (!check_auth(req, session)) {
            res.status = 401;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Unauthorized"}}.dump();
            return;
        }
        json params;
        for (auto& kv : req.params)
            params[kv.first] = kv.second;
        res.status = 200;
        res.set_header("Content-Type", "application/json");
        res.body = session->get_config(params).dump();
    });

    // PUT /config
    svr.Put("/config", [&](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<HikvisionSDKSession> session;
        if (!check_auth(req, session)) {
            res.status = 401;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Unauthorized"}}.dump();
            return;
        }
        try {
            auto payload = json::parse(req.body);
            if (session->set_config(payload)) {
                res.status = 200;
                res.set_header("Content-Type", "application/json");
                res.body = json{{"message", "Config updated"}}.dump();
            } else {
                res.status = 500;
                res.set_header("Content-Type", "application/json");
                res.body = json{{"error", "Failed to update config"}}.dump();
            }
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Malformed request"}}.dump();
        }
    });

    // POST /decode
    svr.Post("/decode", [&](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<HikvisionSDKSession> session;
        if (!check_auth(req, session)) {
            res.status = 401;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Unauthorized"}}.dump();
            return;
        }
        try {
            auto payload = json::parse(req.body);
            std::string action = payload.value("action", "");
            if (action == "start") {
                if (session->start_decode(payload)) {
                    res.status = 200;
                    res.set_header("Content-Type", "application/json");
                    res.body = json{{"message", "Decoding started"}}.dump();
                } else {
                    res.status = 500;
                    res.set_header("Content-Type", "application/json");
                    res.body = json{{"error", "Failed to start decode"}}.dump();
                }
            } else if (action == "stop") {
                if (session->stop_decode(payload)) {
                    res.status = 200;
                    res.set_header("Content-Type", "application/json");
                    res.body = json{{"message", "Decoding stopped"}}.dump();
                } else {
                    res.status = 500;
                    res.set_header("Content-Type", "application/json");
                    res.body = json{{"error", "Failed to stop decode"}}.dump();
                }
            } else {
                res.status = 400;
                res.set_header("Content-Type", "application/json");
                res.body = json{{"error", "Missing or invalid action"}}.dump();
            }
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Malformed request"}}.dump();
        }
    });

    // POST /reboot
    svr.Post("/reboot", [&](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<HikvisionSDKSession> session;
        if (!check_auth(req, session)) {
            res.status = 401;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Unauthorized"}}.dump();
            return;
        }
        if (session->reboot()) {
            res.status = 200;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"message", "Device rebooting"}}.dump();
        } else {
            res.status = 500;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Reboot failed"}}.dump();
        }
    });

    // POST /playback
    svr.Post("/playback", [&](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<HikvisionSDKSession> session;
        if (!check_auth(req, session)) {
            res.status = 401;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Unauthorized"}}.dump();
            return;
        }
        try {
            auto payload = json::parse(req.body);
            if (session->playback(payload)) {
                res.status = 200;
                res.set_header("Content-Type", "application/json");
                res.body = json{{"message", "Playback command sent"}}.dump();
            } else {
                res.status = 500;
                res.set_header("Content-Type", "application/json");
                res.body = json{{"error", "Playback command failed"}}.dump();
            }
        } catch (...) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.body = json{{"error", "Malformed request"}}.dump();
        }
    });

    std::cout << "Hikvision HTTP driver listening on " << server_host << ":" << server_port << std::endl;
    svr.listen(server_host.c_str(), server_port);
    return 0;
}
```

**Dependencies (`httplib.h` and `json.hpp`):**
- Get `httplib.h`: https://github.com/yhirose/cpp-httplib (single header)
- Get `json.hpp`: https://github.com/nlohmann/json (single header)
- Place them in your include path or the same directory as this file.

**Environment variables to set:**
- `DEVICE_IP` (default: 192.168.1.100)
- `DEVICE_PORT` (default: 8000)
- `HTTP_SERVER_HOST` (default: 0.0.0.0)
- `HTTP_SERVER_PORT` (default: 8080)
