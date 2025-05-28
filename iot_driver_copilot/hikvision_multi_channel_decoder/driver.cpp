```cpp
#include <iostream>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <fstream>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <json/json.h>
#include <netinet/in.h>
#include <unistd.h>

// HCNetSDK device stub interface (to be replaced with real SDK)
namespace HCNetSDK {

struct DeviceSession {
    bool logged_in;
    std::string session_token;
    std::string username;
    std::string password;
    std::string device_ip;
    int device_port;
    time_t last_active;
    // ...other state
};

struct DeviceStatus {
    std::string state;
    std::string alarm;
    std::string network;
    std::string channels;
    // ...other fields
};

struct DeviceConfig {
    std::string display;
    std::string network;
    std::string decoder;
    // ...other fields
};

static std::mutex sdk_mtx;
static DeviceSession g_session;
static DeviceConfig g_config = {"default_display", "default_network", "default_decoder"};
static DeviceStatus g_status = {"running", "no_alarms", "default_network", "all_ok"};

bool Login(const std::string& ip, int port, const std::string& user, const std::string& pass, std::string& token) {
    std::lock_guard<std::mutex> l(sdk_mtx);
    if (user == "admin" && pass == "12345") {
        g_session.logged_in = true;
        g_session.username = user;
        g_session.password = pass;
        g_session.device_ip = ip;
        g_session.device_port = port;
        g_session.session_token = "sess_" + std::to_string(std::time(nullptr));
        g_session.last_active = std::time(nullptr);
        token = g_session.session_token;
        return true;
    }
    return false;
}

bool Logout(const std::string& token) {
    std::lock_guard<std::mutex> l(sdk_mtx);
    if (g_session.logged_in && g_session.session_token == token) {
        g_session.logged_in = false;
        g_session.session_token = "";
        return true;
    }
    return false;
}

bool IsLoggedIn(const std::string& token) {
    std::lock_guard<std::mutex> l(sdk_mtx);
    return g_session.logged_in && g_session.session_token == token;
}

bool GetStatus(DeviceStatus& status) {
    std::lock_guard<std::mutex> l(sdk_mtx);
    status = g_status;
    return true;
}

bool GetConfig(DeviceConfig& config) {
    std::lock_guard<std::mutex> l(sdk_mtx);
    config = g_config;
    return true;
}

bool SetConfig(const DeviceConfig& config) {
    std::lock_guard<std::mutex> l(sdk_mtx);
    g_config = config;
    return true;
}

bool ExecuteCommand(const std::string& cmd, const Json::Value& params, Json::Value& result) {
    std::lock_guard<std::mutex> l(sdk_mtx);
    if (cmd == "reboot") {
        result["result"] = "rebooting";
        g_status.state = "rebooting";
        return true;
    } else if (cmd == "shutdown") {
        result["result"] = "shutting down";
        g_status.state = "shutdown";
        return true;
    } else if (cmd == "start_decode") {
        result["result"] = "decoding started";
        g_status.state = "decoding";
        return true;
    } else if (cmd == "stop_decode") {
        result["result"] = "decoding stopped";
        g_status.state = "idle";
        return true;
    }
    result["result"] = "unknown command";
    return false;
}
}

// Utility functions
std::string get_env(const char* key, const char* fallback) {
    const char* val = std::getenv(key);
    return val ? val : fallback;
}

// Minimal HTTP server
class HttpRequest {
public:
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string query;

    std::string get_header(const std::string& key) const {
        auto it = headers.find(key);
        if (it != headers.end()) return it->second;
        return "";
    }
};

class HttpResponse {
public:
    int status;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;

    HttpResponse(int s=200, const std::string& st="OK"): status(s), status_text(st) {}

    std::string to_string() const {
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

void url_decode(std::string &s) {
    size_t pos = 0;
    while ((pos = s.find('%', pos)) != std::string::npos) {
        if (pos + 2 < s.length()) {
            std::string hex = s.substr(pos + 1, 2);
            char ch = static_cast<char>(strtol(hex.c_str(), nullptr, 16));
            s.replace(pos, 3, 1, ch);
        } else {
            break;
        }
    }
    std::replace(s.begin(), s.end(), '+', ' ');
}

bool parse_http_request(const std::string& raw, HttpRequest& req) {
    std::istringstream ss(raw);
    std::string line;
    if (!std::getline(ss, line)) return false;
    std::istringstream lss(line);
    if (!(lss >> req.method >> req.path >> req.version)) return false;
    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        req.query = req.path.substr(qpos+1);
        req.path = req.path.substr(0, qpos);
    }
    std::string header;
    while (std::getline(ss, line) && line != "\r") {
        if (line.empty() || line == "\n" || line == "\r\n") break;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon+1);
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0,1);
            if (!value.empty() && value.back() == '\r') value.pop_back();
            req.headers[key] = value;
        }
    }
    // body
    if (req.headers.count("Content-Length")) {
        int len = std::stoi(req.headers["Content-Length"]);
        std::string body;
        if (len > 0) {
            body.resize(len);
            ss.read(&body[0], len);
            req.body = body;
        }
    }
    return true;
}

// Session management
std::mutex g_token_mtx;
std::map<std::string, std::string> g_token_user;

// Authentication wrapper
bool require_auth(const HttpRequest& req, std::string& token) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return false;
    std::string val = it->second;
    std::string prefix = "Bearer ";
    if (val.compare(0, prefix.size(), prefix) == 0) {
        token = val.substr(prefix.size());
        return HCNetSDK::IsLoggedIn(token);
    }
    return false;
}

// Endpoint handlers
HttpResponse handle_login(const HttpRequest& req, const std::string& device_ip, int device_port) {
    if (req.method != "POST") return HttpResponse(405, "Method Not Allowed");
    Json::Value root;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    std::istringstream iss(req.body);
    if (!Json::parseFromStream(rbuilder, iss, &root, &errs)) {
        HttpResponse resp(400, "Bad Request");
        resp.body = "{\"error\": \"Invalid JSON\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
    std::string username = root.get("username", "").asString();
    std::string password = root.get("password", "").asString();
    std::string token;
    if (HCNetSDK::Login(device_ip, device_port, username, password, token)) {
        HttpResponse resp(200, "OK");
        Json::Value jres;
        jres["token"] = token;
        Json::StreamWriterBuilder wbuilder;
        resp.body = Json::writeString(wbuilder, jres);
        resp.headers["Content-Type"] = "application/json";
        return resp;
    } else {
        HttpResponse resp(401, "Unauthorized");
        resp.body = "{\"error\": \"Invalid credentials\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
}

HttpResponse handle_logout(const HttpRequest& req) {
    if (req.method != "POST") return HttpResponse(405, "Method Not Allowed");
    std::string token;
    if (!require_auth(req, token)) {
        HttpResponse resp(401, "Unauthorized");
        resp.body = "{\"error\": \"Not logged in\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
    if (HCNetSDK::Logout(token)) {
        HttpResponse resp(200, "OK");
        resp.body = "{\"result\": \"Logged out\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    } else {
        HttpResponse resp(400, "Bad Request");
        resp.body = "{\"error\": \"Invalid session token\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
}

HttpResponse handle_status(const HttpRequest& req) {
    if (req.method != "GET") return HttpResponse(405, "Method Not Allowed");
    std::string token;
    if (!require_auth(req, token)) {
        HttpResponse resp(401, "Unauthorized");
        resp.body = "{\"error\": \"Not logged in\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
    HCNetSDK::DeviceStatus status;
    if (HCNetSDK::GetStatus(status)) {
        HttpResponse resp(200, "OK");
        Json::Value jres;
        jres["state"] = status.state;
        jres["alarm"] = status.alarm;
        jres["network"] = status.network;
        jres["channels"] = status.channels;
        Json::StreamWriterBuilder wbuilder;
        resp.body = Json::writeString(wbuilder, jres);
        resp.headers["Content-Type"] = "application/json";
        return resp;
    } else {
        HttpResponse resp(500, "Internal Server Error");
        resp.body = "{\"error\": \"Failed to retrieve status\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
}

HttpResponse handle_get_config(const HttpRequest& req) {
    if (req.method != "GET") return HttpResponse(405, "Method Not Allowed");
    std::string token;
    if (!require_auth(req, token)) {
        HttpResponse resp(401, "Unauthorized");
        resp.body = "{\"error\": \"Not logged in\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
    HCNetSDK::DeviceConfig config;
    if (HCNetSDK::GetConfig(config)) {
        HttpResponse resp(200, "OK");
        Json::Value jres;
        jres["display"] = config.display;
        jres["network"] = config.network;
        jres["decoder"] = config.decoder;
        Json::StreamWriterBuilder wbuilder;
        resp.body = Json::writeString(wbuilder, jres);
        resp.headers["Content-Type"] = "application/json";
        return resp;
    } else {
        HttpResponse resp(500, "Internal Server Error");
        resp.body = "{\"error\": \"Failed to retrieve config\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
}

HttpResponse handle_put_config(const HttpRequest& req) {
    if (req.method != "PUT") return HttpResponse(405, "Method Not Allowed");
    std::string token;
    if (!require_auth(req, token)) {
        HttpResponse resp(401, "Unauthorized");
        resp.body = "{\"error\": \"Not logged in\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
    Json::Value root;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    std::istringstream iss(req.body);
    if (!Json::parseFromStream(rbuilder, iss, &root, &errs)) {
        HttpResponse resp(400, "Bad Request");
        resp.body = "{\"error\": \"Invalid JSON\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
    HCNetSDK::DeviceConfig config;
    config.display = root.get("display", "").asString();
    config.network = root.get("network", "").asString();
    config.decoder = root.get("decoder", "").asString();
    if (HCNetSDK::SetConfig(config)) {
        HttpResponse resp(200, "OK");
        resp.body = "{\"result\": \"Config updated\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    } else {
        HttpResponse resp(500, "Internal Server Error");
        resp.body = "{\"error\": \"Failed to set config\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
}

HttpResponse handle_cmd(const HttpRequest& req) {
    if (req.method != "POST") return HttpResponse(405, "Method Not Allowed");
    std::string token;
    if (!require_auth(req, token)) {
        HttpResponse resp(401, "Unauthorized");
        resp.body = "{\"error\": \"Not logged in\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
    Json::Value root;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    std::istringstream iss(req.body);
    if (!Json::parseFromStream(rbuilder, iss, &root, &errs)) {
        HttpResponse resp(400, "Bad Request");
        resp.body = "{\"error\": \"Invalid JSON\"}";
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
    std::string cmd = root.get("command", "").asString();
    Json::Value params = root.get("params", Json::Value());
    Json::Value result;
    if (HCNetSDK::ExecuteCommand(cmd, params, result)) {
        HttpResponse resp(200, "OK");
        Json::StreamWriterBuilder wbuilder;
        resp.body = Json::writeString(wbuilder, result);
        resp.headers["Content-Type"] = "application/json";
        return resp;
    } else {
        HttpResponse resp(400, "Bad Request");
        Json::Value err;
        err["error"] = "Command failed or unknown";
        Json::StreamWriterBuilder wbuilder;
        resp.body = Json::writeString(wbuilder, err);
        resp.headers["Content-Type"] = "application/json";
        return resp;
    }
}

// Dispatcher
HttpResponse dispatch(const HttpRequest& req, const std::string& device_ip, int device_port) {
    if (req.path == "/login" && req.method == "POST") return handle_login(req, device_ip, device_port);
    if (req.path == "/logout" && req.method == "POST") return handle_logout(req);
    if (req.path == "/status" && req.method == "GET") return handle_status(req);
    if (req.path == "/config" && req.method == "GET") return handle_get_config(req);
    if (req.path == "/config" && req.method == "PUT") return handle_put_config(req);
    if (req.path == "/cmd" && req.method == "POST") return handle_cmd(req);

    HttpResponse resp(404, "Not Found");
    resp.body = "{\"error\": \"Unknown endpoint\"}";
    resp.headers["Content-Type"] = "application/json";
    return resp;
}

// Server main
void serve(int port, const std::string& device_ip, int device_port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "Unable to create socket\n";
        exit(1);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        exit(1);
    }
    if (listen(server_fd, 16) < 0) {
        std::cerr << "Listen failed\n";
        exit(1);
    }
    std::cout << "HTTP server listening on port " << port << "\n";
    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;
        std::thread([client_fd, &device_ip, device_port]() {
            char buf[8192];
            ssize_t rd = read(client_fd, buf, sizeof(buf));
            if (rd <= 0) {
                close(client_fd);
                return;
            }
            std::string raw_req(buf, rd);
            HttpRequest req;
            if (!parse_http_request(raw_req, req)) {
                HttpResponse resp(400, "Bad Request");
                std::string resp_str = resp.to_string();
                write(client_fd, resp_str.c_str(), resp_str.size());
                close(client_fd);
                return;
            }
            HttpResponse resp = dispatch(req, device_ip, device_port);
            std::string resp_str = resp.to_string();
            write(client_fd, resp_str.c_str(), resp_str.size());
            close(client_fd);
        }).detach();
    }
}

int main() {
    std::string device_ip = get_env("DEVICE_IP", "127.0.0.1");
    int device_port = std::stoi(get_env("DEVICE_PORT", "8000"));
    int http_port = std::stoi(get_env("HTTP_PORT", "8080"));

    serve(http_port, device_ip, device_port);
    return 0;
}
```
