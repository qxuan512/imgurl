#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <map>
#include <fstream>
#include <streambuf>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// Mockup for Hikvision SDK API (replace with actual SDK calls as needed)
namespace hikvision_sdk {
    struct DecoderStatus {
        std::string device_status = "online";
        std::string channel_status = "normal";
        std::string display_config = "default";
        std::string alarm_status = "none";
        std::string playback_state = "stopped";
        // ... add more as needed
    };

    struct DecoderConfig {
        std::string display_mode = "1x1";
        std::string scene_mode = "normal";
        std::string loop_decode = "enabled";
        std::string window_mapping = "auto";
        // ... add more as needed
    };

    static DecoderStatus g_status;
    static DecoderConfig g_config;

    void init(const std::string& ip, int port, const std::string& user, const std::string& pwd) {
        // Mock init, no-op
    }

    DecoderStatus getStatus() {
        return g_status;
    }

    DecoderConfig getConfig() {
        return g_config;
    }

    void setConfig(const DecoderConfig& cfg) {
        g_config = cfg;
    }

    void control(const std::map<std::string, std::string>& ctrl) {
        // mock control
        for (const auto& kv : ctrl) {
            if (kv.first == "reboot" && kv.second == "1") {
                g_status.device_status = "rebooting";
            } else if (kv.first == "shutdown" && kv.second == "1") {
                g_status.device_status = "shutdown";
            } // add more as necessary
        }
    }
}

// Utility for getting env or default
std::string getenv_or(const char* key, const char* defval) {
    const char* v = std::getenv(key);
    return v ? v : defval;
}

// Simple JSON helpers
std::string quote(const std::string& s) {
    std::ostringstream oss;
    oss << '"';
    for (auto c : s) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else if (c == '\n') oss << "\\n";
        else oss << c;
    }
    oss << '"';
    return oss.str();
}
std::string json_kv(const std::string& k, const std::string& v) {
    return quote(k) + ":" + quote(v);
}
std::string json_obj(const std::vector<std::string>& kvs) {
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < kvs.size(); ++i) {
        if (i) oss << ",";
        oss << kvs[i];
    }
    oss << "}";
    return oss.str();
}

// HTTP utilities
struct HttpRequest {
    std::string method, path, http_version;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;

    std::string str() const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << " " << status_text << "\r\n";
        for (const auto& h : headers) {
            oss << h.first << ": " << h.second << "\r\n";
        }
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    }
};

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

HttpRequest parse_http_request(const std::string& raw) {
    std::istringstream ss(raw);
    std::string line;
    HttpRequest req;
    // First line
    if (!std::getline(ss, line)) return req;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream lss(line);
    lss >> req.method >> req.path >> req.http_version;
    // Headers
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos+1);
            while (!val.empty() && val[0] == ' ') val.erase(0, 1);
            req.headers[to_lower(key)] = val;
        }
    }
    // Body
    std::ostringstream bod;
    bod << ss.rdbuf();
    req.body = bod.str();
    return req;
}

// Parse decoder config JSON (simple flat, not nested, for demo)
hikvision_sdk::DecoderConfig parse_config_json(const std::string& body) {
    hikvision_sdk::DecoderConfig cfg;
    // Very simple parser: expects keys like "display_mode", etc.
    if (body.find("display_mode") != std::string::npos) {
        size_t p = body.find("display_mode");
        size_t q = body.find(':', p);
        size_t r = body.find('"', q+1);
        size_t s = body.find('"', r+1);
        if (r != std::string::npos && s != std::string::npos)
            cfg.display_mode = body.substr(r+1, s-r-1);
    }
    if (body.find("scene_mode") != std::string::npos) {
        size_t p = body.find("scene_mode");
        size_t q = body.find(':', p);
        size_t r = body.find('"', q+1);
        size_t s = body.find('"', r+1);
        if (r != std::string::npos && s != std::string::npos)
            cfg.scene_mode = body.substr(r+1, s-r-1);
    }
    if (body.find("loop_decode") != std::string::npos) {
        size_t p = body.find("loop_decode");
        size_t q = body.find(':', p);
        size_t r = body.find('"', q+1);
        size_t s = body.find('"', r+1);
        if (r != std::string::npos && s != std::string::npos)
            cfg.loop_decode = body.substr(r+1, s-r-1);
    }
    if (body.find("window_mapping") != std::string::npos) {
        size_t p = body.find("window_mapping");
        size_t q = body.find(':', p);
        size_t r = body.find('"', q+1);
        size_t s = body.find('"', r+1);
        if (r != std::string::npos && s != std::string::npos)
            cfg.window_mapping = body.substr(r+1, s-r-1);
    }
    // Add more fields as needed
    return cfg;
}

// Parse control JSON (flat, for demo)
std::map<std::string, std::string> parse_ctrl_json(const std::string& body) {
    std::map<std::string, std::string> ctrl;
    // Very simple parser: expects keys like "reboot": "1"
    size_t pos = 0;
    while (pos < body.size()) {
        size_t key_s = body.find('"', pos);
        if (key_s == std::string::npos) break;
        size_t key_e = body.find('"', key_s+1);
        if (key_e == std::string::npos) break;
        std::string key = body.substr(key_s+1, key_e-key_s-1);
        size_t val_s = body.find(':', key_e)+1;
        while (val_s < body.size() && (body[val_s]==' '||body[val_s]=='"')) ++val_s;
        size_t val_e = body.find_first_of(",}", val_s);
        std::string val = body.substr(val_s, val_e-val_s);
        while (!val.empty() && val.back()=='"') val.pop_back();
        ctrl[key] = val;
        pos = val_e+1;
    }
    return ctrl;
}

// HTTP Response helpers
HttpResponse resp_json(int status, const std::string& text, const std::string& json) {
    HttpResponse r;
    r.status = status;
    r.status_text = text;
    r.headers["Content-Type"] = "application/json";
    r.body = json;
    return r;
}
HttpResponse resp_plain(int status, const std::string& text, const std::string& msg) {
    HttpResponse r;
    r.status = status;
    r.status_text = text;
    r.headers["Content-Type"] = "text/plain";
    r.body = msg;
    return r;
}

// Handler: GET /decoder/status
HttpResponse handle_get_decoder_status() {
    auto st = hikvision_sdk::getStatus();
    std::vector<std::string> kvs{
        json_kv("device_status", st.device_status),
        json_kv("channel_status", st.channel_status),
        json_kv("display_config", st.display_config),
        json_kv("alarm_status", st.alarm_status),
        json_kv("playback_state", st.playback_state)
    };
    return resp_json(200, "OK", json_obj(kvs));
}

// Handler: GET /decoder/config
HttpResponse handle_get_decoder_config() {
    auto cfg = hikvision_sdk::getConfig();
    std::vector<std::string> kvs{
        json_kv("display_mode", cfg.display_mode),
        json_kv("scene_mode", cfg.scene_mode),
        json_kv("loop_decode", cfg.loop_decode),
        json_kv("window_mapping", cfg.window_mapping)
    };
    return resp_json(200, "OK", json_obj(kvs));
}

// Handler: PUT /decoder/config
HttpResponse handle_put_decoder_config(const std::string& body) {
    hikvision_sdk::DecoderConfig cfg = parse_config_json(body);
    hikvision_sdk::setConfig(cfg);
    return resp_json(200, "OK", "{\"result\":\"Config updated\"}");
}

// Handler: POST /decoder/ctrl
HttpResponse handle_post_decoder_ctrl(const std::string& body) {
    auto ctrl = parse_ctrl_json(body);
    hikvision_sdk::control(ctrl);
    return resp_json(200, "OK", "{\"result\":\"Command executed\"}");
}

// Main HTTP server logic
void handle_http_client(int client_sock) {
    char buffer[8192];
    ssize_t recvd = recv(client_sock, buffer, sizeof(buffer)-1, 0);
    if (recvd <= 0) { close(client_sock); return; }
    buffer[recvd] = 0;
    std::string raw_req(buffer);
    HttpRequest req = parse_http_request(raw_req);

    HttpResponse resp;

    if (req.method == "GET" && req.path == "/decoder/status") {
        resp = handle_get_decoder_status();
    }
    else if (req.method == "GET" && req.path == "/decoder/config") {
        resp = handle_get_decoder_config();
    }
    else if (req.method == "PUT" && req.path == "/decoder/config") {
        resp = handle_put_decoder_config(req.body);
    }
    else if (req.method == "POST" && req.path == "/decoder/ctrl") {
        resp = handle_post_decoder_ctrl(req.body);
    }
    else {
        resp = resp_plain(404, "Not Found", "Endpoint not found");
    }

    std::string out = resp.str();
    send(client_sock, out.c_str(), out.size(), 0);
    close(client_sock);
}

void http_server_main(const std::string& host, int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Socket failed\n";
        exit(1);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        exit(1);
    }

    if (listen(server_fd, 8) < 0) {
        std::cerr << "Listen failed\n";
        exit(1);
    }

    std::cout << "HTTP server listening on " << host << ":" << port << std::endl;
    while (1) {
        int client_sock = accept(server_fd, nullptr, nullptr);
        if (client_sock < 0) continue;
        std::thread(handle_http_client, client_sock).detach();
    }
}

int main() {
    // Env configuration
    std::string device_ip   = getenv_or("DEVICE_IP", "192.168.1.64");
    int device_port         = std::stoi(getenv_or("DEVICE_PORT", "8000"));
    std::string device_user = getenv_or("DEVICE_USER", "admin");
    std::string device_pwd  = getenv_or("DEVICE_PASSWORD", "12345");
    std::string server_host = getenv_or("HTTP_SERVER_HOST", "0.0.0.0");
    int server_port         = std::stoi(getenv_or("HTTP_SERVER_PORT", "8080"));

    hikvision_sdk::init(device_ip, device_port, device_user, device_pwd);

    http_server_main(server_host, server_port);

    return 0;
}