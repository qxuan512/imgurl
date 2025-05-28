#include <iostream>
#include <string>
#include <cstdlib>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <json/json.h> // Requires https://github.com/open-source-parsers/jsoncpp
#ifdef _WIN32
#include <winsock2.h>
typedef int socklen_t;
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

// --- Environment Helpers ---
std::string getEnv(const std::string& key, const std::string& def = "") {
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : def;
}

// --- Global Configuration ---
struct Config {
    std::string device_ip;
    int device_port;
    std::string username;
    std::string password;
    std::string http_host;
    int http_port;
    std::mutex session_mutex;
    std::string session_token;
    bool session_active = false;
    // Simulated device state for demo
    Json::Value device_status;
    Json::Value device_config;
    Config() {
        device_ip = getEnv("DEVICE_IP", "192.168.1.100");
        device_port = std::stoi(getEnv("DEVICE_PORT", "8000"));
        username = getEnv("DEVICE_USER", "admin");
        password = getEnv("DEVICE_PASS", "admin123");
        http_host = getEnv("HTTP_HOST", "0.0.0.0");
        http_port = std::stoi(getEnv("HTTP_PORT", "8080"));
        // Simulated defaults for demonstration
        device_status["channels"] = Json::arrayValue;
        device_status["alarms"] = Json::arrayValue;
        device_status["errors"] = Json::arrayValue;
        device_status["sdk_state"] = "idle";
        device_status["playback"] = "stopped";
        device_config["decoder_channels"] = 4;
        device_config["loop_decode"] = false;
        device_config["scene"] = "default";
        device_config["window_management"] = "auto";
    }
} config;

// --- Utility: URL Decoding, String Split ---
std::string urlDecode(const std::string& s) {
    std::string result;
    char ch;
    int i, ii;
    for (i = 0; i < s.length(); i++) {
        if (int(s[i]) == 37) {
            sscanf(s.substr(i + 1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            result += ch;
            i = i + 2;
        } else {
            result += s[i];
        }
    }
    return result;
}
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) out.push_back(item);
    return out;
}
std::string trim(const std::string& s) {
    auto start = s.begin();
    while (start != s.end() && isspace(*start)) start++;
    auto end = s.end();
    do { end--; } while (distance(start, end) > 0 && isspace(*end));
    return std::string(start, end + 1);
}

// --- HTTP Utilities ---
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status;
    std::string statusText;
    std::string contentType;
    std::string body;
    std::map<std::string, std::string> headers;
};
std::string getStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}
std::string makeHttpResponse(const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status << " " << resp.statusText << "\r\n";
    oss << "Content-Type: " << resp.contentType << "\r\n";
    oss << "Server: Hikvision-Decoder-HTTP-Driver/1.0\r\n";
    for (const auto& h : resp.headers) {
        oss << h.first << ": " << h.second << "\r\n";
    }
    oss << "Content-Length: " << resp.body.size() << "\r\n\r\n";
    oss << resp.body;
    return oss.str();
}
HttpRequest parseHttpRequest(const std::string& raw) {
    HttpRequest req;
    std::istringstream iss(raw);
    std::string line;
    std::getline(iss, line);
    auto parts = split(line, ' ');
    if (parts.size() >= 2) {
        req.method = parts[0];
        auto pathq = parts[1];
        auto qpos = pathq.find('?');
        if (qpos != std::string::npos) {
            req.path = pathq.substr(0, qpos);
            req.query = pathq.substr(qpos + 1);
        } else {
            req.path = pathq;
            req.query = "";
        }
    }
    while (std::getline(iss, line) && line != "\r") {
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = trim(line.substr(0, colon));
            std::string val = trim(line.substr(colon + 1));
            if (!val.empty() && val.back() == '\r') val.pop_back();
            req.headers[key] = val;
        }
    }
    // Body
    std::string body;
    while (std::getline(iss, line)) {
        body += line + "\n";
    }
    req.body = body;
    if (!req.body.empty() && req.body.back() == '\n') req.body.pop_back();
    return req;
}
// --- Session/Auth Token Simulation ---
std::string generateSessionToken() {
    char buf[33];
    srand((unsigned)time(0));
    for (int i = 0; i < 32; ++i) {
        int n = rand() % 16;
        buf[i] = "0123456789abcdef"[n];
    }
    buf[32] = 0;
    return std::string(buf);
}
bool checkAuthToken(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return false;
    std::lock_guard<std::mutex> lock(config.session_mutex);
    return config.session_active && it->second == "Bearer " + config.session_token;
}

// --- Simulated Device SDK/Protocol Functions ---
bool device_login(const std::string& user, const std::string& pass, std::string& token) {
    if (user == config.username && pass == config.password) {
        token = generateSessionToken();
        std::lock_guard<std::mutex> lock(config.session_mutex);
        config.session_token = token;
        config.session_active = true;
        return true;
    }
    return false;
}
void device_logout() {
    std::lock_guard<std::mutex> lock(config.session_mutex);
    config.session_active = false;
    config.session_token.clear();
}
bool device_reboot() {
    // Simulate reboot
    std::lock_guard<std::mutex> lock(config.session_mutex);
    config.session_active = false;
    config.session_token.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    config.session_active = true;
    config.session_token = generateSessionToken();
    return true;
}
Json::Value device_get_status(const std::map<std::string, std::string>& filters) {
    Json::Value out = config.device_status;
    if (!filters.empty()) {
        // Simulate filtering
        Json::Value filtered = Json::objectValue;
        for (const auto& kv : filters) {
            if (out.isMember(kv.first)) filtered[kv.first] = out[kv.first];
        }
        return filtered;
    }
    return out;
}
Json::Value device_get_config(const std::map<std::string, std::string>& filters) {
    Json::Value out = config.device_config;
    if (!filters.empty()) {
        Json::Value filtered = Json::objectValue;
        for (const auto& kv : filters) {
            if (out.isMember(kv.first)) filtered[kv.first] = out[kv.first];
        }
        return filtered;
    }
    return out;
}
bool device_set_config(const Json::Value& changes) {
    for (const auto& key : changes.getMemberNames()) {
        config.device_config[key] = changes[key];
    }
    return true;
}
bool device_decode_control(const std::string& action, const Json::Value& params) {
    if (action == "start") {
        config.device_status["sdk_state"] = "decoding";
    } else if (action == "stop") {
        config.device_status["sdk_state"] = "idle";
    } else {
        return false;
    }
    return true;
}
bool device_playback_control(const Json::Value& params) {
    // Simulate parameter parsing
    if (params.isMember("action")) {
        std::string action = params["action"].asString();
        if (action == "start") {
            config.device_status["playback"] = "playing";
        } else if (action == "stop") {
            config.device_status["playback"] = "stopped";
        }
    }
    return true;
}

// --- HTTP Route Handlers ---
HttpResponse handle_login(const HttpRequest& req) {
    HttpResponse resp;
    resp.contentType = "application/json";
    Json::Value body;
    Json::CharReaderBuilder builder;
    Json::Value payload;
    std::string errs;
    if (!Json::parseFromStream(builder, std::istringstream(req.body), &payload, &errs)) {
        resp.status = 400; resp.statusText = getStatusText(400);
        body["error"] = "Invalid JSON";
        resp.body = Json::writeString(Json::StreamWriterBuilder(), body);
        return resp;
    }
    std::string user = payload.get("username", "").asString();
    std::string pass = payload.get("password", "").asString();
    std::string token;
    if (device_login(user, pass, token)) {
        resp.status = 200; resp.statusText = getStatusText(200);
        body["token"] = token;
    } else {
        resp.status = 401; resp.statusText = getStatusText(401);
        body["error"] = "Authentication failed";
    }
    resp.body = Json::writeString(Json::StreamWriterBuilder(), body);
    return resp;
}
HttpResponse handle_logout(const HttpRequest& req) {
    HttpResponse resp;
    resp.contentType = "application/json";
    if (!checkAuthToken(req)) {
        resp.status = 401; resp.statusText = getStatusText(401);
        resp.body = "{\"error\":\"Invalid or missing token\"}";
        return resp;
    }
    device_logout();
    resp.status = 200; resp.statusText = getStatusText(200);
    resp.body = "{\"status\":\"logged out\"}";
    return resp;
}
HttpResponse handle_status(const HttpRequest& req) {
    HttpResponse resp;
    resp.contentType = "application/json";
    if (!checkAuthToken(req)) {
        resp.status = 401; resp.statusText = getStatusText(401);
        resp.body = "{\"error\":\"Unauthorized\"}";
        return resp;
    }
    // Parse query params for filtering
    std::map<std::string, std::string> filters;
    if (!req.query.empty()) {
        auto kvs = split(req.query, '&');
        for (const auto& kv : kvs) {
            auto sep = kv.find('=');
            if (sep != std::string::npos) filters[urlDecode(kv.substr(0, sep))] = urlDecode(kv.substr(sep + 1));
        }
    }
    Json::Value status = device_get_status(filters);
    resp.status = 200; resp.statusText = getStatusText(200);
    resp.body = Json::writeString(Json::StreamWriterBuilder(), status);
    return resp;
}
HttpResponse handle_config_get(const HttpRequest& req) {
    HttpResponse resp;
    resp.contentType = "application/json";
    if (!checkAuthToken(req)) {
        resp.status = 401; resp.statusText = getStatusText(401);
        resp.body = "{\"error\":\"Unauthorized\"}";
        return resp;
    }
    std::map<std::string, std::string> filters;
    if (!req.query.empty()) {
        auto kvs = split(req.query, '&');
        for (const auto& kv : kvs) {
            auto sep = kv.find('=');
            if (sep != std::string::npos) filters[urlDecode(kv.substr(0, sep))] = urlDecode(kv.substr(sep + 1));
        }
    }
    Json::Value configJson = device_get_config(filters);
    resp.status = 200; resp.statusText = getStatusText(200);
    resp.body = Json::writeString(Json::StreamWriterBuilder(), configJson);
    return resp;
}
HttpResponse handle_config_put(const HttpRequest& req) {
    HttpResponse resp;
    resp.contentType = "application/json";
    if (!checkAuthToken(req)) {
        resp.status = 401; resp.statusText = getStatusText(401);
        resp.body = "{\"error\":\"Unauthorized\"}";
        return resp;
    }
    Json::Value changes;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, std::istringstream(req.body), &changes, &errs)) {
        resp.status = 400; resp.statusText = getStatusText(400);
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return resp;
    }
    if (device_set_config(changes)) {
        resp.status = 200; resp.statusText = getStatusText(200);
        resp.body = "{\"status\":\"config updated\"}";
    } else {
        resp.status = 500; resp.statusText = getStatusText(500);
        resp.body = "{\"error\":\"Config update failed\"}";
    }
    return resp;
}
HttpResponse handle_decode(const HttpRequest& req) {
    HttpResponse resp;
    resp.contentType = "application/json";
    if (!checkAuthToken(req)) {
        resp.status = 401; resp.statusText = getStatusText(401);
        resp.body = "{\"error\":\"Unauthorized\"}";
        return resp;
    }
    Json::Value payload;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, std::istringstream(req.body), &payload, &errs)) {
        resp.status = 400; resp.statusText = getStatusText(400);
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return resp;
    }
    std::string action = payload.get("action", "").asString();
    if (action.empty()) {
        resp.status = 400; resp.statusText = getStatusText(400);
        resp.body = "{\"error\":\"Missing 'action' field\"}";
        return resp;
    }
    if (device_decode_control(action, payload)) {
        resp.status = 200; resp.statusText = getStatusText(200);
        resp.body = "{\"status\":\"decode " + action + "\"}";
    } else {
        resp.status = 400; resp.statusText = getStatusText(400);
        resp.body = "{\"error\":\"Invalid decode action\"}";
    }
    return resp;
}
HttpResponse handle_playback(const HttpRequest& req) {
    HttpResponse resp;
    resp.contentType = "application/json";
    if (!checkAuthToken(req)) {
        resp.status = 401; resp.statusText = getStatusText(401);
        resp.body = "{\"error\":\"Unauthorized\"}";
        return resp;
    }
    Json::Value payload;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, std::istringstream(req.body), &payload, &errs)) {
        resp.status = 400; resp.statusText = getStatusText(400);
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return resp;
    }
    if (device_playback_control(payload)) {
        resp.status = 200; resp.statusText = getStatusText(200);
        resp.body = "{\"status\":\"playback updated\"}";
    } else {
        resp.status = 500; resp.statusText = getStatusText(500);
        resp.body = "{\"error\":\"Playback control failed\"}";
    }
    return resp;
}
HttpResponse handle_reboot(const HttpRequest& req) {
    HttpResponse resp;
    resp.contentType = "application/json";
    if (!checkAuthToken(req)) {
        resp.status = 401; resp.statusText = getStatusText(401);
        resp.body = "{\"error\":\"Unauthorized\"}";
        return resp;
    }
    if (device_reboot()) {
        resp.status = 200; resp.statusText = getStatusText(200);
        resp.body = "{\"status\":\"device rebooted\"}";
    } else {
        resp.status = 500; resp.statusText = getStatusText(500);
        resp.body = "{\"error\":\"Device reboot failed\"}";
    }
    return resp;
}

// --- HTTP Router ---
HttpResponse route(const HttpRequest& req) {
    if (req.path == "/login" && req.method == "POST") {
        return handle_login(req);
    }
    if ((req.path == "/logout") && req.method == "POST") {
        return handle_logout(req);
    }
    if ((req.path == "/status") && req.method == "GET") {
        return handle_status(req);
    }
    if ((req.path == "/config") && req.method == "GET") {
        return handle_config_get(req);
    }
    if ((req.path == "/config") && req.method == "PUT") {
        return handle_config_put(req);
    }
    if ((req.path == "/decode") && req.method == "POST") {
        return handle_decode(req);
    }
    if ((req.path == "/playback") && req.method == "POST") {
        return handle_playback(req);
    }
    if ((req.path == "/reboot") && req.method == "POST") {
        return handle_reboot(req);
    }
    // 404
    HttpResponse resp;
    resp.status = 404; resp.statusText = getStatusText(404);
    resp.contentType = "application/json";
    resp.body = "{\"error\":\"Endpoint not found\"}";
    return resp;
}

// --- HTTP Server ---
void http_server() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Socket creation failed.\n";
        exit(1);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.http_port);
    addr.sin_addr.s_addr = inet_addr(config.http_host.c_str());
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed.\n";
        exit(1);
    }
    listen(server_fd, 10);
    std::cout << "HTTP server listening on " << config.http_host << ":" << config.http_port << "\n";
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;
        std::thread([client_fd]() {
            char buffer[8192];
            int len = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
            if (len <= 0) {
#ifdef _WIN32
                closesocket(client_fd);
#else
                close(client_fd);
#endif
                return;
            }
            buffer[len] = 0;
            HttpRequest req = parseHttpRequest(buffer);
            HttpResponse resp = route(req);
            std::string out = makeHttpResponse(resp);
            send(client_fd, out.c_str(), out.size(), 0);
#ifdef _WIN32
            closesocket(client_fd);
#else
            close(client_fd);
#endif
        }).detach();
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

// --- Main ---
int main() {
    http_server();
    return 0;
}