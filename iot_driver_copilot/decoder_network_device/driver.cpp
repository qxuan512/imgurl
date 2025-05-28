#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <condition_variable>
#include <json/json.h>
#include <netinet/in.h>
#include <unistd.h>

// --- Mock HCNetSDK API BEGIN (Replace with actual device SDK includes and logic) ---
namespace HCNetSDK
{
    typedef int HANDLE;
    static HANDLE login_handle = 1;
    static bool device_logged_in = false;
    static std::mutex sdk_mutex;

    struct DeviceStatus {
        std::string deviceModel = "DS-6400HD";
        std::string firmwareVersion = "V4.2.1";
        int channelCount = 16;
        bool alarm = false;
        std::string upgradeStatus = "idle";
        std::string errorCodes = "";
    };
    static DeviceStatus status;

    bool Login(const std::string& ip, int port, const std::string& user, const std::string& pass, HANDLE* handle)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (ip.empty() || user.empty() || pass.empty()) return false;
        *handle = login_handle;
        device_logged_in = true;
        return true;
    }
    bool Logout(HANDLE handle)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        device_logged_in = false;
        return true;
    }
    bool GetStatus(HANDLE handle, DeviceStatus& out)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (!device_logged_in) return false;
        out = status;
        return true;
    }
    bool GetConfig(HANDLE handle, const std::string& type, Json::Value& out)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (!device_logged_in) return false;
        if (type == "channel") { out["channels"] = 16; }
        else if (type == "display") { out["display"] = "4x4"; }
        else if (type == "decode") { out["decode_mode"] = "dynamic"; }
        else if (type == "wall") { out["wall"] = "VideoWall1"; }
        else { out["info"] = "unknown config type"; }
        return true;
    }
    bool SetConfig(HANDLE handle, const std::string& type, const Json::Value& value)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        return device_logged_in;
    }
    bool StartDecode(HANDLE handle, const Json::Value& payload)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        status.upgradeStatus = "decoding";
        return device_logged_in;
    }
    bool StopDecode(HANDLE handle, const Json::Value& payload)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        status.upgradeStatus = "idle";
        return device_logged_in;
    }
    bool Reboot(HANDLE handle)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        status.upgradeStatus = "rebooting";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        status.upgradeStatus = "idle";
        return device_logged_in;
    }
    bool UpgradeFirmware(HANDLE handle, const Json::Value& payload)
    {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        status.upgradeStatus = "upgrading";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        status.upgradeStatus = "idle";
        return device_logged_in;
    }
}
// --- Mock HCNetSDK API END ---

// --- HTTP Server Implementation BEGIN ---
#define MAX_REQUEST_SIZE 8192
#define MAX_RESPONSE_SIZE 65536
#define SESSION_TOKEN_LENGTH 32

struct HttpRequest {
    std::string method;
    std::string uri;
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

std::string get_env(const char* name, const char* defval = "") {
    const char* v = getenv(name);
    return v ? std::string(v) : std::string(defval);
}

std::string random_token(size_t len = SESSION_TOKEN_LENGTH) {
    static const char tbl[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string tok;
    for (size_t i = 0; i < len; ++i) tok += tbl[rand() % (sizeof(tbl) - 1)];
    return tok;
}

class SessionManager {
    std::map<std::string, HCNetSDK::HANDLE> sessions;
    std::mutex mtx;
public:
    std::string create_session(HCNetSDK::HANDLE handle) {
        std::lock_guard<std::mutex> lock(mtx);
        std::string token = random_token();
        sessions[token] = handle;
        return token;
    }
    HCNetSDK::HANDLE get_handle(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = sessions.find(token);
        if (it != sessions.end()) return it->second;
        return 0;
    }
    void remove_session(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx);
        sessions.erase(token);
    }
    bool valid(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx);
        return sessions.count(token) > 0;
    }
};

SessionManager session_mgr;

// --- HTTP Parsing/Formatting Utilities ---
void parse_request(const std::string& raw, HttpRequest& req) {
    std::istringstream stream(raw);
    std::string line;
    std::getline(stream, line);
    size_t m1 = line.find(' '), m2 = line.find(' ', m1 + 1);
    req.method = line.substr(0, m1);
    req.uri = line.substr(m1 + 1, m2 - m1 - 1);
    size_t qpos = req.uri.find('?');
    if (qpos != std::string::npos) {
        req.path = req.uri.substr(0, qpos);
        req.query = req.uri.substr(qpos + 1);
    } else {
        req.path = req.uri;
        req.query = "";
    }
    while (std::getline(stream, line) && line != "\r") {
        size_t c = line.find(':');
        if (c != std::string::npos) {
            std::string key = line.substr(0, c);
            std::string val = line.substr(c + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            if (!val.empty() && val.back() == '\r') val.pop_back();
            req.headers[key] = val;
        }
    }
    std::ostringstream body;
    while (std::getline(stream, line))
        body << line << "\n";
    req.body = body.str();
    if (!req.body.empty() && req.body.back() == '\n') req.body.pop_back();
}

std::string url_decode(const std::string& in) {
    std::string out;
    char a, b;
    for (size_t i = 0; i < in.length(); ++i) {
        if (in[i] == '%' && i+2 < in.length() && std::isxdigit(a = in[i+1]) && std::isxdigit(b = in[i+2])) {
            a = (a >= 'a' ? a - 'a' + 10 : (a >= 'A' ? a - 'A' + 10 : a - '0'));
            b = (b >= 'a' ? b - 'a' + 10 : (b >= 'A' ? b - 'A' + 10 : b - '0'));
            out += static_cast<char>(16*a+b);
            i += 2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> qmap;
    size_t p = 0, eq, amp;
    while (p < query.size()) {
        eq = query.find('=', p);
        amp = query.find('&', p);
        if (eq == std::string::npos) break;
        std::string key = url_decode(query.substr(p, eq-p));
        std::string val = (amp == std::string::npos)
            ? url_decode(query.substr(eq+1))
            : url_decode(query.substr(eq+1, amp-eq-1));
        qmap[key] = val;
        if (amp == std::string::npos) break;
        p = amp+1;
    }
    return qmap;
}

void send_response(int client, const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_message << "\r\n";
    for (auto& h : resp.headers)
        oss << h.first << ": " << h.second << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n\r\n";
    oss << resp.body;
    std::string out = oss.str();
    send(client, out.c_str(), out.size(), 0);
}

void respond_json(int client, int status, const std::string& status_msg, const Json::Value& json) {
    HttpResponse resp;
    resp.status_code = status;
    resp.status_message = status_msg;
    resp.headers["Content-Type"] = "application/json";
    Json::StreamWriterBuilder builder;
    resp.body = Json::writeString(builder, json);
    send_response(client, resp);
}

void respond_err(int client, int status, const std::string& msg) {
    Json::Value js;
    js["error"] = msg;
    respond_json(client, status, "Error", js);
}

// --- Endpoint Handlers ---
void handle_login(int client, const HttpRequest& req) {
    Json::Value js, out;
    Json::Reader reader;
    if (!reader.parse(req.body, js)) {
        respond_err(client, 400, "Invalid JSON");
        return;
    }
    std::string ip = get_env("DEVICE_IP");
    int port = std::stoi(get_env("DEVICE_PORT", "8000"));
    std::string user = js.get("username", "").asString();
    std::string pass = js.get("password", "").asString();
    if (user.empty() || pass.empty()) {
        respond_err(client, 400, "Missing credentials");
        return;
    }
    HCNetSDK::HANDLE h = 0;
    if (!HCNetSDK::Login(ip, port, user, pass, &h)) {
        respond_err(client, 401, "Login failed");
        return;
    }
    std::string token = session_mgr.create_session(h);
    out["token"] = token;
    respond_json(client, 200, "OK", out);
}

void handle_logout(int client, const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (token.rfind("Bearer ", 0) == 0) token = token.substr(7);
    if (!session_mgr.valid(token)) {
        respond_err(client, 401, "Invalid session token");
        return;
    }
    HCNetSDK::HANDLE h = session_mgr.get_handle(token);
    HCNetSDK::Logout(h);
    session_mgr.remove_session(token);
    Json::Value js;
    js["result"] = "success";
    respond_json(client, 200, "OK", js);
}

void handle_status(int client, const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (token.rfind("Bearer ", 0) == 0) token = token.substr(7);
    if (!session_mgr.valid(token)) {
        respond_err(client, 401, "Invalid session token");
        return;
    }
    HCNetSDK::HANDLE h = session_mgr.get_handle(token);
    HCNetSDK::DeviceStatus stat;
    if (!HCNetSDK::GetStatus(h, stat)) {
        respond_err(client, 500, "Failed to retrieve device status");
        return;
    }
    Json::Value js;
    js["deviceModel"] = stat.deviceModel;
    js["firmwareVersion"] = stat.firmwareVersion;
    js["channelCount"] = stat.channelCount;
    js["alarm"] = stat.alarm;
    js["upgradeStatus"] = stat.upgradeStatus;
    js["errorCodes"] = stat.errorCodes;
    respond_json(client, 200, "OK", js);
}

void handle_get_config(int client, const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (token.rfind("Bearer ", 0) == 0) token = token.substr(7);
    if (!session_mgr.valid(token)) {
        respond_err(client, 401, "Invalid session token");
        return;
    }
    auto qmap = parse_query(req.query);
    std::string type = qmap.count("type") ? qmap["type"] : "";
    if (type.empty()) type = "display";
    HCNetSDK::HANDLE h = session_mgr.get_handle(token);
    Json::Value js;
    if (!HCNetSDK::GetConfig(h, type, js)) {
        respond_err(client, 500, "Failed to retrieve config");
        return;
    }
    respond_json(client, 200, "OK", js);
}

void handle_put_config(int client, const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (token.rfind("Bearer ", 0) == 0) token = token.substr(7);
    if (!session_mgr.valid(token)) {
        respond_err(client, 401, "Invalid session token");
        return;
    }
    auto qmap = parse_query(req.query);
    std::string type = qmap.count("type") ? qmap["type"] : "";
    if (type.empty()) type = "display";
    Json::Value cfg;
    Json::Reader reader;
    if (!reader.parse(req.body, cfg)) {
        respond_err(client, 400, "Invalid JSON");
        return;
    }
    HCNetSDK::HANDLE h = session_mgr.get_handle(token);
    if (!HCNetSDK::SetConfig(h, type, cfg)) {
        respond_err(client, 500, "Failed to set config");
        return;
    }
    Json::Value js;
    js["result"] = "success";
    respond_json(client, 200, "OK", js);
}

void handle_decode_op(int client, const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (token.rfind("Bearer ", 0) == 0) token = token.substr(7);
    if (!session_mgr.valid(token)) {
        respond_err(client, 401, "Invalid session token");
        return;
    }
    Json::Value js;
    Json::Reader reader;
    if (!reader.parse(req.body, js)) {
        respond_err(client, 400, "Invalid JSON");
        return;
    }
    std::string action = js.get("action", "").asString();
    std::string mode = js.get("mode", "dynamic").asString();
    HCNetSDK::HANDLE h = session_mgr.get_handle(token);
    bool ok = false;
    if (action == "start") {
        ok = HCNetSDK::StartDecode(h, js);
    } else if (action == "stop") {
        ok = HCNetSDK::StopDecode(h, js);
    } else {
        respond_err(client, 400, "Invalid action (must be 'start' or 'stop')");
        return;
    }
    if (!ok) {
        respond_err(client, 500, "Decode operation failed");
        return;
    }
    Json::Value out;
    out["result"] = "success";
    out["mode"] = mode;
    respond_json(client, 200, "OK", out);
}

void handle_reboot(int client, const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (token.rfind("Bearer ", 0) == 0) token = token.substr(7);
    if (!session_mgr.valid(token)) {
        respond_err(client, 401, "Invalid session token");
        return;
    }
    HCNetSDK::HANDLE h = session_mgr.get_handle(token);
    if (!HCNetSDK::Reboot(h)) {
        respond_err(client, 500, "Failed to reboot");
        return;
    }
    Json::Value js;
    js["result"] = "rebooting";
    respond_json(client, 200, "OK", js);
}

void handle_upgrade(int client, const HttpRequest& req) {
    std::string token = req.headers.count("Authorization") ? req.headers.at("Authorization") : "";
    if (token.rfind("Bearer ", 0) == 0) token = token.substr(7);
    if (!session_mgr.valid(token)) {
        respond_err(client, 401, "Invalid session token");
        return;
    }
    Json::Value js;
    Json::Reader reader;
    if (!reader.parse(req.body, js)) {
        respond_err(client, 400, "Invalid JSON");
        return;
    }
    HCNetSDK::HANDLE h = session_mgr.get_handle(token);
    if (!HCNetSDK::UpgradeFirmware(h, js)) {
        respond_err(client, 500, "Upgrade failed");
        return;
    }
    Json::Value out;
    out["result"] = "upgrade started";
    respond_json(client, 200, "OK", out);
}

// --- Main Routing/Handler ---
void handle_http(int client, const HttpRequest& req) {
    if (req.method == "POST" && req.path == "/login") {
        handle_login(client, req);
    } else if (req.method == "POST" && req.path == "/logout") {
        handle_logout(client, req);
    } else if (req.method == "GET" && req.path == "/status") {
        handle_status(client, req);
    } else if (req.method == "GET" && req.path == "/config") {
        handle_get_config(client, req);
    } else if (req.method == "PUT" && req.path == "/config") {
        handle_put_config(client, req);
    } else if (req.method == "POST" && (req.path == "/decode" || req.path == "/command/decode")) {
        handle_decode_op(client, req);
    } else if (req.method == "POST" && (req.path == "/reboot" || req.path == "/command/reboot")) {
        handle_reboot(client, req);
    } else if (req.method == "POST" && (req.path == "/upgrade" || req.path == "/command/upgrade")) {
        handle_upgrade(client, req);
    } else {
        respond_err(client, 404, "Not found");
    }
}

void http_worker(int client) {
    char buffer[MAX_REQUEST_SIZE] = {0};
    int len = recv(client, buffer, sizeof(buffer)-1, 0);
    if (len <= 0) {
        close(client);
        return;
    }
    buffer[len] = 0;
    HttpRequest req;
    parse_request(buffer, req);
    handle_http(client, req);
    close(client);
}

void http_server_main() {
    int port = std::stoi(get_env("HTTP_PORT", "8080"));
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(server_fd, 16) < 0) {
        perror("listen");
        exit(1);
    }
    std::cout << "HTTP Server started on port " << port << std::endl;
    while (true) {
        int client = accept(server_fd, NULL, NULL);
        if (client >= 0) {
            std::thread(http_worker, client).detach();
        }
    }
}

// --- MAIN ---
int main(int argc, char** argv) {
    srand(time(nullptr));
    std::thread http_thread(http_server_main);
    http_thread.join();
    return 0;
}