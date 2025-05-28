#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <fstream>
#include <streambuf>
#include <ctime>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

// HCNetSDK C++ header simulation (to be replaced with actual SDK headers)
// #include "HCNetSDK.h"

#define BUFFER_SIZE 8192

// --- Utility Functions ---

std::string get_env(const char *key, const char *default_val = "") {
    const char *val = std::getenv(key);
    return val ? std::string(val) : std::string(default_val);
}

std::string now_iso8601() {
    time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%FT%TZ", std::gmtime(&t));
    return buf;
}

std::string http_status_message(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

void send_http_response(int client_fd, int status_code, const std::string &content_type, const std::string &body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << http_status_message(status_code) << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << body;
    std::string response = oss.str();
    send(client_fd, response.c_str(), response.size(), 0);
}

// --- Global State and Device Session Management ---

std::mutex state_mutex;
struct DeviceSession {
    std::string session_token;
    bool logged_in;
    std::string username;
    std::string password;
    // HCNetSDK session info; replace with actual handles
    int user_id;
    DeviceSession() : session_token(""), logged_in(false), user_id(-1) {}
} g_session;

// --- Device SDK Interaction Simulations ---
// In actual implementation, replace these with direct SDK calls.

bool sdk_initialize() {
    // return NET_DVR_Init();
    return true;
}

bool sdk_cleanup() {
    // NET_DVR_Cleanup();
    return true;
}

bool sdk_login(const std::string &ip, int port, const std::string &user, const std::string &pass, int &user_id) {
    // NET_DVR_DEVICEINFO_V30 devinfo;
    // user_id = NET_DVR_Login_V30(ip.c_str(), port, user.c_str(), pass.c_str(), &devinfo);
    // return user_id >= 0;
    user_id = 1; // Simulated user id
    return true;
}

bool sdk_logout(int user_id) {
    // return NET_DVR_Logout(user_id);
    return true;
}

bool sdk_activate(const std::string &ip, int port, const std::string &password) {
    // NET_DVR_ActivateDevice(ip.c_str(), port, password.c_str());
    return true;
}

bool sdk_reboot(int user_id) {
    // NET_DVR_RebootDVR(user_id);
    return true;
}

std::string sdk_get_status(int user_id) {
    // Simulate status as JSON
    std::ostringstream oss;
    oss << "{";
    oss << "\"timestamp\":\"" << now_iso8601() << "\",";
    oss << "\"device\":{\"model\":\"DS-6300D-JX\",\"manufacturer\":\"Hikvision\"},";
    oss << "\"channels\":[{\"id\":1,\"status\":\"online\"},{\"id\":2,\"status\":\"offline\"}],";
    oss << "\"alarms\":[],";
    oss << "\"sdk\":{\"state\":\"active\",\"version\":\"5.7.0\"},";
    oss << "\"error_codes\":[]";
    oss << "}";
    return oss.str();
}

bool sdk_update_display_config(int user_id, const std::string &config_json) {
    // Parse config_json and apply to SDK
    return true;
}

bool sdk_playback_control(int user_id, const std::string &playback_json) {
    // Parse playback_json and send playback commands to SDK
    return true;
}

// --- HTTP Request Parsing ---

struct HttpRequest {
    std::string method;
    std::string path;
    std::string http_version;
    std::map<std::string, std::string> headers;
    std::string body;
};

bool parse_http_request(const std::string &raw, HttpRequest &req) {
    std::istringstream iss(raw);
    std::string line;
    if (!std::getline(iss, line)) return false;
    std::istringstream ls(line);
    ls >> req.method >> req.path >> req.http_version;
    while (std::getline(iss, line) && line != "\r") {
        auto pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            if (!val.empty() && val.back() == '\r') val.pop_back();
            req.headers[key] = val;
        }
    }
    if (req.headers.count("Content-Length")) {
        int content_length = std::stoi(req.headers["Content-Length"]);
        std::string body(content_length, 0);
        iss.read(&body[0], content_length);
        req.body = body;
    }
    return true;
}

// --- Session Token Generation ---

std::string generate_token() {
    static const char alpha[] = "0123456789abcdef";
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i)
        oss << alpha[rand() % (sizeof(alpha) - 1)];
    return oss.str();
}

// --- REST API Endpoints Implementation ---

void handle_sessions_post(int client_fd, const HttpRequest &req, const std::string &device_ip, int device_port) {
    // JSON parse: {"username": "...", "password": "..."}
    std::string user, pass;
    auto u = req.body.find("\"username\"");
    auto p = req.body.find("\"password\"");
    if (u == std::string::npos || p == std::string::npos) {
        send_http_response(client_fd, 400, "application/json", "{\"error\":\"Missing credentials\"}");
        return;
    }
    auto uq = req.body.find(':', u);
    auto pq = req.body.find(':', p);
    if (uq == std::string::npos || pq == std::string::npos) {
        send_http_response(client_fd, 400, "application/json", "{\"error\":\"Malformed JSON\"}");
        return;
    }
    user = req.body.substr(uq + 1, req.body.find(',', uq) - uq - 1);
    pass = req.body.substr(pq + 1, req.body.find('}', pq) - pq - 1);
    user.erase(remove_if(user.begin(), user.end(), [](char c){return c==' '||c=='\"'||c==','||c=='}';}), user.end());
    pass.erase(remove_if(pass.begin(), pass.end(), [](char c){return c==' '||c=='\"'||c==','||c=='}';}), pass.end());

    std::lock_guard<std::mutex> lock(state_mutex);
    if (!sdk_initialize()) {
        send_http_response(client_fd, 500, "application/json", "{\"error\":\"SDK init failed\"}");
        return;
    }
    int user_id = -1;
    if (!sdk_login(device_ip, device_port, user, pass, user_id)) {
        send_http_response(client_fd, 401, "application/json", "{\"error\":\"Login failed\"}");
        return;
    }
    g_session.session_token = generate_token();
    g_session.logged_in = true;
    g_session.username = user;
    g_session.password = pass;
    g_session.user_id = user_id;
    std::ostringstream oss;
    oss << "{\"session_token\":\"" << g_session.session_token << "\"}";
    send_http_response(client_fd, 201, "application/json", oss.str());
}

void handle_sessions_delete(int client_fd, const HttpRequest &req) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!g_session.logged_in) {
        send_http_response(client_fd, 401, "application/json", "{\"error\":\"Not logged in\"}");
        return;
    }
    if (!sdk_logout(g_session.user_id)) {
        send_http_response(client_fd, 500, "application/json", "{\"error\":\"Logout failed\"}");
        return;
    }
    sdk_cleanup();
    g_session = DeviceSession();
    send_http_response(client_fd, 204, "application/json", "");
}

bool check_session(const HttpRequest &req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return false;
    std::string token = it->second;
    if (token.find("Bearer ") == 0) token = token.substr(7);
    std::lock_guard<std::mutex> lock(state_mutex);
    return g_session.logged_in && (g_session.session_token == token);
}

void handle_commands_activate(int client_fd, const HttpRequest &req, const std::string &device_ip, int device_port) {
    if (!check_session(req)) {
        send_http_response(client_fd, 401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    // JSON parse: {"password": "newpassword"} or reuse login password
    std::string password = g_session.password;
    auto p = req.body.find("\"password\"");
    if (p != std::string::npos) {
        auto pq = req.body.find(':', p);
        if (pq != std::string::npos) {
            password = req.body.substr(pq+1, req.body.find('}', pq)-pq-1);
            password.erase(remove_if(password.begin(), password.end(), [](char c){return c==' '||c=='\"'||c==','||c=='}';}), password.end());
        }
    }
    if (!sdk_activate(device_ip, device_port, password)) {
        send_http_response(client_fd, 500, "application/json", "{\"error\":\"Activation failed\"}");
        return;
    }
    send_http_response(client_fd, 200, "application/json", "{\"status\":\"activated\"}");
}

void handle_commands_reboot(int client_fd, const HttpRequest &req) {
    if (!check_session(req)) {
        send_http_response(client_fd, 401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    if (!sdk_reboot(g_session.user_id)) {
        send_http_response(client_fd, 500, "application/json", "{\"error\":\"Reboot failed\"}");
        return;
    }
    send_http_response(client_fd, 200, "application/json", "{\"status\":\"rebooting\"}");
}

void handle_status_get(int client_fd, const HttpRequest &req) {
    if (!check_session(req)) {
        send_http_response(client_fd, 401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    std::string status_json = sdk_get_status(g_session.user_id);
    send_http_response(client_fd, 200, "application/json", status_json);
}

void handle_config_display_put(int client_fd, const HttpRequest &req) {
    if (!check_session(req)) {
        send_http_response(client_fd, 401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    if (!sdk_update_display_config(g_session.user_id, req.body)) {
        send_http_response(client_fd, 500, "application/json", "{\"error\":\"Update failed\"}");
        return;
    }
    send_http_response(client_fd, 200, "application/json", "{\"status\":\"ok\"}");
}

void handle_commands_playback_post(int client_fd, const HttpRequest &req) {
    if (!check_session(req)) {
        send_http_response(client_fd, 401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    if (!sdk_playback_control(g_session.user_id, req.body)) {
        send_http_response(client_fd, 500, "application/json", "{\"error\":\"Playback failed\"}");
        return;
    }
    send_http_response(client_fd, 200, "application/json", "{\"status\":\"ok\"}");
}

// --- HTTP Server Main Loop ---

void handle_client(int client_fd, const std::string &device_ip, int device_port) {
    char buffer[BUFFER_SIZE];
    ssize_t n = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    if (n <= 0) { close(client_fd); return; }
    buffer[n] = '\0';

    HttpRequest req;
    if (!parse_http_request(buffer, req)) {
        send_http_response(client_fd, 400, "application/json", "{\"error\":\"Malformed request\"}");
        close(client_fd);
        return;
    }

    if (req.method == "POST" && req.path == "/sessions") {
        handle_sessions_post(client_fd, req, device_ip, device_port);
    } else if (req.method == "DELETE" && req.path == "/sessions") {
        handle_sessions_delete(client_fd, req);
    } else if (req.method == "POST" && req.path == "/commands/activate") {
        handle_commands_activate(client_fd, req, device_ip, device_port);
    } else if (req.method == "POST" && req.path == "/commands/reboot") {
        handle_commands_reboot(client_fd, req);
    } else if (req.method == "GET" && req.path == "/status") {
        handle_status_get(client_fd, req);
    } else if (req.method == "PUT" && req.path == "/config/display") {
        handle_config_display_put(client_fd, req);
    } else if (req.method == "POST" && req.path == "/commands/playback") {
        handle_commands_playback_post(client_fd, req);
    } else {
        send_http_response(client_fd, 404, "application/json", "{\"error\":\"Unknown endpoint\"}");
    }
    close(client_fd);
}

// --- Entry Point ---

int main() {
    std::string device_ip = get_env("DEVICE_IP", "192.168.1.64");
    int device_port = std::stoi(get_env("DEVICE_PORT", "8000"));
    std::string server_host = get_env("HTTP_HOST", "0.0.0.0");
    int server_port = std::stoi(get_env("HTTP_PORT", "8080"));

    srand(time(NULL));

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) { std::cerr << "Socket error\n"; return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind error\n";
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "Listen error\n";
        return 1;
    }

    std::cout << "HTTP server running on " << server_host << ":" << server_port << "\n";

    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;
        std::thread(handle_client, client_fd, device_ip, device_port).detach();
    }

    close(server_fd);
    return 0;
}