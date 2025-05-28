#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <map>
#include <vector>
#include <sstream>
#include <fstream>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <functional>
#include <ctime>
#include <json/json.h> // Requires jsoncpp (header-only, no command execution)
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSESOCK closesocket
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define CLOSESOCK close
#endif

// -- Device SDK integration points (Stub/Mock Section) --
// In a real driver, these would be actual calls to the Hikvision SDK (HCNetSDK).
// Here, they are mocked for demonstration.

struct DecoderChannel {
    int id;
    bool enabled;
    std::string name;
    std::string status;
};

struct DeviceStatus {
    std::string decoderState;
    std::string alarmStatus;
    std::string remotePlayStatus;
    std::string firmwareUpgrade;
};

struct DisplayConfig {
    std::string layout;
    std::string scene;
};

std::vector<DecoderChannel> mockChannels = {
    {1, true, "MainDecoder1", "Active"},
    {2, false, "MainDecoder2", "Inactive"},
    {3, true, "MainDecoder3", "Active"}
};
DeviceStatus mockDeviceStatus = {"Running", "NoAlarms", "Stopped", "Idle"};
DisplayConfig mockDisplayConfig = {"2x2", "Default"};

// Mock 'SDK' functions
std::mutex device_mutex;
std::vector<DecoderChannel> device_get_channels() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return mockChannels;
}
DecoderChannel* device_find_channel(int id) {
    std::lock_guard<std::mutex> lock(device_mutex);
    for (auto& ch : mockChannels) if (ch.id == id) return &ch;
    return nullptr;
}
void device_update_channel(int id, bool enabled, const std::string& name) {
    std::lock_guard<std::mutex> lock(device_mutex);
    for (auto& ch : mockChannels) {
        if (ch.id == id) {
            ch.enabled = enabled;
            if (!name.empty()) ch.name = name;
        }
    }
}
DeviceStatus device_get_status() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return mockDeviceStatus;
}
void device_set_display_config(const std::string& layout, const std::string& scene) {
    std::lock_guard<std::mutex> lock(device_mutex);
    if (!layout.empty()) mockDisplayConfig.layout = layout;
    if (!scene.empty()) mockDisplayConfig.scene = scene;
}
void device_control_command(const std::string& type, const Json::Value& params) {
    std::lock_guard<std::mutex> lock(device_mutex);
    if (type == "reset")
        mockDeviceStatus.decoderState = "Resetting";
    else if (type == "start_decode")
        mockDeviceStatus.decoderState = "Decoding";
    else if (type == "stop_decode")
        mockDeviceStatus.decoderState = "Stopped";
    // ... Add more command emulation as required
}

// -- End Mock Section --

// -- HTTP Server Utilities --

const int BUFSIZE = 8192;

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::map<std::string, std::string> query_params;
    std::string path_param; // Used for /channels/{id}
};

struct HttpResponse {
    int code;
    std::string status;
    std::string content_type;
    std::string body;
    std::vector<std::string> extra_headers;
};

void url_decode_inplace(std::string &s) {
    size_t len = s.length();
    std::string res;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '%' && i + 2 < len) {
            int val = 0;
            sscanf(s.substr(i + 1, 2).c_str(), "%x", &val);
            res += static_cast<char>(val);
            i += 2;
        } else if (s[i] == '+') {
            res += ' ';
        } else {
            res += s[i];
        }
    }
    s.swap(res);
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> params;
    std::stringstream ss(query);
    std::string item;
    while (std::getline(ss, item, '&')) {
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = item.substr(0, eq);
            std::string v = item.substr(eq + 1);
            url_decode_inplace(k); url_decode_inplace(v);
            params[k] = v;
        }
    }
    return params;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

// Parse HTTP request from raw buffer
bool parse_http_request(const std::string& raw, HttpRequest& req) {
    std::istringstream iss(raw);
    std::string line;
    if (!std::getline(iss, line)) return false;
    size_t method_end = line.find(' ');
    size_t path_end = line.find(' ', method_end + 1);
    if (method_end == std::string::npos || path_end == std::string::npos) return false;
    req.method = line.substr(0, method_end);
    std::string url = line.substr(method_end + 1, path_end - method_end - 1);

    size_t qmark = url.find('?');
    if (qmark != std::string::npos) {
        req.path = url.substr(0, qmark);
        req.query_params = parse_query(url.substr(qmark + 1));
    } else {
        req.path = url;
    }
    std::transform(req.path.begin(), req.path.end(), req.path.begin(), ::tolower);

    // Headers
    while (std::getline(iss, line) && line != "\r") {
        if (line.empty() || line == "\n" || line == "\r\n") break;
        size_t col = line.find(':');
        if (col != std::string::npos) {
            std::string k = line.substr(0, col);
            std::string v = line.substr(col + 1);
            k.erase(std::remove_if(k.begin(), k.end(), ::isspace), k.end());
            v.erase(0, v.find_first_not_of(" \t\r\n"));
            v.erase(v.find_last_not_of(" \t\r\n") + 1);
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            req.headers[k] = v;
        }
    }
    // Body
    std::string rest((std::istreambuf_iterator<char>(iss)), {});
    req.body = rest;

    // Extract path_param for /channels/{id}
    if (starts_with(req.path, "/channels/")) {
        req.path_param = req.path.substr(std::string("/channels/").length());
        req.path = "/channels/{id}";
    }

    return true;
}

void send_http_response(int client_fd, const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.code << " " << resp.status << "\r\n";
    oss << "Content-Type: " << resp.content_type << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    for (const auto& h : resp.extra_headers)
        oss << h << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << resp.body;
    std::string response = oss.str();
    send(client_fd, response.c_str(), response.size(), 0);
}

// -- API Handlers --

HttpResponse handle_get_channels(const HttpRequest& req) {
    auto channels = device_get_channels();
    // Filtering and pagination (optional, basic demo)
    int page = 1, page_size = channels.size();
    if (req.query_params.count("page")) page = std::stoi(req.query_params.at("page"));
    if (req.query_params.count("page_size")) page_size = std::stoi(req.query_params.at("page_size"));
    int from = (page - 1) * page_size;
    int to = std::min((int)channels.size(), from + page_size);

    Json::Value root(Json::arrayValue);
    for (int i = from; i < to; ++i) {
        Json::Value ch;
        ch["id"] = channels[i].id;
        ch["enabled"] = channels[i].enabled;
        ch["name"] = channels[i].name;
        ch["status"] = channels[i].status;
        root.append(ch);
    }
    Json::StreamWriterBuilder builder;
    std::string body = Json::writeString(builder, root);
    return {200, "OK", "application/json", body, {}};
}

HttpResponse handle_get_status(const HttpRequest& req) {
    DeviceStatus status = device_get_status();
    Json::Value root;
    root["decoderState"] = status.decoderState;
    root["alarmStatus"] = status.alarmStatus;
    root["remotePlayStatus"] = status.remotePlayStatus;
    root["firmwareUpgrade"] = status.firmwareUpgrade;
    Json::StreamWriterBuilder builder;
    std::string body = Json::writeString(builder, root);
    return {200, "OK", "application/json", body, {}};
}

HttpResponse handle_put_display(const HttpRequest& req) {
    Json::Value payload;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream iss(req.body);
    if (!Json::parseFromStream(reader, iss, &payload, &errs)) {
        return {400, "Bad Request", "application/json", "{\"error\":\"Invalid JSON payload.\"}", {}};
    }
    std::string layout = payload.get("layout", "").asString();
    std::string scene = payload.get("scene", "").asString();
    device_set_display_config(layout, scene);

    Json::Value root;
    root["result"] = "success";
    Json::StreamWriterBuilder builder;
    std::string body = Json::writeString(builder, root);
    return {200, "OK", "application/json", body, {}};
}

HttpResponse handle_put_channel_id(const HttpRequest& req) {
    int id = std::stoi(req.path_param);
    Json::Value payload;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream iss(req.body);
    if (!Json::parseFromStream(reader, iss, &payload, &errs)) {
        return {400, "Bad Request", "application/json", "{\"error\":\"Invalid JSON payload.\"}", {}};
    }
    bool enabled = payload.get("enabled", false).asBool();
    std::string name = payload.get("name", "").asString();
    if (!device_find_channel(id)) {
        return {404, "Not Found", "application/json", "{\"error\":\"Channel not found.\"}", {}};
    }
    device_update_channel(id, enabled, name);
    Json::Value root; root["result"] = "success";
    Json::StreamWriterBuilder builder;
    std::string body = Json::writeString(builder, root);
    return {200, "OK", "application/json", body, {}};
}

HttpResponse handle_post_control(const HttpRequest& req) {
    Json::Value payload;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream iss(req.body);
    if (!Json::parseFromStream(reader, iss, &payload, &errs)) {
        return {400, "Bad Request", "application/json", "{\"error\":\"Invalid JSON payload.\"}", {}};
    }
    std::string type = payload.get("command", "").asString();
    Json::Value params = payload.get("params", Json::Value(Json::objectValue));
    if (type.empty())
        return {400, "Bad Request", "application/json", "{\"error\":\"Missing command type.\"}", {}};
    device_control_command(type, params);

    Json::Value root; root["result"] = "success";
    Json::StreamWriterBuilder builder;
    std::string body = Json::writeString(builder, root);
    return {200, "OK", "application/json", body, {}};
}

// -- Main HTTP Router --

HttpResponse route_request(const HttpRequest& req) {
    if (req.method == "GET" && req.path == "/channels")
        return handle_get_channels(req);
    if (req.method == "GET" && req.path == "/status")
        return handle_get_status(req);
    if (req.method == "PUT" && req.path == "/display")
        return handle_put_display(req);
    if (req.method == "PUT" && req.path == "/channels/{id}")
        return handle_put_channel_id(req);
    if (req.method == "POST" && req.path == "/control")
        return handle_post_control(req);
    return {404, "Not Found", "application/json", "{\"error\":\"Not found.\"}", {}};
}

// -- HTTP Server Loop --

void handle_client(int client_fd) {
    char buffer[BUFSIZE + 1];
    int n = recv(client_fd, buffer, BUFSIZE, 0);
    if (n <= 0) { CLOSESOCK(client_fd); return; }
    buffer[n] = '\0';
    std::string raw(buffer, n);
    HttpRequest req;
    if (!parse_http_request(raw, req)) {
        HttpResponse resp = {400, "Bad Request", "application/json", "{\"error\":\"Malformed request.\"}", {}};
        send_http_response(client_fd, resp);
        CLOSESOCK(client_fd);
        return;
    }
    HttpResponse resp = route_request(req);
    send_http_response(client_fd, resp);
    CLOSESOCK(client_fd);
}

int main() {
    // --- Configuration from environment ---
    std::string device_ip = getenv("DEVICE_IP") ? getenv("DEVICE_IP") : "127.0.0.1";
    int device_port = getenv("DEVICE_PORT") ? std::stoi(getenv("DEVICE_PORT")) : 8000; // Not used in mock
    std::string http_host = getenv("HTTP_HOST") ? getenv("HTTP_HOST") : "0.0.0.0";
    int http_port = getenv("HTTP_PORT") ? std::stoi(getenv("HTTP_PORT")) : 8080;

#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2,2), &wsa_data);
#endif

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "Socket error\n"; return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = (http_host == "0.0.0.0") ? INADDR_ANY : inet_addr(http_host.c_str());
    addr.sin_port = htons(http_port);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        return 1;
    }
    listen(server_fd, 10);
    std::cout << "HTTP server running on " << http_host << ":" << http_port << std::endl;

    while (true) {
        sockaddr_in client_addr; socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;
        std::thread th(handle_client, client_fd); th.detach();
    }

    CLOSESOCK(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}