#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <json/json.h>
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

// ------------------- Environment Variables Helper -------------------

std::string get_env(const std::string &var, const std::string &def = "") {
    const char *val = std::getenv(var.c_str());
    return val ? std::string(val) : def;
}

int get_env_int(const std::string &var, int def = 0) {
    std::string value = get_env(var);
    return value.empty() ? def : std::stoi(value);
}

// ------------------- HTTP Server Utilities -------------------

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code;
    std::string status_message;
    std::map<std::string, std::string> headers;
    std::string body;
};

void send_response(int client_sock, const HttpResponse &resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_message << "\r\n";
    for (const auto &kv : resp.headers) {
        oss << kv.first << ": " << kv.second << "\r\n";
    }
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << resp.body;
    std::string msg = oss.str();
#ifdef _WIN32
    send(client_sock, msg.c_str(), static_cast<int>(msg.size()), 0);
    closesocket(client_sock);
#else
    ::send(client_sock, msg.c_str(), msg.size(), 0);
    close(client_sock);
#endif
}

void url_decode(std::string &src) {
    std::string ret;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size()) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            ret += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else if (src[i] == '+') {
            ret += ' ';
        } else {
            ret += src[i];
        }
    }
    src = ret;
}

std::map<std::string, std::string> parse_query(const std::string &query) {
    std::map<std::string, std::string> params;
    std::istringstream iss(query);
    std::string kv;
    while (std::getline(iss, kv, '&')) {
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string key = kv.substr(0, eq);
            std::string value = kv.substr(eq + 1);
            url_decode(key);
            url_decode(value);
            params[key] = value;
        }
    }
    return params;
}

HttpRequest parse_http_request(const std::string &request_str) {
    HttpRequest req;
    std::istringstream stream(request_str);
    std::string line;
    std::getline(stream, line);
    line.erase(line.find_last_not_of("\r\n") + 1);
    std::istringstream first_line(line);
    first_line >> req.method;
    std::string path_full;
    first_line >> path_full;
    size_t qpos = path_full.find('?');
    if (qpos != std::string::npos) {
        req.path = path_full.substr(0, qpos);
        req.query = path_full.substr(qpos + 1);
    } else {
        req.path = path_full;
    }

    while (std::getline(stream, line)) {
        if (line == "\r" || line == "\n" || line.empty())
            break;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of("\r\n") + 1);
            req.headers[key] = value;
        }
    }
    std::ostringstream body;
    while (std::getline(stream, line)) {
        body << line << "\n";
    }
    req.body = body.str();
    if (!req.body.empty() && req.body[req.body.size()-1] == '\n')
        req.body.erase(req.body.size()-1);
    return req;
}

// ------------------- Dummy Device SDK Simulation -------------------

// In practice, use Hikvision's HCNetSDK via its C API: here we stub it out for demo.

std::mutex g_dev_mutex;
struct ChannelInfo {
    int id;
    bool enabled;
    std::string status;
    Json::Value config;
};
struct DeviceStatus {
    std::string device_state;
    std::string playback_info;
    std::string network_config;
    std::vector<ChannelInfo> channels;
};

DeviceStatus g_device_status = {
    "online",
    "no playback",
    "192.168.1.100",
    {}
};

void init_device() {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    g_device_status.channels.clear();
    for (int i = 1; i <= 8; ++i) {
        ChannelInfo ch;
        ch.id = i;
        ch.enabled = (i % 2 == 1);
        ch.status = ch.enabled ? "active" : "inactive";
        ch.config["resolution"] = "1920x1080";
        ch.config["mode"] = "normal";
        g_device_status.channels.push_back(ch);
    }
}

void update_display(const Json::Value &config) {
    // Simulate updating display settings
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    // For demo, just store config
    g_device_status.device_state = "display updated";
}

void update_channel(int id, const Json::Value &conf) {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    for (auto &ch : g_device_status.channels) {
        if (ch.id == id) {
            if (conf.isMember("enabled")) ch.enabled = conf["enabled"].asBool();
            if (conf.isMember("resolution")) ch.config["resolution"] = conf["resolution"];
            if (conf.isMember("mode")) ch.config["mode"] = conf["mode"];
            ch.status = ch.enabled ? "active" : "inactive";
        }
    }
}

void set_channel_enable(const Json::Value &payload) {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    for (const auto &ch : payload["channels"]) {
        int id = ch["id"].asInt();
        bool en = ch["enabled"].asBool();
        for (auto &channel : g_device_status.channels) {
            if (channel.id == id)
                channel.enabled = en;
        }
    }
}

void remote_play_control(const Json::Value &cmd) {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    // For demo, just set playback_info
    g_device_status.playback_info = cmd.toStyledString();
}

void reboot_device() {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    g_device_status.device_state = "rebooting";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    g_device_status.device_state = "online";
}

void upgrade_device(const Json::Value &payload) {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    g_device_status.device_state = "upgrading";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    g_device_status.device_state = "online";
}

void decode_control(const Json::Value &payload) {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    g_device_status.device_state = "decoding updated";
}

// ------------------- Handlers -------------------

HttpResponse handle_status(const HttpRequest &req) {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    Json::Value resp;
    resp["device_state"] = g_device_status.device_state;
    resp["playback_info"] = g_device_status.playback_info;
    resp["network_config"] = g_device_status.network_config;
    Json::Value arr(Json::arrayValue);
    for (const auto &ch : g_device_status.channels) {
        Json::Value c;
        c["id"] = ch.id;
        c["enabled"] = ch.enabled;
        c["status"] = ch.status;
        c["config"] = ch.config;
        arr.append(c);
    }
    resp["channels"] = arr;
    HttpResponse r;
    r.status_code = 200;
    r.status_message = "OK";
    r.headers["Content-Type"] = "application/json";
    r.body = resp.toStyledString();
    return r;
}

HttpResponse handle_channel_get(const HttpRequest &req) {
    std::lock_guard<std::mutex> lock(g_dev_mutex);
    auto params = parse_query(req.query);
    int page = params.count("page") ? std::stoi(params["page"]) : 1;
    int limit = params.count("limit") ? std::stoi(params["limit"]) : 8;
    bool filter_enabled = params.count("enabled");
    std::vector<ChannelInfo> filtered;
    for (const auto &ch : g_device_status.channels) {
        if (!filter_enabled || (ch.enabled == (params["enabled"]=="true" || params["enabled"]=="1")))
            filtered.push_back(ch);
    }
    int start = (page-1)*limit, end = std::min(start+limit, (int)filtered.size());
    Json::Value arr(Json::arrayValue);
    for (int i = start; i < end; ++i) {
        Json::Value c;
        c["id"] = filtered[i].id;
        c["enabled"] = filtered[i].enabled;
        c["status"] = filtered[i].status;
        c["config"] = filtered[i].config;
        arr.append(c);
    }
    Json::Value resp;
    resp["channels"] = arr;
    resp["page"] = page;
    resp["limit"] = limit;
    resp["count"] = (int)filtered.size();
    HttpResponse r;
    r.status_code = 200;
    r.status_message = "OK";
    r.headers["Content-Type"] = "application/json";
    r.body = resp.toStyledString();
    return r;
}

HttpResponse handle_channel_put(const HttpRequest &req) {
    Json::Value payload;
    Json::Reader reader;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Malformed JSON\"}"};
    }
    set_channel_enable(payload);
    return {200, "OK", {{"Content-Type","application/json"}}, "{\"result\":\"Channels updated\"}"};
}

HttpResponse handle_channel_id_put(const HttpRequest &req, int id) {
    Json::Value payload;
    Json::Reader reader;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Malformed JSON\"}"};
    }
    update_channel(id, payload);
    return {200, "OK", {{"Content-Type","application/json"}}, "{\"result\":\"Channel updated\"}"};
}

HttpResponse handle_display_post(const HttpRequest &req) {
    Json::Value payload;
    Json::Reader reader;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Malformed JSON\"}"};
    }
    update_display(payload);
    return {200, "OK", {{"Content-Type","application/json"}}, "{\"result\":\"Display updated\"}"};
}

HttpResponse handle_display_put(const HttpRequest &req) {
    return handle_display_post(req);
}

HttpResponse handle_remote_post(const HttpRequest &req) {
    Json::Value payload;
    Json::Reader reader;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Malformed JSON\"}"};
    }
    remote_play_control(payload);
    return {200, "OK", {{"Content-Type","application/json"}}, "{\"result\":\"Remote control executed\"}"};
}

HttpResponse handle_reboot_post(const HttpRequest &req) {
    std::thread([](){ reboot_device(); }).detach();
    return {200, "OK", {{"Content-Type","application/json"}}, "{\"result\":\"Reboot initiated\"}"};
}

HttpResponse handle_command_reboot_post(const HttpRequest &req) {
    return handle_reboot_post(req);
}

HttpResponse handle_update_post(const HttpRequest &req) {
    Json::Value payload;
    Json::Reader reader;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Malformed JSON\"}"};
    }
    std::thread([payload]{ upgrade_device(payload); }).detach();
    return {200, "OK", {{"Content-Type","application/json"}}, "{\"result\":\"Upgrade initiated\"}"};
}

HttpResponse handle_decode_post(const HttpRequest &req) {
    Json::Value payload;
    Json::Reader reader;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Malformed JSON\"}"};
    }
    decode_control(payload);
    return {200, "OK", {{"Content-Type","application/json"}}, "{\"result\":\"Decode operation updated\"}"};
}

// ------------------- HTTP Router -------------------

void process_http(int client_sock) {
    char buffer[8192] = {0};
    int len =
#ifdef _WIN32
        recv(client_sock, buffer, sizeof(buffer) - 1, 0);
#else
        recv(client_sock, buffer, sizeof(buffer) - 1, 0);
#endif
    if (len <= 0) {
#ifdef _WIN32
        closesocket(client_sock);
#else
        close(client_sock);
#endif
        return;
    }
    buffer[len] = 0;
    std::string reqstr(buffer);
    HttpRequest req = parse_http_request(reqstr);

    // Routing
    HttpResponse resp;
    if (req.method == "GET" && req.path == "/status") {
        resp = handle_status(req);
    } else if (req.method == "GET" && req.path == "/channel") {
        resp = handle_channel_get(req);
    } else if (req.method == "PUT" && req.path == "/channel") {
        resp = handle_channel_put(req);
    } else if (req.method == "PUT" && req.path.find("/channel/") == 0) {
        std::string idstr = req.path.substr(std::strlen("/channel/"));
        int id = std::stoi(idstr);
        resp = handle_channel_id_put(req, id);
    } else if (req.method == "POST" && req.path == "/display") {
        resp = handle_display_post(req);
    } else if (req.method == "PUT" && req.path == "/display") {
        resp = handle_display_put(req);
    } else if (req.method == "POST" && req.path == "/remote") {
        resp = handle_remote_post(req);
    } else if (req.method == "POST" && req.path == "/reboot") {
        resp = handle_reboot_post(req);
    } else if (req.method == "POST" && req.path == "/command/reboot") {
        resp = handle_command_reboot_post(req);
    } else if (req.method == "POST" && req.path == "/update") {
        resp = handle_update_post(req);
    } else if (req.method == "POST" && req.path == "/decode") {
        resp = handle_decode_post(req);
    } else {
        resp.status_code = 404;
        resp.status_message = "Not Found";
        resp.headers["Content-Type"] = "application/json";
        resp.body = "{\"error\":\"Not Found\"}";
    }
    send_response(client_sock, resp);
}

// ------------------- Main Server Loop -------------------

int main() {
    std::string device_ip = get_env("DEVICE_IP", "192.168.1.100");
    int http_port = get_env_int("HTTP_PORT", 8080);

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif

    init_device();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(http_port);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
#ifdef _WIN32
        closesocket(server_fd);
        WSACleanup();
#else
        close(server_fd);
#endif
        return 1;
    }
    listen(server_fd, 8);
    std::cout << "HTTP server listening on port " << http_port << std::endl;

    while (true) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_sock =
#ifdef _WIN32
            accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);
#else
            accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);
#endif
        if (client_sock < 0) continue;
        std::thread(process_http, client_sock).detach();
    }

#ifdef _WIN32
    closesocket(server_fd);
    WSACleanup();
#else
    close(server_fd);
#endif
    return 0;
}