// hikvision_decoder_matrix_http_driver.cpp

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <json/json.h>

// --- Environment variable helpers ---
std::string get_env(const char* env, const char* defval = "") {
    const char* val = std::getenv(env);
    return val ? std::string(val) : std::string(defval);
}

// --- Device Communication Mockups (Replace with actual SDK calls) ---
struct DeviceStatus {
    std::string decoder_state;
    std::vector<std::string> error_codes;
    int channel_count;
    std::string scene_mode;
    std::string sdk_version;
    std::string network_info;
};

struct ChannelStatus {
    int id;
    bool enabled;
    std::string playback_status;
    std::string loop_decoding_info;
};

struct DeviceConfig {
    std::string display_config;
    std::string wall_config;
    std::string window_config;
    std::string scene_config;
    std::string network_config;
    std::string time_config;
};

std::mutex device_mutex;
DeviceStatus mock_status = {"running", {"0"}, 8, "default", "v5.1.2", "192.168.1.200/24"};
DeviceConfig mock_config = {"dispA", "wall1", "winX", "scene1", "eth0:192.168.1.200", "2024-01-01T00:00:00Z"};
std::vector<ChannelStatus> mock_channels(8, {0, true, "idle", "none"});

void device_init() {
    for (int i = 0; i < 8; ++i) {
        mock_channels[i].id = i;
        mock_channels[i].enabled = true;
        mock_channels[i].playback_status = "idle";
        mock_channels[i].loop_decoding_info = "none";
    }
}

bool device_update_config(const Json::Value& jconf) {
    std::lock_guard<std::mutex> lock(device_mutex);
    if (jconf.isMember("display_config")) mock_config.display_config = jconf["display_config"].asString();
    if (jconf.isMember("wall_config")) mock_config.wall_config = jconf["wall_config"].asString();
    if (jconf.isMember("window_config")) mock_config.window_config = jconf["window_config"].asString();
    if (jconf.isMember("scene_config")) mock_config.scene_config = jconf["scene_config"].asString();
    if (jconf.isMember("network_config")) mock_config.network_config = jconf["network_config"].asString();
    if (jconf.isMember("time_config")) mock_config.time_config = jconf["time_config"].asString();
    return true;
}

DeviceConfig device_get_config() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return mock_config;
}

DeviceStatus device_get_status() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return mock_status;
}

std::vector<ChannelStatus> device_get_channels() {
    std::lock_guard<std::mutex> lock(device_mutex);
    return mock_channels;
}

bool device_execute_command(const Json::Value& jcmd, std::string& result) {
    std::lock_guard<std::mutex> lock(device_mutex);
    if (!jcmd.isMember("command")) {
        result = "Missing 'command'";
        return false;
    }
    std::string cmd = jcmd["command"].asString();
    if (cmd == "device_reboot") {
        mock_status.decoder_state = "rebooting";
        result = "Device rebooting";
        return true;
    }
    if (cmd == "start_decode") {
        int ch = jcmd.get("channel", -1).asInt();
        if (ch >= 0 && ch < (int)mock_channels.size()) {
            mock_channels[ch].playback_status = "decoding";
            result = "Channel decoding started";
            return true;
        }
        result = "Invalid channel";
        return false;
    }
    if (cmd == "stop_decode") {
        int ch = jcmd.get("channel", -1).asInt();
        if (ch >= 0 && ch < (int)mock_channels.size()) {
            mock_channels[ch].playback_status = "idle";
            result = "Channel decoding stopped";
            return true;
        }
        result = "Invalid channel";
        return false;
    }
    if (cmd == "set_scene") {
        mock_status.scene_mode = jcmd.get("scene", "default").asString();
        result = "Scene set";
        return true;
    }
    result = "Unknown command";
    return false;
}

// --- JSON Serialization Helpers ---
std::string device_status_to_json(const DeviceStatus& st) {
    Json::Value root;
    root["decoder_state"] = st.decoder_state;
    root["error_codes"] = Json::arrayValue;
    for (const auto& code : st.error_codes) root["error_codes"].append(code);
    root["channel_count"] = st.channel_count;
    root["scene_mode"] = st.scene_mode;
    root["sdk_version"] = st.sdk_version;
    root["network_info"] = st.network_info;
    Json::StreamWriterBuilder wbuilder;
    return Json::writeString(wbuilder, root);
}

std::string device_config_to_json(const DeviceConfig& conf) {
    Json::Value root;
    root["display_config"] = conf.display_config;
    root["wall_config"] = conf.wall_config;
    root["window_config"] = conf.window_config;
    root["scene_config"] = conf.scene_config;
    root["network_config"] = conf.network_config;
    root["time_config"] = conf.time_config;
    Json::StreamWriterBuilder wbuilder;
    return Json::writeString(wbuilder, root);
}

std::string channels_to_json(const std::vector<ChannelStatus>& chans) {
    Json::Value root(Json::arrayValue);
    for (const auto& ch : chans) {
        Json::Value chj;
        chj["id"] = ch.id;
        chj["enabled"] = ch.enabled;
        chj["playback_status"] = ch.playback_status;
        chj["loop_decoding_info"] = ch.loop_decoding_info;
        root.append(chj);
    }
    Json::StreamWriterBuilder wbuilder;
    return Json::writeString(wbuilder, root);
}

// --- HTTP Server (Simple, single-threaded, for embedded use) ---
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;
};

void write_response(int client_sock, const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_text << "\r\n";
    for (const auto& h : resp.headers)
        oss << h.first << ": " << h.second << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << resp.body;
    std::string out = oss.str();
    send(client_sock, out.c_str(), out.size(), 0);
}

bool read_line(int sock, std::string& line) {
    char c;
    line.clear();
    while (recv(sock, &c, 1, 0) == 1) {
        if (c == '\r') continue;
        if (c == '\n') break;
        line += c;
    }
    return !line.empty();
}

bool parse_http_request(int client_sock, HttpRequest& req) {
    std::string line;
    if (!read_line(client_sock, line)) return false;
    std::istringstream iss(line);
    iss >> req.method >> req.path;
    // Skip HTTP version (not used)
    while (read_line(client_sock, line)) {
        if (line.empty()) break;
        auto pos = line.find(":");
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos+1);
            while (!val.empty() && val[0] == ' ') val.erase(0,1);
            req.headers[key] = val;
        }
    }
    if (req.headers.count("Content-Length")) {
        int content_length = std::stoi(req.headers["Content-Length"]);
        req.body.resize(content_length);
        int read_bytes = 0;
        while (read_bytes < content_length) {
            int r = recv(client_sock, &req.body[read_bytes], content_length - read_bytes, 0);
            if (r <= 0) break;
            read_bytes += r;
        }
    }
    return true;
}

// --- API Endpoint Handlers ---
HttpResponse handle_status() {
    DeviceStatus st = device_get_status();
    HttpResponse resp{200, "OK"};
    resp.headers["Content-Type"] = "application/json";
    resp.body = device_status_to_json(st);
    return resp;
}

HttpResponse handle_channels() {
    auto chans = device_get_channels();
    HttpResponse resp{200, "OK"};
    resp.headers["Content-Type"] = "application/json";
    resp.body = channels_to_json(chans);
    return resp;
}

HttpResponse handle_get_config() {
    auto conf = device_get_config();
    HttpResponse resp{200, "OK"};
    resp.headers["Content-Type"] = "application/json";
    resp.body = device_config_to_json(conf);
    return resp;
}

HttpResponse handle_put_config(const HttpRequest& req) {
    Json::Value jconf;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    std::istringstream iss(req.body);
    if (!Json::parseFromStream(rbuilder, iss, &jconf, &errs)) {
        return {400, "Bad Request", {{"Content-Type","application/json"}},
                "{\"error\":\"Invalid JSON payload\"}"};
    }
    if (device_update_config(jconf))
        return {200, "OK", {{"Content-Type","application/json"}},
                "{\"result\":\"Configuration updated\"}"};
    else
        return {500, "Internal Server Error", {{"Content-Type","application/json"}},
                "{\"error\":\"Failed to update configuration\"}"};
}

HttpResponse handle_post_commands(const HttpRequest& req) {
    Json::Value jcmd;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    std::istringstream iss(req.body);
    if (!Json::parseFromStream(rbuilder, iss, &jcmd, &errs)) {
        return {400, "Bad Request", {{"Content-Type","application/json"}},
                "{\"error\":\"Invalid JSON payload\"}"};
    }
    std::string result;
    if (device_execute_command(jcmd, result))
        return {200, "OK", {{"Content-Type","application/json"}},
                "{\"result\":\"" + result + "\"}"};
    else
        return {400, "Bad Request", {{"Content-Type","application/json"}},
                "{\"error\":\"" + result + "\"}"};
}

HttpResponse handle_post_cmd(const HttpRequest& req) {
    // Same as /commands
    return handle_post_commands(req);
}

// --- Main Routing ---
HttpResponse route_request(const HttpRequest& req) {
    if (req.method == "GET" && req.path == "/status")
        return handle_status();
    if (req.method == "GET" && req.path == "/channels")
        return handle_channels();
    if (req.method == "GET" && req.path == "/config")
        return handle_get_config();
    if (req.method == "PUT" && req.path == "/config")
        return handle_put_config(req);
    if (req.method == "POST" && req.path == "/commands")
        return handle_post_commands(req);
    if (req.method == "POST" && req.path == "/cmd")
        return handle_post_cmd(req);
    return {404, "Not Found", {{"Content-Type","application/json"}},
            "{\"error\":\"Not found\"}"};
}

// --- HTTP Server Main Loop ---
void http_server_loop(const std::string& host, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }
    sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(host.c_str());
    servaddr.sin_port = htons(port);

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(sockfd, (sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind"); close(sockfd); exit(1);
    }
    if (listen(sockfd, 8) < 0) {
        perror("listen"); close(sockfd); exit(1);
    }
    std::cout << "HTTP server running on " << host << ":" << port << std::endl;
    while (1) {
        sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int client_sock = accept(sockfd, (sockaddr*)&cliaddr, &clilen);
        if (client_sock < 0) continue;
        HttpRequest req;
        if (parse_http_request(client_sock, req)) {
            HttpResponse resp = route_request(req);
            write_response(client_sock, resp);
        }
        close(client_sock);
    }
}

// --- Main ---
int main() {
    std::string device_ip = get_env("DEVICE_IP", "192.168.1.200");
    std::string http_host = get_env("HTTP_HOST", "0.0.0.0");
    int http_port = std::stoi(get_env("HTTP_PORT", "8080"));

    device_init();

    http_server_loop(http_host, http_port);
    return 0;
}