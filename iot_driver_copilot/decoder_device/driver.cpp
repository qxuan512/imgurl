#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <streambuf>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <cctype>
#include <json/json.h>

// --- Minimal HTTP server dependencies ---
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// --- Device SDK Placeholder (HCNetSDK) ---
class DeviceSDK {
public:
    DeviceSDK(const std::string& ip, int port, const std::string& user, const std::string& pass)
        : device_ip(ip), device_port(port), username(user), password(pass) {}

    // Dummy implementations -- replace with real SDK calls
    Json::Value getStatus(const std::string& filter) {
        Json::Value status;
        if (filter.empty() || filter == "decoder") {
            status["decoder_channel_state"] = "OK";
        }
        if (filter.empty() || filter == "alarm") {
            status["alarm_status"] = "No alarms";
        }
        if (filter.empty() || filter == "playback") {
            status["playback_progress"] = 37;
        }
        if (filter.empty() || filter == "network") {
            status["network_config"] = "eth0:192.168.1.20";
        }
        status["device_model"] = "DS-6300D";
        status["manufacturer"] = "Hikvision";
        return status;
    }

    bool updateDisplay(const Json::Value& config, std::string& err) {
        // Validate and "apply" config
        if (!config.isObject()) {
            err = "Invalid configuration object";
            return false;
        }
        // Simulate update
        return true;
    }

    bool playbackControl(const Json::Value& command, std::string& err) {
        // Dummy validate
        if (!command.isMember("action")) {
            err = "Missing 'action' in payload";
            return false;
        }
        // Simulate playback control
        return true;
    }

    bool reboot(const Json::Value& params, std::string& err) {
        // Simulate reboot
        return true;
    }

private:
    std::string device_ip;
    int device_port;
    std::string username;
    std::string password;
};

// --- Utility ---
std::string getenv_def(const char* key, const char* def) {
    const char* val = std::getenv(key);
    return val ? val : def;
}

int getenv_int(const char* key, int def) {
    const char* val = std::getenv(key);
    if (!val) return def;
    return std::atoi(val);
}

void send_response(int client_fd, int status, const std::string& status_text, const std::string& content_type, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << status_text << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    std::string resp = oss.str();
    send(client_fd, resp.data(), resp.size(), 0);
}

void send_json(int client_fd, int status, const std::string& status_text, const Json::Value& obj) {
    Json::StreamWriterBuilder wbuilder;
    std::string output = Json::writeString(wbuilder, obj);
    send_response(client_fd, status, status_text, "application/json", output);
}

void send_404(int client_fd) {
    Json::Value j;
    j["error"] = "Not found";
    send_json(client_fd, 404, "Not Found", j);
}

void send_400(int client_fd, const std::string& err) {
    Json::Value j;
    j["error"] = err;
    send_json(client_fd, 400, "Bad Request", j);
}

void send_405(int client_fd) {
    Json::Value j;
    j["error"] = "Method not allowed";
    send_json(client_fd, 405, "Method Not Allowed", j);
}

// --- HTTP Parsing ---
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string url_decode(const std::string& s) {
    std::string out;
    char a, b;
    for (size_t i = 0; i < s.length(); ++i) {
        if ((s[i] == '%') && ((a = s[i+1]) && (b = s[i+2])) &&
            (isxdigit(a) && isxdigit(b))) {
            a = (a >= 'a') ? (a - 'a' + 10) : (a >= 'A') ? (a - 'A' + 10) : (a - '0');
            b = (b >= 'a') ? (b - 'a' + 10) : (b >= 'A') ? (b - 'A' + 10) : (b - '0');
            out += static_cast<char>(16 * a + b);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

void parse_query(const std::string& qs, std::map<std::string, std::string>& qmap) {
    size_t start = 0;
    while (start < qs.size()) {
        size_t eq = qs.find('=', start);
        size_t amp = qs.find('&', start);
        if (eq == std::string::npos) break;
        std::string key = url_decode(qs.substr(start, eq - start));
        std::string val = url_decode(qs.substr(eq + 1, 
                                  (amp == std::string::npos ? qs.size() : amp) - eq - 1));
        qmap[key] = val;
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
}

bool parse_http_request(const std::string& req, HttpRequest& out) {
    std::istringstream iss(req);
    std::string line;
    if (!std::getline(iss, line)) return false;
    std::istringstream first(line);
    if (!(first >> out.method)) return false;
    std::string pathq;
    if (!(first >> pathq)) return false;
    size_t qmark = pathq.find('?');
    if (qmark == std::string::npos) {
        out.path = pathq;
    } else {
        out.path = pathq.substr(0, qmark);
        parse_query(pathq.substr(qmark+1), out.query);
    }
    // Headers
    while (std::getline(iss, line) && line != "\r") {
        size_t col = line.find(':');
        if (col != std::string::npos) {
            std::string key = line.substr(0, col);
            std::string val = line.substr(col+1);
            key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of("\r\n")+1);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            out.headers[key] = val;
        }
    }
    // Body
    if (out.headers.count("content-length")) {
        int cl = std::stoi(out.headers["content-length"]);
        std::string bl;
        while (std::getline(iss, line)) {
            bl += line + "\n";
        }
        if (bl.size() > cl) bl.resize(cl);
        out.body = bl;
    }
    return true;
}

// --- Routing ---
void handle_display_put(int client_fd, DeviceSDK& sdk, const HttpRequest& req) {
    Json::CharReaderBuilder rbuilder;
    Json::Value payload;
    std::string errs;
    std::istringstream s(req.body);
    if (!Json::parseFromStream(rbuilder, s, &payload, &errs)) {
        send_400(client_fd, "Invalid JSON payload");
        return;
    }
    std::string err;
    if (!sdk.updateDisplay(payload, err)) {
        send_400(client_fd, err);
        return;
    }
    Json::Value resp;
    resp["success"] = true;
    send_json(client_fd, 200, "OK", resp);
}

void handle_status_get(int client_fd, DeviceSDK& sdk, const HttpRequest& req) {
    std::string filter;
    if (req.query.count("filter")) filter = req.query.at("filter");
    Json::Value status = sdk.getStatus(filter);
    send_json(client_fd, 200, "OK", status);
}

void handle_playback_post(int client_fd, DeviceSDK& sdk, const HttpRequest& req) {
    Json::CharReaderBuilder rbuilder;
    Json::Value payload;
    std::string errs;
    std::istringstream s(req.body);
    if (!Json::parseFromStream(rbuilder, s, &payload, &errs)) {
        send_400(client_fd, "Invalid JSON payload");
        return;
    }
    std::string err;
    if (!sdk.playbackControl(payload, err)) {
        send_400(client_fd, err);
        return;
    }
    Json::Value resp;
    resp["success"] = true;
    send_json(client_fd, 200, "OK", resp);
}

void handle_reboot_post(int client_fd, DeviceSDK& sdk, const HttpRequest& req) {
    Json::CharReaderBuilder rbuilder;
    Json::Value payload;
    std::string errs;
    if (!req.body.empty()) {
        std::istringstream s(req.body);
        if (!Json::parseFromStream(rbuilder, s, &payload, &errs)) {
            send_400(client_fd, "Invalid JSON payload");
            return;
        }
    }
    std::string err;
    if (!sdk.reboot(payload, err)) {
        send_400(client_fd, err);
        return;
    }
    Json::Value resp;
    resp["success"] = true;
    send_json(client_fd, 200, "OK", resp);
}

// --- Main HTTP Server Loop ---
void http_worker(int client_fd, DeviceSDK& sdk) {
    char buf[8192];
    int recvd = recv(client_fd, buf, sizeof(buf)-1, 0);
    if (recvd <= 0) {
        close(client_fd);
        return;
    }
    buf[recvd] = 0;
    std::string reqstr(buf);
    HttpRequest req;
    if (!parse_http_request(reqstr, req)) {
        send_400(client_fd, "Malformed HTTP request");
        close(client_fd);
        return;
    }
    // Routing
    if (req.method == "PUT" && req.path == "/display") {
        handle_display_put(client_fd, sdk, req);
    } else if (req.method == "GET" && req.path == "/status") {
        handle_status_get(client_fd, sdk, req);
    } else if (req.method == "POST" && req.path == "/commands/playback") {
        handle_playback_post(client_fd, sdk, req);
    } else if (req.method == "POST" && req.path == "/commands/reboot") {
        handle_reboot_post(client_fd, sdk, req);
    } else if ((req.method == "OPTIONS") && 
                (req.path == "/display" || req.path == "/status" || req.path == "/commands/playback" || req.path == "/commands/reboot")) {
        // CORS Preflight
        std::string allow;
        if (req.path == "/display") allow = "PUT,OPTIONS";
        else if (req.path == "/status") allow = "GET,OPTIONS";
        else if (req.path == "/commands/playback") allow = "POST,OPTIONS";
        else if (req.path == "/commands/reboot") allow = "POST,OPTIONS";
        std::ostringstream oss;
        oss << "HTTP/1.1 204 No Content\r\n";
        oss << "Access-Control-Allow-Origin: *\r\n";
        oss << "Access-Control-Allow-Methods: " << allow << "\r\n";
        oss << "Access-Control-Allow-Headers: Content-Type\r\n";
        oss << "Content-Length: 0\r\n";
        oss << "Connection: close\r\n\r\n";
        send(client_fd, oss.str().data(), oss.str().size(), 0);
    } else {
        send_404(client_fd);
    }
    close(client_fd);
}

// --- Main ---
volatile bool running = true;

void sigint_handler(int) {
    running = false;
}

int main() {
    // --- Read config from environment ---
    std::string device_ip = getenv_def("DEVICE_IP", "192.168.1.64");
    int device_port = getenv_int("DEVICE_PORT", 8000); // SDK port, not HTTP
    std::string device_user = getenv_def("DEVICE_USER", "admin");
    std::string device_pass = getenv_def("DEVICE_PASS", "12345");
    std::string http_host = getenv_def("HTTP_HOST", "0.0.0.0");
    int http_port = getenv_int("HTTP_PORT", 8080);

    // --- Device SDK connection (simulate login) ---
    DeviceSDK sdk(device_ip, device_port, device_user, device_pass);

    // --- HTTP Server ---
    struct sockaddr_in addr;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(http_port);
    addr.sin_addr.s_addr = (http_host == "0.0.0.0") ? INADDR_ANY : inet_addr(http_host.c_str());
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }
    std::cout << "HTTP server listening on " << http_host << ":" << http_port << std::endl;

    std::signal(SIGINT, sigint_handler);

    while (running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (client_fd < 0) {
            if (!running) break;
            continue;
        }
        std::thread t(http_worker, client_fd, std::ref(sdk));
        t.detach();
    }
    close(server_fd);
    std::cout << "Shutting down.\n";
    return 0;
}