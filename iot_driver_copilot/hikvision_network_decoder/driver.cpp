#include <iostream>
#include <cstdlib>
#include <string>
#include <map>
#include <sstream>
#include <thread>
#include <mutex>
#include <vector>
#include <fstream>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <json/json.h>

// ==== CONFIGURATION FROM ENVIRONMENT ====
std::string get_env(const std::string& key, const std::string& def = "") {
    const char* val = getenv(key.c_str());
    return val ? val : def;
}

const std::string DEVICE_IP       = get_env("DEVICE_IP", "192.168.1.100");
const std::string DEVICE_PORT     = get_env("DEVICE_PORT", "8000");
const std::string DEVICE_USER     = get_env("DEVICE_USER", "admin");
const std::string DEVICE_PASS     = get_env("DEVICE_PASS", "12345");
const std::string SERVER_HOST     = get_env("SERVER_HOST", "0.0.0.0");
const int         SERVER_PORT     = std::stoi(get_env("SERVER_PORT", "8080"));

// ==== SESSION TOKEN MANAGEMENT ====
std::mutex session_mutex;
std::string g_session_token;
std::time_t g_token_expiry = 0;
bool is_authenticated(const std::string& token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    return !g_session_token.empty() && g_session_token == token && std::time(nullptr) < g_token_expiry;
}
std::string create_token() {
    std::lock_guard<std::mutex> lock(session_mutex);
    g_session_token = std::to_string(std::time(nullptr)) + "_token";
    g_token_expiry = std::time(nullptr) + 3600; // 1 hour
    return g_session_token;
}
void clear_token() {
    std::lock_guard<std::mutex> lock(session_mutex);
    g_session_token.clear();
    g_token_expiry = 0;
}

// ==== MOCK HIKVISION SDK COMMUNICATION ====
struct DeviceStatus {
    std::string sdk_state = "connected";
    std::string alarm_status = "normal";
    std::string error_code = "0";
    std::vector<std::string> channel_status = {"online", "online", "offline"};
    std::string playback_status = "stopped";
};

struct DeviceConfig {
    std::map<std::string, std::string> config_map = {
        {"channel_count", "8"},
        {"loop_decode", "enabled"},
        {"scene", "default"},
        {"display_mode", "16:9"}
    };
};

DeviceStatus g_device_status;
DeviceConfig g_device_config;

bool device_login(const std::string& user, const std::string& pass) {
    // Pretend to connect to device
    return user == DEVICE_USER && pass == DEVICE_PASS;
}
void device_logout() {
    // Pretend to logout
}
DeviceStatus device_get_status() {
    // Return status struct with mock data
    return g_device_status;
}
DeviceConfig device_get_config() {
    return g_device_config;
}
void device_set_config(const Json::Value& json) {
    for (const auto& key : json.getMemberNames()) {
        g_device_config.config_map[key] = json[key].asString();
    }
}
bool device_decode_action(const std::string& action, const Json::Value& params) {
    if (action == "start") g_device_status.playback_status = "playing";
    else if (action == "stop") g_device_status.playback_status = "stopped";
    else return false;
    return true;
}
void device_reboot() {
    g_device_status.sdk_state = "rebooting";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    g_device_status.sdk_state = "connected";
}
bool device_playback(const Json::Value& params) {
    if (params.isMember("action") && params["action"].asString() == "start")
        g_device_status.playback_status = "playing";
    else
        g_device_status.playback_status = "stopped";
    return true;
}

// ==== HTTP SERVER IMPLEMENTATION ====
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status;
    std::string status_text;
    std::string content_type;
    std::string body;
    std::map<std::string, std::string> headers;
};

std::string url_decode(const std::string& in) {
    std::string out;
    char ch;
    int i, ii;
    for (i=0; i<in.length(); i++) {
        if (in[i] == '%') {
            sscanf(in.substr(i+1,2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            out += ch;
            i = i+2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> params;
    std::istringstream ss(query);
    std::string kv;
    while (std::getline(ss, kv, '&')) {
        auto eq = kv.find('=');
        if (eq != std::string::npos) {
            params[url_decode(kv.substr(0, eq))] = url_decode(kv.substr(eq+1));
        }
    }
    return params;
}

HttpRequest parse_http_request(const std::string& raw) {
    std::istringstream ss(raw);
    std::string line, method, path, ver;
    std::getline(ss, line);
    std::istringstream lss(line);
    lss >> method >> path >> ver;
    std::string actual_path = path, query;
    auto qpos = path.find('?');
    if (qpos != std::string::npos) {
        actual_path = path.substr(0, qpos);
        query = path.substr(qpos+1);
    }
    std::map<std::string, std::string> headers;
    while (std::getline(ss, line) && line != "\r") {
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon+1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val = val.substr(1);
            val.erase(std::remove(val.end()-1, val.end(), '\r'), val.end());
            headers[key] = val;
        }
    }
    std::string body;
    if (headers.count("Content-Length")) {
        int cl = std::stoi(headers["Content-Length"]);
        body.resize(cl);
        ss.read(&body[0], cl);
    }
    HttpRequest req{method, actual_path, query, headers, body};
    return req;
}

std::string http_status_text(int code) {
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
        default:  return "Unknown";
    }
}

std::string serialize_http_response(const HttpResponse& resp) {
    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << ' ' << resp.status_text << "\r\n";
    out << "Content-Type: " << resp.content_type << "\r\n";
    for (const auto& kv : resp.headers) {
        out << kv.first << ": " << kv.second << "\r\n";
    }
    out << "Content-Length: " << resp.body.size() << "\r\n";
    out << "Connection: close\r\n";
    out << "\r\n";
    out << resp.body;
    return out.str();
}

// ==== API HANDLERS ====
HttpResponse handle_login(const HttpRequest& req) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(req.body, root) || !root.isMember("username") || !root.isMember("password")) {
        return {400, http_status_text(400), "application/json", "{\"error\":\"username and password required\"}"};
    }
    std::string u = root["username"].asString();
    std::string p = root["password"].asString();
    if (!device_login(u, p)) {
        return {401, http_status_text(401), "application/json", "{\"error\":\"Invalid credentials\"}"};
    }
    std::string token = create_token();
    Json::Value out;
    out["token"] = token;
    out["expires_in"] = 3600;
    Json::FastWriter writer;
    return {200, http_status_text(200), "application/json", writer.write(out)};
}

HttpResponse handle_logout(const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (!is_authenticated(token)) {
        return {401, http_status_text(401), "application/json", "{\"error\":\"Not authenticated\"}"};
    }
    device_logout();
    clear_token();
    return {200, http_status_text(200), "application/json", "{\"result\":\"Logged out\"}"};
}

HttpResponse handle_status(const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (!is_authenticated(token)) {
        return {401, http_status_text(401), "application/json", "{\"error\":\"Not authenticated\"}"};
    }
    DeviceStatus st = device_get_status();
    Json::Value out;
    out["sdk_state"] = st.sdk_state;
    out["alarm_status"] = st.alarm_status;
    out["error_code"] = st.error_code;
    out["channel_status"] = Json::arrayValue;
    for (auto& ch : st.channel_status) out["channel_status"].append(ch);
    out["playback_status"] = st.playback_status;
    Json::FastWriter writer;
    return {200, http_status_text(200), "application/json", writer.write(out)};
}

HttpResponse handle_config_get(const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (!is_authenticated(token)) {
        return {401, http_status_text(401), "application/json", "{\"error\":\"Not authenticated\"}"};
    }
    DeviceConfig conf = device_get_config();
    Json::Value out;
    for (const auto& kv : conf.config_map) out[kv.first] = kv.second;
    Json::FastWriter writer;
    return {200, http_status_text(200), "application/json", writer.write(out)};
}

HttpResponse handle_config_put(const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (!is_authenticated(token)) {
        return {401, http_status_text(401), "application/json", "{\"error\":\"Not authenticated\"}"};
    }
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(req.body, json)) {
        return {400, http_status_text(400), "application/json", "{\"error\":\"Invalid JSON\"}"};
    }
    device_set_config(json);
    return {200, http_status_text(200), "application/json", "{\"result\":\"Configuration updated\"}"};
}

HttpResponse handle_decode(const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (!is_authenticated(token)) {
        return {401, http_status_text(401), "application/json", "{\"error\":\"Not authenticated\"}"};
    }
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(req.body, json) || !json.isMember("action")) {
        return {400, http_status_text(400), "application/json", "{\"error\":\"Missing action key\"}"};
    }
    std::string action = json["action"].asString();
    if (action != "start" && action != "stop") {
        return {400, http_status_text(400), "application/json", "{\"error\":\"Action must be start or stop\"}"};
    }
    if (!device_decode_action(action, json)) {
        return {500, http_status_text(500), "application/json", "{\"error\":\"Decode action failed\"}"};
    }
    return {200, http_status_text(200), "application/json", "{\"result\":\"Decode " + action + "ed\"}"};
}

HttpResponse handle_reboot(const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (!is_authenticated(token)) {
        return {401, http_status_text(401), "application/json", "{\"error\":\"Not authenticated\"}"};
    }
    device_reboot();
    return {200, http_status_text(200), "application/json", "{\"result\":\"Device rebooted\"}"};
}

HttpResponse handle_playback(const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (!is_authenticated(token)) {
        return {401, http_status_text(401), "application/json", "{\"error\":\"Not authenticated\"}"};
    }
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(req.body, json)) {
        return {400, http_status_text(400), "application/json", "{\"error\":\"Invalid JSON\"}"};
    }
    if (!device_playback(json)) {
        return {500, http_status_text(500), "application/json", "{\"error\":\"Playback operation failed\"}"};
    }
    return {200, http_status_text(200), "application/json", "{\"result\":\"Playback updated\"}"};
}

// ==== HTTP ROUTER ====
HttpResponse route_request(const HttpRequest& req) {
    if (req.method == "POST" && req.path == "/login") return handle_login(req);
    if (req.method == "POST" && req.path == "/logout") return handle_logout(req);
    if (req.method == "GET"  && req.path == "/status") return handle_status(req);
    if (req.method == "GET"  && req.path == "/config") return handle_config_get(req);
    if (req.method == "PUT"  && req.path == "/config") return handle_config_put(req);
    if (req.method == "POST" && req.path == "/decode") return handle_decode(req);
    if (req.method == "POST" && req.path == "/reboot") return handle_reboot(req);
    if (req.method == "POST" && req.path == "/playback") return handle_playback(req);
    return {404, http_status_text(404), "application/json", "{\"error\":\"Endpoint not found\"}"};
}

// ==== SERVER THREAD ====
void handle_client(int client_sock) {
    char buffer[8192];
    ssize_t len = recv(client_sock, buffer, sizeof(buffer)-1, 0);
    if (len <= 0) { close(client_sock); return; }
    buffer[len] = '\0';
    std::string raw_req(buffer, len);
    HttpRequest req = parse_http_request(raw_req);
    HttpResponse resp = route_request(req);
    std::string out = serialize_http_response(resp);
    send(client_sock, out.c_str(), out.size(), 0);
    close(client_sock);
}

void http_server() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket()"); exit(1); }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(SERVER_HOST.c_str());
    addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind()"); exit(1);
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen()"); exit(1);
    }
    std::cout << "HTTP server listening on " << SERVER_HOST << ":" << SERVER_PORT << std::endl;
    while (true) {
        sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int client_fd = accept(server_fd, (sockaddr*)&caddr, &clen);
        if (client_fd < 0) continue;
        std::thread(handle_client, client_fd).detach();
    }
}

// ==== MAIN ENTRY ====
int main() {
    http_server();
    return 0;
}