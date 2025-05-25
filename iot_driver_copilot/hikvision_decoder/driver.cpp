#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

// --- Configuration ---
struct Config {
    std::string device_ip;
    std::string device_port;
    std::string server_host;
    int server_port;

    static Config load_from_env() {
        Config cfg;
        const char* env_device_ip = std::getenv("DEVICE_IP");
        const char* env_device_port = std::getenv("DEVICE_PORT");
        const char* env_host = std::getenv("SERVER_HOST");
        const char* env_port = std::getenv("SERVER_PORT");
        cfg.device_ip = env_device_ip ? env_device_ip : "127.0.0.1";
        cfg.device_port = env_device_port ? env_device_port : "8000";
        cfg.server_host = env_host ? env_host : "0.0.0.0";
        cfg.server_port = env_port ? std::atoi(env_port) : 8080;
        return cfg;
    }
};

// --- Simple HTTP Parser/Response ---
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code;
    std::string status;
    std::map<std::string, std::string> headers;
    std::string body;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status_code << " " << status << "\r\n";
        for(const auto& kv : headers) {
            oss << kv.first << ": " << kv.second << "\r\n";
        }
        oss << "Content-Length: " << body.length() << "\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    }
};

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \r\n\t");
    size_t e = s.find_last_not_of(" \r\n\t");
    if(b == std::string::npos || e == std::string::npos) return "";
    return s.substr(b, e-b+1);
}

bool parse_http_request(const std::string& data, HttpRequest& req) {
    std::istringstream iss(data);
    std::string line;
    if(!std::getline(iss, line)) return false;
    std::istringstream lss(line);
    if(!(lss >> req.method >> req.path)) return false;

    // Headers
    while(std::getline(iss, line) && trim(line) != "") {
        size_t d = line.find(':');
        if(d != std::string::npos) {
            std::string key = trim(line.substr(0, d));
            std::string val = trim(line.substr(d+1));
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            req.headers[key] = val;
        }
    }

    // Body
    std::string body((std::istreambuf_iterator<char>(iss)), std::istreambuf_iterator<char>());
    req.body = body;
    return true;
}

// --- Device SDK Simulated Interface ---
class HikvisionDecoder {
    std::mutex mtx;
    // Simulated device state
    std::map<std::string, std::string> device_config;
    std::string device_status_json;
public:
    HikvisionDecoder() {
        // Initialize with some dummy configuration/state
        device_config = {
            {"display_mode", "quad"},
            {"scene", "default"},
            {"loop_decode", "off"}
        };
        update_status();
    }
    void update_status() {
        // Simulate status as JSON
        std::ostringstream oss;
        oss << "{"
            << "\"channel_status\": \"ok\","
            << "\"display_config\": \"" << device_config["display_mode"] << "\","
            << "\"alarm_status\": \"normal\","
            << "\"scene\": \"" << device_config["scene"] << "\","
            << "\"loop_decode\": \"" << device_config["loop_decode"] << "\","
            << "\"device_health\": \"good\""
            << "}";
        device_status_json = oss.str();
    }

    std::string get_status() {
        std::lock_guard<std::mutex> lock(mtx);
        update_status();
        return device_status_json;
    }

    std::string get_config() {
        std::lock_guard<std::mutex> lock(mtx);
        std::ostringstream oss;
        oss << "{";
        int cnt = 0;
        for(const auto& kv : device_config) {
            if(cnt++) oss << ",";
            oss << "\"" << kv.first << "\":\"" << kv.second << "\"";
        }
        oss << "}";
        return oss.str();
    }

    bool set_config(const std::string& json, std::string& out_error) {
        std::lock_guard<std::mutex> lock(mtx);
        // Very primitive JSON parse: expects {"key":"val", ...}
        size_t p = 0;
        while((p = json.find('"', p)) != std::string::npos) {
            size_t p2 = json.find('"', p+1); if(p2==std::string::npos) break;
            std::string key = json.substr(p+1, p2-p-1);
            size_t c = json.find(':', p2); if(c==std::string::npos) break;
            size_t v1 = json.find('"', c); if(v1==std::string::npos) break;
            size_t v2 = json.find('"', v1+1); if(v2==std::string::npos) break;
            std::string value = json.substr(v1+1, v2-v1-1);
            device_config[key] = value;
            p = v2+1;
        }
        update_status();
        return true;
    }

    bool send_ctrl_command(const std::string& json, std::string& result) {
        std::lock_guard<std::mutex> lock(mtx);
        // Simulate some commands
        if(json.find("reboot") != std::string::npos) {
            result = "{\"result\": \"device rebooting\"}";
            return true;
        }
        if(json.find("start_decoding") != std::string::npos) {
            result = "{\"result\": \"dynamic decoding started\"}";
            device_config["decoding"] = "on";
            update_status();
            return true;
        }
        if(json.find("stop_decoding") != std::string::npos) {
            result = "{\"result\": \"decoding stopped\"}";
            device_config["decoding"] = "off";
            update_status();
            return true;
        }
        result = "{\"result\": \"command received\"}";
        return true;
    }
};

// --- Handler Functions ---
HttpResponse handle_get_decoder_status(HikvisionDecoder& dev) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status = "OK";
    resp.headers["Content-Type"] = "application/json";
    resp.body = dev.get_status();
    return resp;
}

HttpResponse handle_get_decoder_config(HikvisionDecoder& dev) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status = "OK";
    resp.headers["Content-Type"] = "application/json";
    resp.body = dev.get_config();
    return resp;
}

HttpResponse handle_put_decoder_config(HikvisionDecoder& dev, const HttpRequest& req) {
    HttpResponse resp;
    std::string err;
    if(req.headers.count("content-type")==0 ||
       req.headers.at("content-type").find("application/json") == std::string::npos) {
        resp.status_code = 400;
        resp.status = "Bad Request";
        resp.body = R"({"error": "Content-Type must be application/json"})";
        return resp;
    }
    if(dev.set_config(req.body, err)) {
        resp.status_code = 200;
        resp.status = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = R"({"result": "config updated"})";
    } else {
        resp.status_code = 400;
        resp.status = "Bad Request";
        resp.body = "{\"error\": \"" + err + "\"}";
    }
    return resp;
}

HttpResponse handle_post_decoder_ctrl(HikvisionDecoder& dev, const HttpRequest& req) {
    HttpResponse resp;
    std::string result;
    if(req.headers.count("content-type")==0 ||
       req.headers.at("content-type").find("application/json") == std::string::npos) {
        resp.status_code = 400;
        resp.status = "Bad Request";
        resp.body = R"({"error": "Content-Type must be application/json"})";
        return resp;
    }
    if(dev.send_ctrl_command(req.body, result)) {
        resp.status_code = 200;
        resp.status = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = result;
    } else {
        resp.status_code = 400;
        resp.status = "Bad Request";
        resp.body = R"({"error": "Invalid command"})";
    }
    return resp;
}

// --- Routing Dispatcher ---
HttpResponse dispatch_request(const HttpRequest& req, HikvisionDecoder& dev) {
    // GET /decoder/status
    if(req.method == "GET" && req.path == "/decoder/status") {
        return handle_get_decoder_status(dev);
    }
    // GET /decoder/config
    if(req.method == "GET" && req.path == "/decoder/config") {
        return handle_get_decoder_config(dev);
    }
    // PUT /decoder/config
    if(req.method == "PUT" && req.path == "/decoder/config") {
        return handle_put_decoder_config(dev, req);
    }
    // POST /decoder/ctrl
    if(req.method == "POST" && req.path == "/decoder/ctrl") {
        return handle_post_decoder_ctrl(dev, req);
    }
    // Not found
    HttpResponse resp;
    resp.status_code = 404;
    resp.status = "Not Found";
    resp.body = R"({"error": "Not found"})";
    return resp;
}

// --- HTTP Server ---
void client_thread(int client_sock, HikvisionDecoder& dev) {
    // Simple HTTP: read, parse, respond
    constexpr size_t BUFSZ = 16*1024;
    char buf[BUFSZ];
    ssize_t n = recv(client_sock, buf, BUFSZ-1, 0);
    if(n <= 0) { close(client_sock); return; }
    buf[n] = 0;
    HttpRequest req;
    if(!parse_http_request(buf, req)) {
        HttpResponse resp;
        resp.status_code = 400;
        resp.status = "Bad Request";
        resp.body = R"({"error": "Cannot parse request"})";
        std::string out = resp.to_string();
        send(client_sock, out.c_str(), out.size(), 0);
        close(client_sock); return;
    }
    HttpResponse resp = dispatch_request(req, dev);
    std::string out = resp.to_string();
    send(client_sock, out.c_str(), out.size(), 0);
    close(client_sock);
}

void http_server_main(const Config& cfg, HikvisionDecoder& dev) {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd < 0) { perror("socket"); exit(1); }
    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.server_port);
    addr.sin_addr.s_addr = inet_addr(cfg.server_host.c_str());
    if(bind(listenfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if(listenfd < 0) { perror("listen"); exit(1); }
    if(listen(listenfd, 16) < 0) { perror("listen"); exit(1); }
    std::cout << "HTTP server listening on " << cfg.server_host << ":" << cfg.server_port << std::endl;
    while(true) {
        sockaddr_in cli_addr;
        socklen_t clilen = sizeof(cli_addr);
        int newsock = accept(listenfd, (sockaddr*)&cli_addr, &clilen);
        if(newsock < 0) continue;
        std::thread(client_thread, newsock, std::ref(dev)).detach();
    }
}

// --- Main ---
int main() {
    Config cfg = Config::load_from_env();
    HikvisionDecoder decoder;
    http_server_main(cfg, decoder);
    return 0;
}