#include <iostream>
#include <string>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <sstream>
#include <fstream>
#include <functional>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

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

// Utility: Simple JSON generator/escape for safe output
std::string json_escape(const std::string& in) {
    std::ostringstream oss;
    for (char c : in) {
        switch (c) {
            case '\"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f')
                    oss << "\\u"
                        << std::hex << std::uppercase << (int)c;
                else
                    oss << c;
        }
    }
    return oss.str();
}
std::string json_kv(const std::string& k, const std::string& v) {
    return "\"" + json_escape(k) + "\":\"" + json_escape(v) + "\"";
}
std::string json_kv(const std::string& k, int v) {
    return "\"" + json_escape(k) + "\":" + std::to_string(v);
}

// Environment variable helper
std::string env_or(const char* k, const char* def) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(def);
}
int env_or_int(const char* k, int def) {
    const char* v = std::getenv(k);
    return v ? std::atoi(v) : def;
}

// HTTP status codes
std::map<int, std::string> status_map = {
    {200, "OK"},
    {201, "Created"},
    {204, "No Content"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"}
};

// --- Hikvision Device Protocol Stub Implementation ---
// These simulate communication with the Hikvision device via proprietary protocol.
// In production, replace these with real SDK calls.

struct Session {
    std::string token;
    std::string username;
    bool active;
    time_t last_active;
};
std::mutex session_mutex;
std::map<std::string, Session> sessions;

std::string generate_token() {
    static int seq = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "session_%d_%ld", ++seq, std::time(nullptr));
    return std::string(buf);
}

// "Device" status mock
std::map<std::string, std::string> device_status = {
    {"channel_status", "online"},
    {"alarm_status", "normal"},
    {"error_codes", ""},
    {"sdk_state", "connected"}
};
// "Device" config mock
std::map<std::string, std::string> device_config = {
    {"decoder_channels", "4"},
    {"loop_decode", "enabled"},
    {"scene", "default"}
};

bool check_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    auto it = sessions.find(token);
    if (it != sessions.end() && it->second.active) {
        it->second.last_active = std::time(nullptr);
        return true;
    }
    return false;
}
void logout_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    auto it = sessions.find(token);
    if (it != sessions.end()) {
        it->second.active = false;
    }
}

// Simulated device protocol functions
bool device_login(const std::string& username, const std::string& password, std::string& out_token) {
    // Suppose "admin" / "hik12345" are valid
    if (username == "admin" && password == "hik12345") {
        out_token = generate_token();
        Session s;
        s.token = out_token;
        s.username = username;
        s.active = true;
        s.last_active = std::time(nullptr);
        {
            std::lock_guard<std::mutex> lock(session_mutex);
            sessions[out_token] = s;
        }
        return true;
    }
    return false;
}
void device_logout(const std::string& token) {
    logout_token(token);
}
std::map<std::string, std::string> device_get_status() {
    return device_status;
}
std::map<std::string, std::string> device_get_config() {
    return device_config;
}
bool device_set_config(const std::map<std::string, std::string>& new_cfg) {
    for (const auto& kv : new_cfg) {
        device_config[kv.first] = kv.second;
    }
    return true;
}
bool device_reboot() {
    // Simulate reboot: clear status, re-init config
    device_status["sdk_state"] = "rebooting";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    device_status["sdk_state"] = "connected";
    return true;
}
bool device_decode_control(const std::string& action, const std::string& type) {
    device_status["decode_status"] = action + "_" + type;
    return true;
}
bool device_playback_control(const std::map<std::string, std::string>& params) {
    device_status["playback"] = "active";
    return true;
}

// Parse JSON (minimal, for this driver, not robust!)
std::map<std::string, std::string> parse_json_kv(const std::string& body) {
    std::map<std::string, std::string> out;
    size_t s = body.find('{');
    size_t e = body.find('}');
    if (s == std::string::npos || e == std::string::npos) return out;
    std::string inner = body.substr(s+1, e-s-1);
    size_t p = 0;
    while (p < inner.size()) {
        size_t k1 = inner.find('"', p);
        if (k1 == std::string::npos) break;
        size_t k2 = inner.find('"', k1+1);
        if (k2 == std::string::npos) break;
        std::string key = inner.substr(k1+1, k2-k1-1);
        size_t c = inner.find(':', k2);
        if (c == std::string::npos) break;
        size_t v1 = inner.find_first_of("\"0123456789", c+1);
        if (v1 == std::string::npos) break;
        std::string val;
        if (inner[v1] == '"') {
            size_t v2 = inner.find('"', v1+1);
            if (v2 == std::string::npos) break;
            val = inner.substr(v1+1, v2-v1-1);
            p = v2+1;
        } else {
            size_t v2 = inner.find_first_not_of("0123456789.", v1);
            val = inner.substr(v1, v2-v1);
            p = v2;
        }
        out[key] = val;
        p = inner.find(',', p);
        if (p == std::string::npos) break;
        ++p;
    }
    return out;
}

// HTTP parsing
struct HTTPRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string get_query(const std::string& k) const {
        auto it = query.find(k);
        return it != query.end() ? it->second : "";
    }
    std::string get_header(const std::string& k) const {
        auto it = headers.find(k);
        return it != headers.end() ? it->second : "";
    }
};

// Parse query string
std::map<std::string, std::string> parse_query(const std::string& q) {
    std::map<std::string, std::string> res;
    size_t p = 0;
    while (p < q.size()) {
        size_t e = q.find('&', p);
        std::string kv = q.substr(p, e-p);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            res[kv.substr(0, eq)] = kv.substr(eq+1);
        }
        if (e == std::string::npos) break;
        p = e+1;
    }
    return res;
}
void trim(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
}

// Parse HTTP request from socket
bool parse_http_request(std::istream& in, HTTPRequest& req) {
    std::string line;
    if (!std::getline(in, line)) return false;
    trim(line);
    std::istringstream iss(line);
    if (!(iss >> req.method)) return false;
    std::string url;
    if (!(iss >> url)) return false;
    size_t q = url.find('?');
    if (q != std::string::npos) {
        req.path = url.substr(0, q);
        req.query = parse_query(url.substr(q+1));
    } else {
        req.path = url;
    }
    std::string ver;
    if (!(iss >> ver)) return false;
    std::map<std::string, std::string> headers;
    while (std::getline(in, line)) {
        trim(line);
        if (line.empty()) break;
        size_t c = line.find(':');
        if (c != std::string::npos) {
            std::string k = line.substr(0, c);
            std::string v = line.substr(c+1);
            while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0,1);
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            headers[k] = v;
        }
    }
    req.headers = headers;
    // Read body if Content-Length
    auto it = headers.find("content-length");
    if (it != headers.end()) {
        int len = std::stoi(it->second);
        std::string body(len, '\0');
        in.read(&body[0], len);
        req.body = body;
    }
    return true;
}

// HTTP response
void send_http_response(std::ostream& out, int code, const std::string& content, const std::string& content_type="application/json") {
    out << "HTTP/1.1 " << code << " " << status_map[code] << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << content.size() << "\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Connection: close\r\n\r\n";
    out << content;
}

// --- HTTP Handlers ---

void handle_login(const HTTPRequest& req, std::ostream& out) {
    auto kv = parse_json_kv(req.body);
    std::string user = kv["username"];
    std::string pass = kv["password"];
    std::string token;
    if (device_login(user, pass, token)) {
        send_http_response(out, 200, "{"+json_kv("token", token)+"}");
    } else {
        send_http_response(out, 401, "{\"error\":\"Invalid credentials\"}");
    }
}
void handle_logout(const HTTPRequest& req, std::ostream& out) {
    std::string token = req.get_header("authorization");
    if (token.empty()) token = req.get_query("token");
    if (!check_token(token)) {
        send_http_response(out, 401, "{\"error\":\"Invalid or expired token\"}");
        return;
    }
    device_logout(token);
    send_http_response(out, 200, "{\"result\":\"Logged out\"}");
}
void handle_status(const HTTPRequest& req, std::ostream& out) {
    std::string token = req.get_header("authorization");
    if (token.empty()) token = req.get_query("token");
    if (!check_token(token)) {
        send_http_response(out, 401, "{\"error\":\"Invalid or expired token\"}");
        return;
    }
    // Accepts pagination/filter query params, here we just ignore for simplicity
    auto st = device_get_status();
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& kv : st) {
        if (!first) oss << ",";
        oss << json_kv(kv.first, kv.second);
        first = false;
    }
    oss << "}";
    send_http_response(out, 200, oss.str());
}
void handle_get_config(const HTTPRequest& req, std::ostream& out) {
    std::string token = req.get_header("authorization");
    if (token.empty()) token = req.get_query("token");
    if (!check_token(token)) {
        send_http_response(out, 401, "{\"error\":\"Invalid or expired token\"}");
        return;
    }
    // Filter query param support can be added here
    auto cfg = device_get_config();
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& kv : cfg) {
        if (!first) oss << ",";
        oss << json_kv(kv.first, kv.second);
        first = false;
    }
    oss << "}";
    send_http_response(out, 200, oss.str());
}
void handle_put_config(const HTTPRequest& req, std::ostream& out) {
    std::string token = req.get_header("authorization");
    if (token.empty()) token = req.get_query("token");
    if (!check_token(token)) {
        send_http_response(out, 401, "{\"error\":\"Invalid or expired token\"}");
        return;
    }
    auto new_cfg = parse_json_kv(req.body);
    if (device_set_config(new_cfg)) {
        send_http_response(out, 200, "{\"result\":\"Configuration updated\"}");
    } else {
        send_http_response(out, 500, "{\"error\":\"Failed to update configuration\"}");
    }
}
void handle_reboot(const HTTPRequest& req, std::ostream& out) {
    std::string token = req.get_header("authorization");
    if (token.empty()) token = req.get_query("token");
    if (!check_token(token)) {
        send_http_response(out, 401, "{\"error\":\"Invalid or expired token\"}");
        return;
    }
    if (device_reboot()) {
        send_http_response(out, 200, "{\"result\":\"Device rebooted\"}");
    } else {
        send_http_response(out, 500, "{\"error\":\"Failed to reboot device\"}");
    }
}
void handle_decode(const HTTPRequest& req, std::ostream& out) {
    std::string token = req.get_header("authorization");
    if (token.empty()) token = req.get_query("token");
    if (!check_token(token)) {
        send_http_response(out, 401, "{\"error\":\"Invalid or expired token\"}");
        return;
    }
    auto kv = parse_json_kv(req.body);
    std::string action = kv["action"];
    std::string type = kv["type"];
    if (action.empty() || (action != "start" && action != "stop")) {
        send_http_response(out, 400, "{\"error\":\"Invalid action (start/stop) required\"}");
        return;
    }
    if (type.empty()) type = "active";
    if (device_decode_control(action, type)) {
        send_http_response(out, 200, "{\"result\":\"Decode "+action+" "+type+"\"}");
    } else {
        send_http_response(out, 500, "{\"error\":\"Decode control failed\"}");
    }
}
void handle_playback(const HTTPRequest& req, std::ostream& out) {
    std::string token = req.get_header("authorization");
    if (token.empty()) token = req.get_query("token");
    if (!check_token(token)) {
        send_http_response(out, 401, "{\"error\":\"Invalid or expired token\"}");
        return;
    }
    auto params = parse_json_kv(req.body);
    if (device_playback_control(params)) {
        send_http_response(out, 200, "{\"result\":\"Playback command sent\"}");
    } else {
        send_http_response(out, 500, "{\"error\":\"Playback control failed\"}");
    }
}

// --- HTTP Router ---
void route_http(const HTTPRequest& req, std::ostream& out) {
    if (req.path == "/login" && req.method == "POST") {
        handle_login(req, out);
    } else if (req.path == "/logout" && req.method == "POST") {
        handle_logout(req, out);
    } else if (req.path == "/status" && req.method == "GET") {
        handle_status(req, out);
    } else if (req.path == "/config" && req.method == "GET") {
        handle_get_config(req, out);
    } else if (req.path == "/config" && req.method == "PUT") {
        handle_put_config(req, out);
    } else if (req.path == "/reboot" && req.method == "POST") {
        handle_reboot(req, out);
    } else if (req.path == "/decode" && req.method == "POST") {
        handle_decode(req, out);
    } else if (req.path == "/playback" && req.method == "POST") {
        handle_playback(req, out);
    } else {
        send_http_response(out, 404, "{\"error\":\"Not found\"}");
    }
}

// --- HTTP Server (minimal, single-thread per connection) ---

class HTTPServer {
public:
    HTTPServer(const std::string& host, int port)
        : host_(host), port_(port), running_(false)
    {}

    void start() {
        running_ = true;
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            std::cerr << "Failed to create socket\n";
            return;
        }
        int yes = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Bind failed\n";
#ifdef _WIN32
            closesocket(listen_fd);
#else
            close(listen_fd);
#endif
            return;
        }
        if (listen(listen_fd, 16) < 0) {
            std::cerr << "Listen failed\n";
#ifdef _WIN32
            closesocket(listen_fd);
#else
            close(listen_fd);
#endif
            return;
        }
        std::cout << "[HTTP] Server started on port " << port_ << std::endl;
        while (running_) {
            sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int client_fd =
#ifdef _WIN32
                accept(listen_fd, (sockaddr*)&client_addr, &addrlen);
#else
                accept(listen_fd, (sockaddr*)&client_addr, &addrlen);
#endif
            if (client_fd < 0) continue;
            std::thread([client_fd]() {
#ifdef _WIN32
                FILE* fp = _fdopen(client_fd, "r+");
#else
                FILE* fp = fdopen(client_fd, "r+");
#endif
                if (fp) {
                    std::istream in(fp);
                    std::ostream out(fp);
                    HTTPRequest req;
                    if (parse_http_request(in, req)) {
                        route_http(req, out);
                    }
                    fflush(fp);
                    fclose(fp);
                }
#ifdef _WIN32
                closesocket(client_fd);
#else
                close(client_fd);
#endif
            }).detach();
        }
#ifdef _WIN32
        closesocket(listen_fd);
#else
        close(listen_fd);
#endif
    }

    void stop() {
        running_ = false;
    }
private:
    std::string host_;
    int port_;
    bool running_;
};

int main() {
    std::string host = env_or("HTTP_SERVER_HOST", "0.0.0.0");
    int port = env_or_int("HTTP_SERVER_PORT", 8080);

    HTTPServer server(host, port);
    server.start();
    return 0;
}