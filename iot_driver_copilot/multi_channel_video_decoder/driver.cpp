```cpp
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <memory>
#include <cctype>
#include <functional>
#include <json/json.h> // Requires jsoncpp (header only or system install)
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#endif

// --------------------- Configuration ---------------------
struct Config {
    std::string device_ip;
    int device_port;
    std::string device_user;
    std::string device_pass;
    std::string http_host;
    int http_port;

    Config() {
        device_ip   = getenv("DEVICE_IP")   ? getenv("DEVICE_IP")   : "192.168.0.64";
        device_port = getenv("DEVICE_PORT") ? atoi(getenv("DEVICE_PORT")) : 8000;
        device_user = getenv("DEVICE_USER") ? getenv("DEVICE_USER") : "admin";
        device_pass = getenv("DEVICE_PASS") ? getenv("DEVICE_PASS") : "12345";
        http_host   = getenv("HTTP_HOST")   ? getenv("HTTP_HOST")   : "0.0.0.0";
        http_port   = getenv("HTTP_PORT")   ? atoi(getenv("HTTP_PORT")) : 8080;
    }
};

Config config;

// --------------------- HTTP Utilities ---------------------
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status;
    std::string status_msg;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string url_decode(const std::string& src) {
    std::string ret;
    char ch;
    int i, ii;
    for (i=0; i<src.length(); i++) {
        if (int(src[i])==37) {
            sscanf(src.substr(i+1,2).c_str(), "%x", &ii);
            ch=static_cast<char>(ii);
            ret+=ch;
            i=i+2;
        } else {
            ret+=src[i];
        }
    }
    return ret;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> result;
    size_t start = 0;
    while (start < query.length()) {
        size_t eq = query.find('=', start);
        size_t amp = query.find('&', start);
        if (eq == std::string::npos) break;
        std::string key = url_decode(query.substr(start, eq-start));
        std::string value = url_decode(query.substr(eq+1, (amp==std::string::npos?query.length():amp)-eq-1));
        result[key] = value;
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return result;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

void split_path(const std::string& req_path, std::string& path, std::string& query) {
    size_t qpos = req_path.find('?');
    if (qpos == std::string::npos) {
        path = req_path;
        query = "";
    } else {
        path = req_path.substr(0, qpos);
        query = req_path.substr(qpos+1);
    }
}

HttpRequest parse_http_request(const std::string& raw) {
    HttpRequest req;
    std::istringstream iss(raw);
    std::string line;
    getline(iss, line);
    size_t m = line.find(' ');
    size_t p = line.find(' ', m+1);
    req.method = line.substr(0, m);
    std::string req_path = line.substr(m+1, p-m-1);
    std::string path, query;
    split_path(req_path, path, query);
    req.path = path;
    req.query = parse_query(query);

    // Headers
    while (getline(iss, line) && line != "\r" && line != "") {
        size_t d = line.find(':');
        if (d != std::string::npos) {
            std::string key = line.substr(0, d);
            std::string value = line.substr(d+1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of("\r\n")+1);
            req.headers[key] = value;
        }
    }
    // Body
    std::string body;
    while (getline(iss, line)) {
        body += line + "\n";
    }
    req.body = body;
    return req;
}

std::string http_response_str(const HttpResponse& res) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << res.status << " " << res.status_msg << "\r\n";
    for (const auto& h : res.headers) {
        oss << h.first << ": " << h.second << "\r\n";
    }
    oss << "Content-Length: " << res.body.size() << "\r\n";
    oss << "\r\n";
    oss << res.body;
    return oss.str();
}

void send_http_response(int client_sock, const HttpResponse& res) {
    std::string resp = http_response_str(res);
#ifdef _WIN32
    send(client_sock, resp.c_str(), (int)resp.size(), 0);
    closesocket(client_sock);
#else
    ::send(client_sock, resp.c_str(), resp.size(), 0);
    close(client_sock);
#endif
}

// --------------------- Device Mock SDK Layer ---------------------
/* 
   In production, these would be replaced with actual calls to the Hikvision SDK (HCNetSDK).
   Here, we simulate responses to show the driver structure.
*/

std::mutex sdk_mutex;
struct ChannelStatus {
    int id;
    std::string state;
    std::string display_config;
};
std::vector<ChannelStatus> g_channels = {
    {1, "active", "window-1"}, {2, "inactive", "window-2"}
};

Json::Value sdk_get_channels(const std::map<std::string, std::string>& query) {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    Json::Value arr(Json::arrayValue);
    for (const auto& c : g_channels) {
        if (!query.empty()) {
            if (query.count("id")) {
                if (std::to_string(c.id) != query.at("id"))
                    continue;
            }
            if (query.count("state")) {
                if (c.state != query.at("state"))
                    continue;
            }
        }
        Json::Value v;
        v["id"] = c.id;
        v["state"] = c.state;
        v["display_config"] = c.display_config;
        arr.append(v);
    }
    return arr;
}

Json::Value sdk_update_channels(const Json::Value& payload) {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    for (Json::ArrayIndex i = 0; i < payload.size(); ++i) {
        int id = payload[i].get("id", 0).asInt();
        std::string state = payload[i].get("state", "").asString();
        std::string display_config = payload[i].get("display_config", "").asString();
        for (auto& c : g_channels) {
            if (c.id == id) {
                if (!state.empty()) c.state = state;
                if (!display_config.empty()) c.display_config = display_config;
            }
        }
    }
    Json::Value result;
    result["success"] = true;
    return result;
}

Json::Value sdk_get_status() {
    Json::Value v;
    v["operational_state"] = "online";
    v["error_code"] = 0;
    v["sdk_status"] = "ready";
    v["alarm_status"] = "disarmed";
    v["stream_status"] = "idle";
    v["playback_status"] = "stopped";
    return v;
}

Json::Value sdk_maintain(const Json::Value& payload) {
    std::string cmd = payload.get("command", "").asString();
    Json::Value result;
    if (cmd == "reboot" || cmd == "shutdown" || cmd == "restore" || cmd == "upgrade" || cmd == "activate") {
        result["success"] = true;
        result["action"] = cmd;
    } else {
        result["success"] = false;
        result["error"] = "Unknown command";
    }
    return result;
}

Json::Value sdk_alarm(const Json::Value& payload) {
    std::string state = payload.get("state", "").asString();
    Json::Value result;
    if (state == "arm" || state == "disarm") {
        result["success"] = true;
        result["alarm_state"] = state;
    } else {
        result["success"] = false;
        result["error"] = "Invalid alarm state";
    }
    return result;
}

Json::Value sdk_decode(const Json::Value& payload) {
    std::string action = payload.get("action", "").asString();
    std::string mode = payload.get("mode", "").asString();
    Json::Value result;
    if ((action == "start" || action == "stop") && (mode == "dynamic" || mode == "passive")) {
        result["success"] = true;
        result["action"] = action;
        result["mode"] = mode;
    } else {
        result["success"] = false;
        result["error"] = "Invalid action or mode";
    }
    return result;
}

Json::Value sdk_playback(const Json::Value& payload) {
    Json::Value result;
    result["success"] = true;
    result["playback"] = payload;
    return result;
}

// --------------------- HTTP API Handlers ---------------------
HttpResponse api_channels_get(const HttpRequest& req) {
    Json::Value arr = sdk_get_channels(req.query);
    Json::FastWriter writer;
    HttpResponse res{200, "OK"};
    res.headers["Content-Type"] = "application/json";
    res.body = writer.write(arr);
    return res;
}

HttpResponse api_channels_put(const HttpRequest& req) {
    Json::Reader reader;
    Json::Value payload;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type", "application/json"}}, "{\"error\":\"Invalid JSON\"}"};
    }
    Json::Value result = sdk_update_channels(payload);
    Json::FastWriter writer;
    HttpResponse res{200, "OK"};
    res.headers["Content-Type"] = "application/json";
    res.body = writer.write(result);
    return res;
}

HttpResponse api_status_get(const HttpRequest&) {
    Json::Value status = sdk_get_status();
    Json::FastWriter writer;
    HttpResponse res{200, "OK"};
    res.headers["Content-Type"] = "application/json";
    res.body = writer.write(status);
    return res;
}

HttpResponse api_maintain_post(const HttpRequest& req) {
    Json::Reader reader;
    Json::Value payload;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type", "application/json"}}, "{\"error\":\"Invalid JSON\"}"};
    }
    Json::Value result = sdk_maintain(payload);
    Json::FastWriter writer;
    HttpResponse res{200, "OK"};
    res.headers["Content-Type"] = "application/json";
    res.body = writer.write(result);
    return res;
}

HttpResponse api_alarm_post(const HttpRequest& req) {
    Json::Reader reader;
    Json::Value payload;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type", "application/json"}}, "{\"error\":\"Invalid JSON\"}"};
    }
    Json::Value result = sdk_alarm(payload);
    Json::FastWriter writer;
    HttpResponse res{200, "OK"};
    res.headers["Content-Type"] = "application/json";
    res.body = writer.write(result);
    return res;
}

HttpResponse api_decode_post(const HttpRequest& req) {
    Json::Reader reader;
    Json::Value payload;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type", "application/json"}}, "{\"error\":\"Invalid JSON\"}"};
    }
    Json::Value result = sdk_decode(payload);
    Json::FastWriter writer;
    HttpResponse res{200, "OK"};
    res.headers["Content-Type"] = "application/json";
    res.body = writer.write(result);
    return res;
}

HttpResponse api_playback_post(const HttpRequest& req) {
    Json::Reader reader;
    Json::Value payload;
    if (!reader.parse(req.body, payload)) {
        return {400, "Bad Request", {{"Content-Type", "application/json"}}, "{\"error\":\"Invalid JSON\"}"};
    }
    Json::Value result = sdk_playback(payload);
    Json::FastWriter writer;
    HttpResponse res{200, "OK"};
    res.headers["Content-Type"] = "application/json";
    res.body = writer.write(result);
    return res;
}

// --------------------- HTTP Routing ---------------------
typedef std::function<HttpResponse(const HttpRequest&)> ApiHandler;

struct ApiRoute {
    std::string method;
    std::string path;
    ApiHandler handler;
};

std::vector<ApiRoute> api_routes = {
    {"GET",    "/channels",   api_channels_get},
    {"PUT",    "/channels",   api_channels_put},
    {"GET",    "/status",     api_status_get},
    {"POST",   "/maintain",   api_maintain_post},
    {"POST",   "/alarm",      api_alarm_post},
    {"POST",   "/decode",     api_decode_post},
    {"POST",   "/playback",   api_playback_post}
};

ApiHandler find_handler(const std::string& method, const std::string& path) {
    for (const auto& route : api_routes) {
        if (route.method == method && route.path == path)
            return route.handler;
    }
    return nullptr;
}

// --------------------- HTTP Server ---------------------
void http_server_loop() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(1);
    }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config.http_port);
    if (bind(server_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(server_sock, 16) < 0) {
        perror("listen");
        exit(1);
    }
    printf("HTTP server listening at %s:%d\n", config.http_host.c_str(), config.http_port);

    while (1) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) continue;
        std::thread([client_sock]() {
            char buf[8192];
            int len = recv(client_sock, buf, sizeof(buf)-1, 0);
            if (len <= 0) {
#ifdef _WIN32
                closesocket(client_sock);
#else
                close(client_sock);
#endif
                return;
            }
            buf[len] = 0;
            std::string req_raw(buf);
            HttpRequest req = parse_http_request(req_raw);
            ApiHandler handler = find_handler(req.method, req.path);
            HttpResponse res;
            if (!handler) {
                res = {404, "Not Found", {{"Content-Type","application/json"}}, "{\"error\":\"API not found\"}"};
            } else {
                res = handler(req);
            }
            send_http_response(client_sock, res);
        }).detach();
    }
#ifdef _WIN32
    closesocket(server_sock);
    WSACleanup();
#else
    close(server_sock);
#endif
}

// --------------------- Main ---------------------
int main() {
    http_server_loop();
    return 0;
}
```
