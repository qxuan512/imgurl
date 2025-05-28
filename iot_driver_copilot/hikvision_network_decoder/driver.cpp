#include <iostream>
#include <cstdlib>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <stdexcept>

// =========== Minimal HTTP Server Implementation ============

#ifdef _WIN32
#include <winsock2.h>
typedef int socklen_t;
#pragma comment(lib, "Ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

// =========== Device SDK Protocol Simulation ===============
// In production, replace these with actual SDK API calls

struct Session {
    std::string token;
    bool logged_in = false;
};

struct DeviceStatus {
    std::string channel_status = "OK";
    std::string alarm_status = "None";
    std::string error_codes = "";
    std::string sdk_state = "Running";
};

struct DeviceConfig {
    std::string decoder_channels = "4";
    std::string loop_decode = "Enabled";
    std::string scene = "Default";
};

static std::mutex session_mutex;
static Session device_session;
static DeviceStatus device_status;
static DeviceConfig device_config;

std::string generate_token() {
    static int counter = 1000;
    return "token_" + std::to_string(counter++);
}

bool sdk_login(const std::string& user, const std::string& pass, std::string& out_token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (user == "admin" && pass == "12345") {
        device_session.logged_in = true;
        device_session.token = generate_token();
        out_token = device_session.token;
        return true;
    }
    return false;
}

bool sdk_logout(const std::string& token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (device_session.token == token && device_session.logged_in) {
        device_session.logged_in = false;
        device_session.token = "";
        return true;
    }
    return false;
}

bool sdk_is_logged_in(const std::string& token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    return device_session.logged_in && device_session.token == token;
}

bool sdk_reboot(const std::string& token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (sdk_is_logged_in(token)) {
        // Simulate reboot
        device_status.sdk_state = "Rebooting";
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        device_status.sdk_state = "Running";
        return true;
    }
    return false;
}

bool sdk_get_status(const std::string& token, DeviceStatus& out_status) {
    if (sdk_is_logged_in(token)) {
        out_status = device_status;
        return true;
    }
    return false;
}

bool sdk_get_config(const std::string& token, DeviceConfig& out_config) {
    if (sdk_is_logged_in(token)) {
        out_config = device_config;
        return true;
    }
    return false;
}

bool sdk_update_config(const std::string& token, const DeviceConfig& in_config) {
    if (sdk_is_logged_in(token)) {
        device_config = in_config;
        return true;
    }
    return false;
}

bool sdk_control_decode(const std::string& token, const std::string& action, const std::string& channel, const std::string& mode) {
    if (sdk_is_logged_in(token)) {
        if (action == "start") {
            device_status.channel_status = "Decoding on " + channel + "/" + mode;
        } else if (action == "stop") {
            device_status.channel_status = "Stopped";
        }
        return true;
    }
    return false;
}

bool sdk_playback(const std::string& token, const std::string& action, const std::string& channel, const std::string& speed) {
    if (sdk_is_logged_in(token)) {
        device_status.sdk_state = "Playback: " + action + " on " + channel + " at " + speed;
        return true;
    }
    return false;
}

// ========== JSON Utilities =============
std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (auto c : s) {
        switch (c) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\b': o << "\\b"; break;
        case '\f': o << "\\f"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:
            if ('\x00' <= c && c <= '\x1f') {
                o << "\\u"
                  << std::hex << std::setw(4) << std::setfill('0') << int(c);
            } else {
                o << c;
            }
        }
    }
    return o.str();
}

std::string status_to_json(const DeviceStatus& st) {
    std::ostringstream ss;
    ss << "{"
       << "\"channel_status\":\"" << json_escape(st.channel_status) << "\","
       << "\"alarm_status\":\"" << json_escape(st.alarm_status) << "\","
       << "\"error_codes\":\"" << json_escape(st.error_codes) << "\","
       << "\"sdk_state\":\"" << json_escape(st.sdk_state) << "\""
       << "}";
    return ss.str();
}

std::string config_to_json(const DeviceConfig& cfg) {
    std::ostringstream ss;
    ss << "{"
       << "\"decoder_channels\":\"" << json_escape(cfg.decoder_channels) << "\","
       << "\"loop_decode\":\"" << json_escape(cfg.loop_decode) << "\","
       << "\"scene\":\"" << json_escape(cfg.scene) << "\""
       << "}";
    return ss.str();
}

// ========== HTTP Utilities =============
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status;
    std::string content_type;
    std::string body;
    std::map<std::string, std::string> headers;
};

std::string url_decode(const std::string &SRC) {
    std::string ret;
    char ch;
    int i, ii;
    for (i=0; i<SRC.length(); i++) {
        if (int(SRC[i]) == 37) {
            sscanf(SRC.substr(i+1,2).c_str(), "%x", &ii);
            ch=static_cast<char>(ii);
            ret+=ch;
            i=i+2;
        } else {
            ret+=SRC[i];
        }
    }
    return ret;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> params;
    std::istringstream ss(query);
    std::string item;
    while (std::getline(ss, item, '&')) {
        auto pos = item.find('=');
        if (pos != std::string::npos) {
            params[url_decode(item.substr(0, pos))] = url_decode(item.substr(pos + 1));
        }
    }
    return params;
}

HttpRequest parse_http_request(const std::string& request) {
    HttpRequest req;
    std::istringstream ss(request);
    std::string line;
    std::getline(ss, line);
    std::istringstream ls(line);

    ls >> req.method;
    std::string full_path;
    ls >> full_path;
    auto qpos = full_path.find('?');
    if (qpos != std::string::npos) {
        req.path = full_path.substr(0, qpos);
        req.query = parse_query(full_path.substr(qpos + 1));
    } else {
        req.path = full_path;
    }

    // Headers
    while (std::getline(ss, line) && line != "\r") {
        if (line.empty() || line == "\n" || line == "\r\n") break;
        auto col = line.find(':');
        if (col != std::string::npos) {
            std::string key = line.substr(0, col);
            std::string val = line.substr(col + 1);
            key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            req.headers[key] = val;
        }
    }
    // Body
    std::string body;
    while (std::getline(ss, line)) {
        body += line + "\n";
    }
    if (!body.empty()) {
        req.body = body;
    }
    return req;
}

std::string http_response_to_string(const HttpResponse& resp) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << resp.status << " ";
    switch (resp.status) {
        case 200: ss << "OK"; break;
        case 201: ss << "Created"; break;
        case 204: ss << "No Content"; break;
        case 400: ss << "Bad Request"; break;
        case 401: ss << "Unauthorized"; break;
        case 404: ss << "Not Found"; break;
        case 500: ss << "Internal Server Error"; break;
        default: ss << "Unknown";
    }
    ss << "\r\n";
    ss << "Content-Type: " << resp.content_type << "\r\n";
    for (auto& h : resp.headers) {
        ss << h.first << ": " << h.second << "\r\n";
    }
    ss << "Content-Length: " << resp.body.size() << "\r\n";
    ss << "\r\n";
    ss << resp.body;
    return ss.str();
}

// ========== JSON Parse Helpers (minimal, non-strict) =======
std::map<std::string, std::string> json_to_map(const std::string& json) {
    std::map<std::string, std::string> m;
    std::string s = json;
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
    if (s.front() == '{') s = s.substr(1);
    if (s.back() == '}') s = s.substr(0, s.size()-1);
    std::istringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto c = item.find(':');
        if (c != std::string::npos) {
            std::string k = item.substr(0, c);
            std::string v = item.substr(c+1);
            if (!k.empty() && k.front() == '"') k = k.substr(1, k.size()-2);
            if (!v.empty() && v.front() == '"') v = v.substr(1, v.size()-2);
            m[k] = v;
        }
    }
    return m;
}

// =========== API Handler Functions =========================

HttpResponse handle_login(const HttpRequest& req) {
    auto params = json_to_map(req.body);
    std::string user = params["username"];
    std::string pass = params["password"];
    std::string token;
    if (sdk_login(user, pass, token)) {
        return {200, "application/json", "{\"token\":\"" + token + "\"}", {}};
    }
    return {401, "application/json", "{\"error\":\"Invalid credentials\"}", {}};
}

HttpResponse handle_logout(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
        return {401, "application/json", "{\"error\":\"Missing token\"}", {}};
    }
    std::string token = it->second;
    if (sdk_logout(token)) {
        return {200, "application/json", "{\"result\":\"Logged out\"}", {}};
    }
    return {401, "application/json", "{\"error\":\"Invalid token\"}", {}};
}

HttpResponse handle_status(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
        return {401, "application/json", "{\"error\":\"Missing token\"}", {}};
    }
    DeviceStatus st;
    if (sdk_get_status(it->second, st)) {
        return {200, "application/json", status_to_json(st), {}};
    }
    return {401, "application/json", "{\"error\":\"Invalid token\"}", {}};
}

HttpResponse handle_get_config(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
        return {401, "application/json", "{\"error\":\"Missing token\"}", {}};
    }
    DeviceConfig cfg;
    if (sdk_get_config(it->second, cfg)) {
        return {200, "application/json", config_to_json(cfg), {}};
    }
    return {401, "application/json", "{\"error\":\"Invalid token\"}", {}};
}

HttpResponse handle_put_config(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
        return {401, "application/json", "{\"error\":\"Missing token\"}", {}};
    }
    auto params = json_to_map(req.body);
    DeviceConfig cfg;
    cfg.decoder_channels = params.count("decoder_channels") ? params["decoder_channels"] : device_config.decoder_channels;
    cfg.loop_decode = params.count("loop_decode") ? params["loop_decode"] : device_config.loop_decode;
    cfg.scene = params.count("scene") ? params["scene"] : device_config.scene;
    if (sdk_update_config(it->second, cfg)) {
        return {200, "application/json", "{\"result\":\"Config updated\"}", {}};
    }
    return {401, "application/json", "{\"error\":\"Invalid token\"}", {}};
}

HttpResponse handle_decode(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
        return {401, "application/json", "{\"error\":\"Missing token\"}", {}};
    }
    auto params = json_to_map(req.body);
    std::string action = params.count("action") ? params["action"] : "";
    std::string channel = params.count("channel") ? params["channel"] : "1";
    std::string mode = params.count("mode") ? params["mode"] : "active";
    if (action != "start" && action != "stop") {
        return {400, "application/json", "{\"error\":\"Invalid action\"}", {}};
    }
    if (sdk_control_decode(it->second, action, channel, mode)) {
        return {200, "application/json", "{\"result\":\"" + action + "ed\"}", {}};
    }
    return {401, "application/json", "{\"error\":\"Invalid token or operation\"}", {}};
}

HttpResponse handle_reboot(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
        return {401, "application/json", "{\"error\":\"Missing token\"}", {}};
    }
    if (sdk_reboot(it->second)) {
        return {200, "application/json", "{\"result\":\"Device rebooted\"}", {}};
    }
    return {401, "application/json", "{\"error\":\"Invalid token or operation\"}", {}};
}

HttpResponse handle_playback(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
        return {401, "application/json", "{\"error\":\"Missing token\"}", {}};
    }
    auto params = json_to_map(req.body);
    std::string action = params.count("action") ? params["action"] : "";
    std::string channel = params.count("channel") ? params["channel"] : "1";
    std::string speed = params.count("speed") ? params["speed"] : "1x";
    if (action.empty()) {
        return {400, "application/json", "{\"error\":\"Missing action param\"}", {}};
    }
    if (sdk_playback(it->second, action, channel, speed)) {
        return {200, "application/json", "{\"result\":\"Playback " + action + "\"}", {}};
    }
    return {401, "application/json", "{\"error\":\"Invalid token or operation\"}", {}};
}

// =========== Request Router =========================

HttpResponse route_request(const HttpRequest& req) {
    if (req.method == "POST" && req.path == "/login") return handle_login(req);
    if (req.method == "POST" && req.path == "/logout") return handle_logout(req);
    if (req.method == "GET" && req.path == "/status") return handle_status(req);
    if (req.method == "POST" && req.path == "/decode") return handle_decode(req);
    if (req.method == "PUT" && req.path == "/config") return handle_put_config(req);
    if (req.method == "GET" && req.path == "/config") return handle_get_config(req);
    if (req.method == "POST" && req.path == "/reboot") return handle_reboot(req);
    if (req.method == "POST" && req.path == "/playback") return handle_playback(req);
    return {404, "application/json", "{\"error\":\"Not found\"}", {}};
}

// =========== HTTP Server Main ======================

int create_server_socket(const std::string& host, int port) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("socket failed");
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (host == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else {
        address.sin_addr.s_addr = inet_addr(host.c_str());
    }
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        throw std::runtime_error("bind failed");
    }
    if (listen(server_fd, 10) < 0) {
        throw std::runtime_error("listen failed");
    }
    return server_fd;
}

void handle_client(int client_fd) {
    char buffer[8192];
    int read_bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (read_bytes <= 0) {
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        return;
    }
    buffer[read_bytes] = 0;
    std::string request_str(buffer);
    auto req = parse_http_request(request_str);
    auto resp = route_request(req);
    std::string resp_str = http_response_to_string(resp);
    send(client_fd, resp_str.c_str(), resp_str.size(), 0);
#ifdef _WIN32
    closesocket(client_fd);
#else
    close(client_fd);
#endif
}

int main() {
    // ==== Environment Variables for configuration ====
    const char* device_ip = getenv("DEVICE_IP");
    const char* http_host = getenv("HTTP_SERVER_HOST");
    const char* http_port = getenv("HTTP_SERVER_PORT");
    std::string host = http_host ? http_host : "0.0.0.0";
    int port = http_port ? std::atoi(http_port) : 8080;

    int server_fd = create_server_socket(host, port);
    std::cout << "HTTP server running on " << host << ":" << port << std::endl;

    while (1) {
        sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) continue;
        std::thread(handle_client, client_fd).detach();
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}