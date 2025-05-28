#include <iostream>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <cctype>
#include <ctime>
#include <csignal>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

// ----------- HCNetSDK Simulation (stub) ---------------
// In a real implementation, use Hikvision's HCNetSDK APIs.
// Here, we use stubs to simulate device interaction.

struct DeviceStatus {
    std::string device_name = "Decoder Matrix";
    std::string device_model = "DS-64XXHD-S";
    std::string manufacturer = "Hikvision";
    std::string sdk_version = "V5.3.0";
    std::string scene_mode = "Default";
    std::string network_info = "192.168.1.100";
    std::map<int, bool> channel_enabled{{1, true}, {2, false}, {3, true}};
    std::map<int, std::string> channel_state{{1, "Playing"}, {2, "Stopped"}, {3, "Paused"}};
    int error_code = 0;
};

struct DeviceConfig {
    std::string display_mode = "4x4";
    std::string wall_mode = "Single";
    std::string network_mode = "DHCP";
    std::string time = "2024-06-13T12:00:00Z";
};

DeviceStatus g_status;
DeviceConfig g_config;

std::mutex g_data_mutex;

bool sdk_update_config(const DeviceConfig& cfg) {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_config = cfg;
    return true;
}

DeviceConfig sdk_fetch_config() {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    return g_config;
}

DeviceStatus sdk_fetch_status() {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    return g_status;
}

std::vector<std::map<std::string, std::string>> sdk_fetch_channel_status() {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    std::vector<std::map<std::string, std::string>> channels;
    for (const auto& ch : g_status.channel_enabled) {
        std::map<std::string, std::string> ch_info;
        ch_info["channel_id"] = std::to_string(ch.first);
        ch_info["enabled"] = ch.second ? "true" : "false";
        ch_info["state"] = g_status.channel_state[ch.first];
        channels.push_back(ch_info);
    }
    return channels;
}

bool sdk_execute_command(const std::string& cmd, const std::map<std::string, std::string>& params) {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    // Simulate command execution
    if (cmd == "reboot") {
        g_status.error_code = 0;
        return true;
    } else if (cmd == "shutdown") {
        g_status.error_code = 0;
        return true;
    } else if (cmd == "start_decode") {
        int ch = std::stoi(params.at("channel_id"));
        g_status.channel_state[ch] = "Playing";
        return true;
    } else if (cmd == "stop_decode") {
        int ch = std::stoi(params.at("channel_id"));
        g_status.channel_state[ch] = "Stopped";
        return true;
    }
    // Add more command handling as needed
    return false;
}

// ----------- Utility Functions ------------

std::string json_escape(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f') {
                    oss << "\\u" << std::hex << (int)c;
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

std::string device_status_json(const DeviceStatus& status) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"device_name\":\"" << json_escape(status.device_name) << "\",";
    oss << "\"device_model\":\"" << json_escape(status.device_model) << "\",";
    oss << "\"manufacturer\":\"" << json_escape(status.manufacturer) << "\",";
    oss << "\"sdk_version\":\"" << json_escape(status.sdk_version) << "\",";
    oss << "\"scene_mode\":\"" << json_escape(status.scene_mode) << "\",";
    oss << "\"network_info\":\"" << json_escape(status.network_info) << "\",";
    oss << "\"error_code\":" << status.error_code << ",";
    oss << "\"channels\":[";
    bool first = true;
    for (const auto& ch : status.channel_enabled) {
        if (!first) oss << ",";
        first = false;
        oss << "{";
        oss << "\"channel_id\":" << ch.first << ",";
        oss << "\"enabled\":" << (ch.second ? "true" : "false") << ",";
        oss << "\"state\":\"" << json_escape(status.channel_state.at(ch.first)) << "\"";
        oss << "}";
    }
    oss << "]";
    oss << "}";
    return oss.str();
}

std::string device_config_json(const DeviceConfig& config) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"display_mode\":\"" << json_escape(config.display_mode) << "\",";
    oss << "\"wall_mode\":\"" << json_escape(config.wall_mode) << "\",";
    oss << "\"network_mode\":\"" << json_escape(config.network_mode) << "\",";
    oss << "\"time\":\"" << json_escape(config.time) << "\"";
    oss << "}";
    return oss.str();
}

std::string channel_status_json(const std::vector<std::map<std::string, std::string>>& channels) {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const auto& ch : channels) {
        if (!first) oss << ",";
        first = false;
        oss << "{";
        oss << "\"channel_id\":" << ch.at("channel_id") << ",";
        oss << "\"enabled\":" << (ch.at("enabled") == "true" ? "true" : "false") << ",";
        oss << "\"state\":\"" << json_escape(ch.at("state")) << "\"";
        oss << "}";
    }
    oss << "]";
    return oss.str();
}

// ----------- HTTP Server Implementation ----------------

#define BUF_SIZE 8192

struct HttpRequest {
    std::string method;
    std::string path;
    std::string http_version;
    std::map<std::string,std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code;
    std::string status_text;
    std::string content_type;
    std::string body;
};

std::string http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

void send_response(int client_fd, const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_text << "\r\n";
    oss << "Content-Type: " << resp.content_type << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << resp.body;
    std::string data = oss.str();
    send(client_fd, data.c_str(), data.size(), 0);
}

HttpRequest parse_http_request(const std::string& raw) {
    HttpRequest req;
    std::istringstream iss(raw);
    std::string line;
    std::getline(iss, line);
    size_t method_end = line.find(' ');
    size_t path_end = line.find(' ', method_end+1);
    req.method = line.substr(0, method_end);
    req.path = line.substr(method_end+1, path_end-method_end-1);
    req.http_version = line.substr(path_end+1);
    while (std::getline(iss, line) && line != "\r" && !line.empty()) {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon+1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n")+1);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            req.headers[key] = value;
        }
    }
    if (req.headers.count("content-length")) {
        int len = std::stoi(req.headers["content-length"]);
        std::vector<char> buf(len);
        iss.read(buf.data(), len);
        req.body.assign(buf.begin(), buf.begin() + iss.gcount());
    }
    return req;
}

std::map<std::string, std::string> parse_json_object(const std::string& body) {
    // Minimal JSON parser for flat objects: {"key":"value",...}
    std::map<std::string, std::string> kv;
    size_t pos = 0;
    while (true) {
        pos = body.find('"', pos);
        if (pos == std::string::npos) break;
        size_t key_end = body.find('"', pos+1);
        if (key_end == std::string::npos) break;
        std::string key = body.substr(pos+1, key_end-pos-1);
        pos = body.find(':', key_end);
        if (pos == std::string::npos) break;
        pos++;
        while (pos < body.length() && std::isspace(body[pos])) pos++;
        char value_quote = body[pos];
        size_t value_end;
        std::string value;
        if (value_quote == '"') {
            value_end = body.find('"', pos+1);
            value = body.substr(pos+1, value_end-pos-1);
            pos = value_end+1;
        } else {
            value_end = body.find_first_of(",}", pos);
            value = body.substr(pos, value_end-pos);
            pos = value_end;
        }
        kv[key] = value;
        pos = body.find(',', pos);
        if (pos == std::string::npos) break;
        pos++;
    }
    return kv;
}

// ---- API Handlers -----

HttpResponse handle_get_status() {
    DeviceStatus status = sdk_fetch_status();
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = http_status_text(200);
    resp.content_type = "application/json";
    resp.body = device_status_json(status);
    return resp;
}

HttpResponse handle_get_channels() {
    auto chs = sdk_fetch_channel_status();
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = http_status_text(200);
    resp.content_type = "application/json";
    resp.body = channel_status_json(chs);
    return resp;
}

HttpResponse handle_get_config() {
    DeviceConfig cfg = sdk_fetch_config();
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = http_status_text(200);
    resp.content_type = "application/json";
    resp.body = device_config_json(cfg);
    return resp;
}

HttpResponse handle_put_config(const std::string& body) {
    auto params = parse_json_object(body);
    DeviceConfig cfg = sdk_fetch_config();
    if (params.count("display_mode")) cfg.display_mode = params["display_mode"];
    if (params.count("wall_mode")) cfg.wall_mode = params["wall_mode"];
    if (params.count("network_mode")) cfg.network_mode = params["network_mode"];
    if (params.count("time")) cfg.time = params["time"];
    if (!sdk_update_config(cfg)) {
        HttpResponse resp;
        resp.status_code = 500;
        resp.status_text = http_status_text(500);
        resp.content_type = "application/json";
        resp.body = "{\"error\":\"Failed to update config\"}";
        return resp;
    }
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = http_status_text(200);
    resp.content_type = "application/json";
    resp.body = device_config_json(cfg);
    return resp;
}

HttpResponse handle_post_commands(const std::string& body) {
    auto params = parse_json_object(body);
    std::string cmd = params.count("command") ? params["command"] : "";
    if (cmd.empty()) {
        HttpResponse resp;
        resp.status_code = 400;
        resp.status_text = http_status_text(400);
        resp.content_type = "application/json";
        resp.body = "{\"error\":\"Missing command field\"}";
        return resp;
    }
    if (!sdk_execute_command(cmd, params)) {
        HttpResponse resp;
        resp.status_code = 500;
        resp.status_text = http_status_text(500);
        resp.content_type = "application/json";
        resp.body = "{\"error\":\"Failed to execute command\"}";
        return resp;
    }
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = http_status_text(200);
    resp.content_type = "application/json";
    resp.body = "{\"result\":\"success\"}";
    return resp;
}

HttpResponse handle_post_cmd(const std::string& body) {
    // For this stub, identical to /commands
    return handle_post_commands(body);
}

HttpResponse handle_not_found() {
    HttpResponse resp;
    resp.status_code = 404;
    resp.status_text = http_status_text(404);
    resp.content_type = "application/json";
    resp.body = "{\"error\":\"Not found\"}";
    return resp;
}

HttpResponse handle_method_not_allowed() {
    HttpResponse resp;
    resp.status_code = 405;
    resp.status_text = http_status_text(405);
    resp.content_type = "application/json";
    resp.body = "{\"error\":\"Method not allowed\"}";
    return resp;
}

// ----------- Main HTTP Request Dispatcher -------------

HttpResponse dispatch_request(const HttpRequest& req) {
    // Normalize path (ignore trailing slash)
    std::string path = req.path;
    if (path.length() > 1 && path.back() == '/')
        path.pop_back();

    if (path == "/status" && req.method == "GET") {
        return handle_get_status();
    } else if (path == "/channels" && req.method == "GET") {
        return handle_get_channels();
    } else if (path == "/config" && req.method == "GET") {
        return handle_get_config();
    } else if (path == "/config" && req.method == "PUT") {
        return handle_put_config(req.body);
    } else if (path == "/commands" && req.method == "POST") {
        return handle_post_commands(req.body);
    } else if (path == "/cmd" && req.method == "POST") {
        return handle_post_cmd(req.body);
    } else {
        return handle_not_found();
    }
}

// ----------- Server Logic ----------------------------

volatile bool g_terminate = false;

void signal_handler(int signo) {
    g_terminate = true;
}

void handle_client(int client_fd) {
    char buf[BUF_SIZE];
    ssize_t n = recv(client_fd, buf, BUF_SIZE-1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = 0;
    std::string raw_req(buf);
    HttpRequest req = parse_http_request(raw_req);
    HttpResponse resp = dispatch_request(req);
    send_response(client_fd, resp);
    close(client_fd);
}

int main() {
    // Environment Variables
    std::string device_ip = getenv("DEVICE_IP") ? getenv("DEVICE_IP") : "192.168.1.100";
    std::string server_host = getenv("SERVER_HOST") ? getenv("SERVER_HOST") : "0.0.0.0";
    std::string server_port_str = getenv("SERVER_PORT") ? getenv("SERVER_PORT") : "8080";
    int server_port = std::stoi(server_port_str);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (inet_pton(AF_INET, server_host.c_str(), &addr.sin_addr) <= 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        std::cerr << "Listen failed\n";
        close(listen_fd);
        return 1;
    }
    std::cout << "HTTP server listening on " << server_host << ":" << server_port << std::endl;
    while (!g_terminate) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (g_terminate) break;
            continue;
        }
        std::thread([client_fd]() {
            handle_client(client_fd);
        }).detach();
    }
    close(listen_fd);
    std::cout << "Server terminated.\n";
    return 0;
}