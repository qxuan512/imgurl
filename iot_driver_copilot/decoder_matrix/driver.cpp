#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <cstring>
#include <sstream>
#include <fstream>
#include <streambuf>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <json/json.h>

// ------------ Device SDK Simulation (Replace with actual SDK calls) -----------
namespace DeviceSDK {
    // Simulate device state/config
    std::mutex dev_mutex;
    Json::Value device_status;
    Json::Value device_config;

    void Init() {
        std::lock_guard<std::mutex> lk(dev_mutex);
        device_status["decoder_state"] = "running";
        device_status["channel_enable"] = true;
        device_status["playback_state"] = "stopped";
        device_status["scene_mode"] = "normal";
        device_status["error_code"] = 0;

        device_config["display"] = "4K";
        device_config["wall"] = "video_wall_1";
        device_config["window"] = "window_0";
        device_config["scene"] = "default";
        device_config["network"] = "192.168.1.100";
        device_config["time"] = "2024-06-01T12:00:00Z";
    }

    Json::Value GetStatus() {
        std::lock_guard<std::mutex> lk(dev_mutex);
        return device_status;
    }

    Json::Value GetConfig() {
        std::lock_guard<std::mutex> lk(dev_mutex);
        return device_config;
    }

    bool SetConfig(const Json::Value& new_cfg, std::string& errmsg) {
        std::lock_guard<std::mutex> lk(dev_mutex);
        // Do basic validation
        if (!new_cfg.isObject()) {
            errmsg = "Invalid config payload";
            return false;
        }
        device_config = new_cfg;
        return true;
    }

    bool ExecuteCommand(const Json::Value& cmd, Json::Value& result, std::string& errmsg) {
        std::lock_guard<std::mutex> lk(dev_mutex);
        if (!cmd.isMember("command")) {
            errmsg = "Missing 'command' field";
            return false;
        }
        std::string c = cmd["command"].asString();
        if(c == "reboot") {
            device_status["decoder_state"] = "rebooting";
            result["result"] = "Device rebooting";
            return true;
        }
        if(c == "start_playback") {
            device_status["playback_state"] = "playing";
            result["result"] = "Playback started";
            return true;
        }
        if(c == "stop_playback") {
            device_status["playback_state"] = "stopped";
            result["result"] = "Playback stopped";
            return true;
        }
        // ... (expand as needed)
        errmsg = "Unknown command";
        return false;
    }
}
// -----------------------------------------------------------------------------

// --------------- Minimal HTTP Server Implementation ---------------------------
#define HTTP_BUFFER_SIZE 8192

struct HttpRequest {
    std::string method;
    std::string path;
    std::string http_version;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code;
    std::string status_message;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string http_status_message(int code) {
    switch(code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

void send_response(int client_fd, const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_message << "\r\n";
    for (const auto& header : resp.headers) {
        oss << header.first << ": " << header.second << "\r\n";
    }
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "\r\n";
    oss << resp.body;
    std::string msg = oss.str();
    send(client_fd, msg.c_str(), msg.size(), 0);
}

bool parse_http_request(const std::string& req, HttpRequest& out) {
    std::istringstream iss(req);
    std::string line;
    if (!std::getline(iss, line)) return false;
    std::istringstream liness(line);
    liness >> out.method >> out.path >> out.http_version;

    // Headers
    while (std::getline(iss, line) && line != "\r") {
        if (line == "\r" || line.empty()) break;
        size_t pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos+1);
        // Remove whitespace
        while(!val.empty() && (val[0]==' '||val[0]=='\t')) val.erase(0,1);
        while(!val.empty() && (val.back()=='\r'||val.back()=='\n')) val.pop_back();
        out.headers[key] = val;
    }
    // Body
    std::string body;
    while (std::getline(iss, line)) {
        body += line + "\n";
    }
    if (!body.empty()) {
        // Remove extra CRLF
        size_t pos = body.find("\r\n\r\n");
        if(pos != std::string::npos) body = body.substr(pos+4);
        out.body = body;
    }
    return true;
}

// --------------- JSON Helper Functions ---------------------------------------
std::string json_stringify(const Json::Value& val) {
    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = ""; // compact
    return Json::writeString(wbuilder, val);
}

bool json_parse(const std::string& s, Json::Value& out) {
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    std::istringstream iss(s);
    return Json::parseFromStream(rbuilder, iss, &out, &errs);
}

// --------------- Endpoint Handlers -------------------------------------------
void handle_status(int client_fd, const HttpRequest& req) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_message = http_status_message(resp.status_code);
    resp.headers["Content-Type"] = "application/json";
    Json::Value status = DeviceSDK::GetStatus();
    resp.body = json_stringify(status);
    send_response(client_fd, resp);
}

void handle_get_config(int client_fd, const HttpRequest& req) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_message = http_status_message(resp.status_code);
    resp.headers["Content-Type"] = "application/json";
    Json::Value config = DeviceSDK::GetConfig();
    resp.body = json_stringify(config);
    send_response(client_fd, resp);
}

void handle_put_config(int client_fd, const HttpRequest& req) {
    HttpResponse resp;
    Json::Value new_cfg;
    if (!json_parse(req.body, new_cfg)) {
        resp.status_code = 400;
        resp.status_message = http_status_message(resp.status_code);
        resp.headers["Content-Type"] = "application/json";
        Json::Value err;
        err["error"] = "Invalid JSON";
        resp.body = json_stringify(err);
        send_response(client_fd, resp);
        return;
    }
    std::string errmsg;
    if (!DeviceSDK::SetConfig(new_cfg, errmsg)) {
        resp.status_code = 400;
        resp.status_message = http_status_message(resp.status_code);
        resp.headers["Content-Type"] = "application/json";
        Json::Value err;
        err["error"] = errmsg;
        resp.body = json_stringify(err);
        send_response(client_fd, resp);
        return;
    }
    resp.status_code = 200;
    resp.status_message = http_status_message(resp.status_code);
    resp.headers["Content-Type"] = "application/json";
    Json::Value ok;
    ok["result"] = "Config updated";
    resp.body = json_stringify(ok);
    send_response(client_fd, resp);
}

void handle_post_commands(int client_fd, const HttpRequest& req) {
    HttpResponse resp;
    Json::Value cmd;
    if (!json_parse(req.body, cmd)) {
        resp.status_code = 400;
        resp.status_message = http_status_message(resp.status_code);
        resp.headers["Content-Type"] = "application/json";
        Json::Value err;
        err["error"] = "Invalid JSON";
        resp.body = json_stringify(err);
        send_response(client_fd, resp);
        return;
    }
    Json::Value result;
    std::string errmsg;
    if (!DeviceSDK::ExecuteCommand(cmd, result, errmsg)) {
        resp.status_code = 400;
        resp.status_message = http_status_message(resp.status_code);
        resp.headers["Content-Type"] = "application/json";
        Json::Value err;
        err["error"] = errmsg;
        resp.body = json_stringify(err);
        send_response(client_fd, resp);
        return;
    }
    resp.status_code = 200;
    resp.status_message = http_status_message(resp.status_code);
    resp.headers["Content-Type"] = "application/json";
    resp.body = json_stringify(result);
    send_response(client_fd, resp);
}

void handle_not_found(int client_fd) {
    HttpResponse resp;
    resp.status_code = 404;
    resp.status_message = http_status_message(resp.status_code);
    resp.headers["Content-Type"] = "application/json";
    Json::Value err;
    err["error"] = "Not Found";
    resp.body = json_stringify(err);
    send_response(client_fd, resp);
}

void handle_method_not_allowed(int client_fd) {
    HttpResponse resp;
    resp.status_code = 405;
    resp.status_message = http_status_message(resp.status_code);
    resp.headers["Content-Type"] = "application/json";
    Json::Value err;
    err["error"] = "Method Not Allowed";
    resp.body = json_stringify(err);
    send_response(client_fd, resp);
}

// --------------- Routing -----------------------------------------------------
void handle_http_request(int client_fd, const HttpRequest& req) {
    if (req.method == "GET" && req.path == "/status") {
        handle_status(client_fd, req);
    } else if (req.method == "GET" && req.path == "/config") {
        handle_get_config(client_fd, req);
    } else if (req.method == "PUT" && req.path == "/config") {
        handle_put_config(client_fd, req);
    } else if (req.method == "POST" && req.path == "/commands") {
        handle_post_commands(client_fd, req);
    } else if (req.path == "/config" || req.path == "/status" || req.path == "/commands") {
        handle_method_not_allowed(client_fd);
    } else {
        handle_not_found(client_fd);
    }
}

// --------------- HTTP Server Main Loop ---------------------------------------
void client_thread(int client_fd) {
    char buffer[HTTP_BUFFER_SIZE+1];
    int received = recv(client_fd, buffer, HTTP_BUFFER_SIZE, 0);
    if (received <= 0) {
        close(client_fd);
        return;
    }
    buffer[received] = 0;
    std::string reqstr(buffer);
    HttpRequest req;
    if (!parse_http_request(reqstr, req)) {
        HttpResponse resp;
        resp.status_code = 400;
        resp.status_message = http_status_message(resp.status_code);
        resp.headers["Content-Type"] = "application/json";
        Json::Value err;
        err["error"] = "Bad Request";
        resp.body = json_stringify(err);
        send_response(client_fd, resp);
        close(client_fd);
        return;
    }
    handle_http_request(client_fd, req);
    close(client_fd);
}

void run_http_server(const std::string& host, int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "Failed to create socket\n";
        exit(1);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        close(server_fd);
        exit(1);
    }
    if (listen(server_fd, 8) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        exit(1);
    }
    std::cout << "HTTP server listening on " << host << ":" << port << std::endl;
    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;
        std::thread(client_thread, client_fd).detach();
    }
}

// --------------- Main --------------------------------------------------------
std::string get_env(const char* name, const std::string& defval = "") {
    const char* v = std::getenv(name);
    if (v) return std::string(v);
    return defval;
}

int main() {
    // Device SDK init (simulate device state)
    DeviceSDK::Init();

    // Read configuration from environment
    std::string http_host = get_env("HTTP_SERVER_HOST", "0.0.0.0");
    int http_port = std::stoi(get_env("HTTP_SERVER_PORT", "8080"));
    // Device connection info (for true SDK use)
    std::string device_ip = get_env("DEVICE_IP", "192.168.1.64");
    std::string device_user = get_env("DEVICE_USER", "admin");
    std::string device_pass = get_env("DEVICE_PASS", "12345");
    // ... more as needed

    run_http_server(http_host, http_port);
    return 0;
}