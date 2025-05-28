#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <memory>
#include <chrono>

#include "civetweb.h"

// Placeholder for HCNetSDK C++ API
// #include "HCNetSDK.h" // Uncomment and link against actual SDK

// ------------------ Environment Variable Helpers ------------------
std::string getEnvOrDefault(const char* env, const std::string& def) {
    const char* val = std::getenv(env);
    return val ? std::string(val) : def;
}
int getEnvIntOrDefault(const char* env, int def) {
    const char* val = std::getenv(env);
    return val ? std::atoi(val) : def;
}

// ------------------ Device Session Management ------------------
struct DeviceSession {
    std::string session_token;
    std::string username;
    std::string password;
    std::atomic<bool> is_logged_in;
    std::mutex session_mutex;
    // int user_id; // SDK login handle
};

DeviceSession g_session;

// ------------------ JSON Helpers ------------------
std::string jsonError(const std::string& msg) {
    return "{\"success\":false,\"error\":\"" + msg + "\"}";
}
std::string jsonSuccess(const std::string& msg = "") {
    if (msg.empty()) return "{\"success\":true}";
    return "{\"success\":true,\"msg\":\"" + msg + "\"}";
}

// Very basic JSON parser for getting values, for demo purposes
std::string jsonExtract(const std::string& body, const std::string& key) {
    auto pos = body.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\"')) pos++;
    size_t end = pos;
    while (end < body.size() && body[end] != ',' && body[end] != '}' && body[end] != '\"') end++;
    return body.substr(pos, end - pos);
}

// ------------------ Token Generation ------------------
std::string generateSessionToken() {
    static std::atomic<unsigned long> cnt{0};
    std::ostringstream oss;
    oss << std::chrono::steady_clock::now().time_since_epoch().count() << "-" << (cnt++);
    return oss.str();
}

// ------------------ Device SDK Placeholders ------------------

// All these should actually use real calls to HCNetSDK
bool device_login(const std::string& ip, int port, const std::string& user, const std::string& pwd, std::string& token) {
    // int user_id = NET_DVR_Login_V40(...);
    // if (user_id < 0) return false;
    token = generateSessionToken();
    // g_session.user_id = user_id;
    return true;
}
void device_logout() {
    // if (g_session.user_id >= 0) NET_DVR_Logout(g_session.user_id);
}
std::string device_get_status() {
    // Query device via SDK and return JSON
    return "{\"status\":\"ok\",\"channels\":4,\"alarms\":[],\"error_codes\":[]}";
}
std::string device_get_config(const std::string& type) {
    // SDK call, for now dummy response
    return "{\"type\":\"" + type + "\",\"config\":{}}";
}
bool device_set_config(const std::string& type, const std::string& configJson) {
    // Use SDK to set config
    return true;
}
bool device_decode_control(const std::string& action, const std::string& mode, const std::string& details) {
    // SDK decode control
    return true;
}
bool device_reboot(const std::string& params) {
    // SDK reboot
    return true;
}
bool device_upgrade(const std::string& params) {
    // SDK upgrade
    return true;
}

// ------------------ Auth Helpers ------------------
bool checkAuth(struct mg_connection* conn) {
    char auth_token[128];
    if (mg_get_cookie(conn, "session_token", auth_token, sizeof(auth_token)) > 0) {
        std::lock_guard<std::mutex> lk(g_session.session_mutex);
        if (g_session.is_logged_in && g_session.session_token == std::string(auth_token)) return true;
    }
    // Also check "Authorization" header (Bearer token)
    const char* hdr = mg_get_header(conn, "Authorization");
    if (hdr && strstr(hdr, "Bearer ") == hdr) {
        std::lock_guard<std::mutex> lk(g_session.session_mutex);
        if (g_session.is_logged_in && g_session.session_token == std::string(hdr + 7)) return true;
    }
    return false;
}

// ------------------ HTTP Handlers ------------------
int handle_login(struct mg_connection* conn, void* cbdata) {
    char buf[2048];
    int len = mg_read(conn, buf, sizeof(buf)-1);
    buf[len] = 0;

    std::string body(buf, len);
    std::string username = jsonExtract(body, "username");
    std::string password = jsonExtract(body, "password");

    if (username.empty() || password.empty()) {
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Missing credentials").c_str());
        return 200;
    }
    std::string device_ip = getEnvOrDefault("DEVICE_IP", "192.168.1.100");
    int device_port = getEnvIntOrDefault("DEVICE_PORT", 8000);

    std::string token;
    if (!device_login(device_ip, device_port, username, password, token)) {
        mg_printf(conn, "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Device login failed").c_str());
        return 200;
    }
    {
        std::lock_guard<std::mutex> lk(g_session.session_mutex);
        g_session.session_token = token;
        g_session.username = username;
        g_session.password = password;
        g_session.is_logged_in = true;
    }
    mg_printf(conn, "HTTP/1.1 200 OK\r\nSet-Cookie: session_token=%s; HttpOnly\r\nContent-Type: application/json\r\n\r\n", token.c_str());
    mg_printf(conn, "{\"success\":true,\"session_token\":\"%s\"}", token.c_str());
    return 200;
}

int handle_logout(struct mg_connection* conn, void* cbdata) {
    if (!checkAuth(conn)) {
        mg_printf(conn, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Not authenticated").c_str());
        return 200;
    }
    {
        std::lock_guard<std::mutex> lk(g_session.session_mutex);
        g_session.is_logged_in = false;
        g_session.session_token = "";
    }
    device_logout();
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s", jsonSuccess("Logged out").c_str());
    return 200;
}

int handle_status(struct mg_connection* conn, void* cbdata) {
    if (!checkAuth(conn)) {
        mg_printf(conn, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Not authenticated").c_str());
        return 200;
    }
    std::string status = device_get_status();
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s", status.c_str());
    return 200;
}

int handle_get_config(struct mg_connection* conn, void* cbdata) {
    if (!checkAuth(conn)) {
        mg_printf(conn, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Not authenticated").c_str());
        return 200;
    }
    char type[64] = {0};
    mg_get_var(conn->query_string, strlen(conn->query_string), "type", type, sizeof(type));
    std::string cfg = device_get_config(type[0] ? type : "all");
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s", cfg.c_str());
    return 200;
}

int handle_put_config(struct mg_connection* conn, void* cbdata) {
    if (!checkAuth(conn)) {
        mg_printf(conn, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Not authenticated").c_str());
        return 200;
    }
    char type[64] = {0};
    mg_get_var(conn->query_string, strlen(conn->query_string), "type", type, sizeof(type));
    char buf[4096];
    int len = mg_read(conn, buf, sizeof(buf)-1);
    buf[len] = 0;
    if (!device_set_config(type[0] ? type : "all", std::string(buf, len))) {
        mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Failed to set config").c_str());
        return 200;
    }
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s", jsonSuccess().c_str());
    return 200;
}

int handle_command_decode(struct mg_connection* conn, void* cbdata) {
    if (!checkAuth(conn)) {
        mg_printf(conn, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Not authenticated").c_str());
        return 200;
    }
    char buf[4096];
    int len = mg_read(conn, buf, sizeof(buf)-1);
    buf[len] = 0;
    std::string body(buf, len);
    std::string action = jsonExtract(body, "action");
    std::string mode = jsonExtract(body, "mode");
    if (action.empty() || mode.empty()) {
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Missing action or mode").c_str());
        return 200;
    }
    if (!device_decode_control(action, mode, body)) {
        mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Decode control failed").c_str());
        return 200;
    }
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s", jsonSuccess().c_str());
    return 200;
}

// POST /decode -- alias for /command/decode
int handle_decode(struct mg_connection* conn, void* cbdata) {
    return handle_command_decode(conn, cbdata);
}

// POST /command/reboot
int handle_command_reboot(struct mg_connection* conn, void* cbdata) {
    if (!checkAuth(conn)) {
        mg_printf(conn, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Not authenticated").c_str());
        return 200;
    }
    char buf[1024];
    int len = mg_read(conn, buf, sizeof(buf)-1);
    buf[len] = 0;
    if (!device_reboot(std::string(buf, len))) {
        mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Reboot failed").c_str());
        return 200;
    }
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s", jsonSuccess().c_str());
    return 200;
}

// POST /reboot
int handle_reboot(struct mg_connection* conn, void* cbdata) {
    return handle_command_reboot(conn, cbdata);
}

// POST /command/upgrade
int handle_command_upgrade(struct mg_connection* conn, void* cbdata) {
    if (!checkAuth(conn)) {
        mg_printf(conn, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Not authenticated").c_str());
        return 200;
    }
    char buf[4096];
    int len = mg_read(conn, buf, sizeof(buf)-1);
    buf[len] = 0;
    if (!device_upgrade(std::string(buf, len))) {
        mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n%s", jsonError("Upgrade failed").c_str());
        return 200;
    }
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s", jsonSuccess().c_str());
    return 200;
}

// POST /upgrade
int handle_upgrade(struct mg_connection* conn, void* cbdata) {
    return handle_command_upgrade(conn, cbdata);
}

// ------------------ Main ------------------
int main(int argc, char* argv[]) {
    const char* options[] = {
        "listening_ports", getEnvOrDefault("HTTP_PORT", "8080").c_str(),
        "num_threads", "8",
        "enable_auth_domain_check", "no",
        nullptr
    };
    struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    struct mg_context* ctx = mg_start(&callbacks, nullptr, options);

    // Session status
    g_session.is_logged_in = false;

    mg_set_request_handler(ctx, "/login", handle_login, nullptr);
    mg_set_request_handler(ctx, "/logout", handle_logout, nullptr);
    mg_set_request_handler(ctx, "/status", handle_status, nullptr);
    mg_set_request_handler(ctx, "/config", handle_get_config, nullptr); // GET
    mg_set_request_handler(ctx, "/config", handle_put_config, nullptr); // PUT
    mg_set_request_handler(ctx, "/command/decode", handle_command_decode, nullptr);
    mg_set_request_handler(ctx, "/decode", handle_decode, nullptr);
    mg_set_request_handler(ctx, "/command/reboot", handle_command_reboot, nullptr);
    mg_set_request_handler(ctx, "/reboot", handle_reboot, nullptr);
    mg_set_request_handler(ctx, "/command/upgrade", handle_command_upgrade, nullptr);
    mg_set_request_handler(ctx, "/upgrade", handle_upgrade, nullptr);

    printf("Hikvision Decoder HTTP Driver listening on port %s\n", getEnvOrDefault("HTTP_PORT", "8080").c_str());
    printf("Press Ctrl+C to stop.\n");
    // Block forever
    while (true) std::this_thread::sleep_for(std::chrono::seconds(60));
    mg_stop(ctx);
    return 0;
}