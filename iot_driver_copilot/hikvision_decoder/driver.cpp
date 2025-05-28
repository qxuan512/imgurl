#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>
#include <json/json.h>

// ==== Environment variable helpers ====
std::string getenv_or_default(const std::string& var, const std::string& def) {
    const char* val = std::getenv(var.c_str());
    return val ? std::string(val) : def;
}

// ==== Device Model (Mocked for illustration) ====
struct DeviceInfo {
    std::string device_name = "Hikvision Decoder";
    std::string device_model = "DS-6300D(-JX/-T), DS-6400HD(-JX/-T/-S), DS-6500D(-T), DS_64XXHD_S, DS64XXHD_T, DS63XXD_T, DS65XXD";
    std::string manufacturer = "Hikvision";
    std::string device_type = "Decoder";
};

struct ChannelStatus {
    int channel_id;
    std::string status;
    std::string stream_url;
    std::string last_command;
};

struct DeviceStatus {
    std::string sdk_state;
    std::string sdk_version;
    int error_code;
    std::string health;
};

struct DeviceConfig {
    std::string display_mode;
    int decode_channels;
    std::string scene_mode;
};

class HikvisionDevice {
public:
    HikvisionDevice() {
        // Mock some data
        channels = {
            {1, {"1", "idle", "rtsp://192.168.1.10:554/stream1", ""}},
            {2, {"2", "decoding", "rtsp://192.168.1.10:554/stream2", "start"}}
        };
        status = {"active", "v5.3.0", 0, "OK"};
        config = {"wall", 2, "default"};
    }

    DeviceInfo get_info() {
        return {};
    }

    DeviceStatus get_status(bool detail) {
        if (detail) {
            status.health = "All systems normal. Temp=45C, Fans=OK.";
        } else {
            status.health = "OK";
        }
        return status;
    }

    std::vector<ChannelStatus> get_channels(const std::string& channel_id = "") {
        std::vector<ChannelStatus> res;
        if (channel_id.empty()) {
            for (const auto& kv : channels) res.push_back(kv.second);
        } else {
            if (channels.count(std::stoi(channel_id))) {
                res.push_back(channels[std::stoi(channel_id)]);
            }
        }
        return res;
    }

    bool decode_command(const std::string& action, const Json::Value& params, std::string& err_msg) {
        if (action != "start" && action != "stop") {
            err_msg = "Invalid action";
            return false;
        }
        int ch = params.get("channel_id", 0).asInt();
        if (!channels.count(ch)) {
            err_msg = "Channel not found";
            return false;
        }
        channels[ch].last_command = action;
        channels[ch].status = (action == "start") ? "decoding" : "idle";
        return true;
    }

    bool update_config(const Json::Value& j, std::string& err_msg) {
        if (j.isMember("display_mode")) config.display_mode = j["display_mode"].asString();
        if (j.isMember("decode_channels")) config.decode_channels = j["decode_channels"].asInt();
        if (j.isMember("scene_mode")) config.scene_mode = j["scene_mode"].asString();
        // Accept any structure for now
        return true;
    }

private:
    std::map<int, ChannelStatus> channels;
    DeviceStatus status;
    DeviceConfig config;
};

// ==== HTTP Utilities ====
struct HttpRequest {
    std::string method;
    std::string uri;
    std::map<std::string, std::string> headers;
    std::string body;
    std::map<std::string, std::string> query_params;
};

struct HttpResponse {
    int status_code;
    std::string content_type;
    std::string body;
    std::map<std::string, std::string> headers;
};

void send_response(int client, const HttpResponse& res) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << res.status_code << " ";
    switch (res.status_code) {
        case 200: oss << "OK"; break;
        case 201: oss << "Created"; break;
        case 204: oss << "No Content"; break;
        case 400: oss << "Bad Request"; break;
        case 404: oss << "Not Found"; break;
        case 405: oss << "Method Not Allowed"; break;
        case 500: oss << "Internal Server Error"; break;
        default: oss << "Error";
    }
    oss << "\r\n";
    oss << "Content-Type: " << res.content_type << "\r\n";
    for (const auto& kv : res.headers) {
        oss << kv.first << ": " << kv.second << "\r\n";
    }
    oss << "Content-Length: " << res.body.size() << "\r\n";
    oss << "\r\n";
    oss << res.body;
    std::string msg = oss.str();
    send(client, msg.c_str(), msg.size(), 0);
}

// Parse HTTP request (very simple, only for this context)
HttpRequest parse_request(const std::string& raw) {
    HttpRequest req;
    std::istringstream iss(raw);
    std::string line;
    std::getline(iss, line);
    std::istringstream lss(line);
    lss >> req.method >> req.uri;
    // Query params
    size_t qpos = req.uri.find('?');
    if (qpos != std::string::npos) {
        std::string path = req.uri.substr(0, qpos);
        std::string qstr = req.uri.substr(qpos + 1);
        req.uri = path;
        std::istringstream qss(qstr);
        std::string kv;
        while (std::getline(qss, kv, '&')) {
            size_t eq = kv.find('=');
            if (eq != std::string::npos)
                req.query_params[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    // Headers
    while (std::getline(iss, line) && line != "\r") {
        if (line.empty() || line == "\r") break;
        size_t c = line.find(':');
        if (c != std::string::npos) {
            std::string key = line.substr(0, c);
            std::string val = line.substr(c+1);
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            req.headers[key] = val;
        }
    }
    // Body
    std::string body;
    while (std::getline(iss, line)) body += line + "\n";
    if (!body.empty() && body.back() == '\n') body.pop_back();
    req.body = body;
    return req;
}

// ==== API Handlers ====
HttpResponse handle_device(const HttpRequest& req, HikvisionDevice& dev) {
    if (req.method != "GET") return {405, "application/json", "{\"error\":\"Method Not Allowed\"}", {}};
    DeviceInfo info = dev.get_info();
    Json::Value j;
    j["device_name"] = info.device_name;
    j["device_model"] = info.device_model;
    j["manufacturer"] = info.manufacturer;
    j["device_type"] = info.device_type;
    Json::StreamWriterBuilder w;
    return {200, "application/json", Json::writeString(w, j), {}};
}

HttpResponse handle_status(const HttpRequest& req, HikvisionDevice& dev) {
    if (req.method != "GET") return {405, "application/json", "{\"error\":\"Method Not Allowed\"}", {}};
    bool detail = (req.query_params.count("detail") && req.query_params.at("detail") == "1");
    DeviceStatus s = dev.get_status(detail);
    Json::Value j;
    j["sdk_state"] = s.sdk_state;
    j["sdk_version"] = s.sdk_version;
    j["error_code"] = s.error_code;
    j["health"] = s.health;
    Json::StreamWriterBuilder w;
    return {200, "application/json", Json::writeString(w, j), {}};
}

HttpResponse handle_channels(const HttpRequest& req, HikvisionDevice& dev) {
    if (req.method != "GET") return {405, "application/json", "{\"error\":\"Method Not Allowed\"}", {}};
    std::string ch_id = "";
    if (req.query_params.count("channel_id")) ch_id = req.query_params.at("channel_id");
    std::vector<ChannelStatus> channels = dev.get_channels(ch_id);
    Json::Value j;
    for (auto& ch : channels) {
        Json::Value cj;
        cj["channel_id"] = ch.channel_id;
        cj["status"] = ch.status;
        cj["stream_url"] = ch.stream_url;
        cj["last_command"] = ch.last_command;
        j.append(cj);
    }
    Json::StreamWriterBuilder w;
    return {200, "application/json", Json::writeString(w, j), {}};
}

HttpResponse handle_decode_cmd(const HttpRequest& req, HikvisionDevice& dev) {
    if (req.method != "POST") return {405, "application/json", "{\"error\":\"Method Not Allowed\"}", {}};
    Json::CharReaderBuilder rb;
    Json::Value j;
    std::string errs;
    std::istringstream ss(req.body);
    if (!Json::parseFromStream(rb, ss, &j, &errs)) {
        return {400, "application/json", "{\"error\":\"Invalid JSON\"}", {}};
    }
    if (!j.isMember("action")) return {400, "application/json", "{\"error\":\"Missing action\"}", {}};
    std::string err_msg;
    if (!dev.decode_command(j["action"].asString(), j, err_msg)) {
        Json::Value je;
        je["error"] = err_msg;
        Json::StreamWriterBuilder w;
        return {400, "application/json", Json::writeString(w, je), {}};
    }
    return {200, "application/json", "{\"result\":\"OK\"}", {}};
}

HttpResponse handle_config_cmd(const HttpRequest& req, HikvisionDevice& dev) {
    if (req.method != "POST") return {405, "application/json", "{\"error\":\"Method Not Allowed\"}", {}};
    Json::CharReaderBuilder rb;
    Json::Value j;
    std::string errs;
    std::istringstream ss(req.body);
    if (!Json::parseFromStream(rb, ss, &j, &errs)) {
        return {400, "application/json", "{\"error\":\"Invalid JSON\"}", {}};
    }
    std::string err_msg;
    if (!dev.update_config(j, err_msg)) {
        Json::Value je;
        je["error"] = err_msg;
        Json::StreamWriterBuilder w;
        return {400, "application/json", Json::writeString(w, je), {}};
    }
    return {200, "application/json", "{\"result\":\"OK\"}", {}};
}

// ==== HTTP Server Loop ====
void http_server_loop(const std::string& host, int port, HikvisionDevice& dev) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // Accept all
    addr.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(server_fd, 10) < 0) { perror("listen"); exit(1); }
    std::cout << "HTTP server listening on port " << port << std::endl;

    while (true) {
        sockaddr_in client_addr;
        socklen_t ca_len = sizeof(client_addr);
        int client = accept(server_fd, (struct sockaddr*)&client_addr, &ca_len);
        if (client < 0) continue;
        std::thread([client, &dev]() {
            char buf[8192];
            int len = recv(client, buf, sizeof(buf) - 1, 0);
            if (len <= 0) { close(client); return; }
            buf[len] = '\0';
            std::string raw(buf);
            HttpRequest req = parse_request(raw);
            HttpResponse res;
            if (req.uri == "/device") {
                res = handle_device(req, dev);
            } else if (req.uri == "/status") {
                res = handle_status(req, dev);
            } else if (req.uri == "/channels") {
                res = handle_channels(req, dev);
            } else if (req.uri == "/cmd/decode") {
                res = handle_decode_cmd(req, dev);
            } else if (req.uri == "/cmd/config") {
                res = handle_config_cmd(req, dev);
            } else {
                res = {404, "application/json", "{\"error\":\"Not Found\"}", {}};
            }
            send_response(client, res);
            close(client);
        }).detach();
    }
}

// ==== Main Entrypoint ====
int main() {
    std::string server_host = getenv_or_default("HTTP_SERVER_HOST", "0.0.0.0");
    int server_port = std::stoi(getenv_or_default("HTTP_SERVER_PORT", "8080"));
    // Device info can be extended with env if needed

    HikvisionDevice device;

    http_server_loop(server_host, server_port, device);
    return 0;
}