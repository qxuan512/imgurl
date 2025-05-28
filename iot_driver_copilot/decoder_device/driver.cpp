// hikvision_decoder_http_driver.cpp
#include <iostream>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <vector>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdarg>
#include <unordered_map>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

// --- Simple JSON parser/writer for C++ ---
#include <json/json.h>

// ---- Minimal HTTP Server (POSIX sockets) ----
#define RECV_BUF_SIZE 8192
#define SEND_BUF_SIZE 4096

// --- Session Management ---
struct Session {
    std::string token;
    std::string username;
    time_t expiry;
};

std::mutex g_sessions_mutex;
std::map<std::string, Session> g_sessions;
const int SESSION_TIMEOUT_SEC = 3600; // 1 hour

std::string generate_token() {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    int len = 32;
    std::string token;
    for (int i = 0; i < len; ++i) {
        token += alphanum[rand() % (sizeof(alphanum) - 1)];
    }
    return token;
}

bool is_token_valid(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto it = g_sessions.find(token);
    if (it == g_sessions.end()) return false;
    if (it->second.expiry < std::time(nullptr)) {
        g_sessions.erase(it);
        return false;
    }
    return true;
}

std::string get_username_from_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto it = g_sessions.find(token);
    if (it == g_sessions.end()) return "";
    return it->second.username;
}

void invalidate_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    g_sessions.erase(token);
}

// --- Device (Fake) SDK/RTSP/Config Simulation ---
struct DeviceConfig {
    Json::Value displayConfig;
    Json::Value sceneConfig;
    Json::Value otherConfig;
};

DeviceConfig g_device_config;
std::mutex g_device_mutex;

// --- Environment Variables ---
std::string getenv_or_default(const char* key, const char* def) {
    const char* v = std::getenv(key);
    return v ? v : def;
}

std::string DEVICE_IP;
int DEVICE_PORT;
std::string DEVICE_USER;
std::string DEVICE_PASS;
std::string HTTP_HOST;
int HTTP_PORT;

void load_env() {
    DEVICE_IP = getenv_or_default("DEVICE_IP", "192.168.1.64");
    DEVICE_PORT = std::stoi(getenv_or_default("DEVICE_PORT", "8000"));
    DEVICE_USER = getenv_or_default("DEVICE_USER", "admin");
    DEVICE_PASS = getenv_or_default("DEVICE_PASS", "12345");
    HTTP_HOST = getenv_or_default("HTTP_HOST", "0.0.0.0");
    HTTP_PORT = std::stoi(getenv_or_default("HTTP_PORT", "8080"));
}

// --- HTTP Utilities ---
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string protocol;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int code;
    std::string message;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string http_status_message(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

std::string http_response_to_string(const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.code << " " << http_status_message(resp.code) << "\r\n";
    for (const auto& h : resp.headers) {
        oss << h.first << ": " << h.second << "\r\n";
    }
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << resp.body;
    return oss.str();
}

bool parse_http_request(const std::string& data, HttpRequest& req) {
    std::istringstream ss(data);
    std::string line;
    if (!std::getline(ss, line)) return false;
    std::istringstream reqline(line);
    if (!(reqline >> req.method >> req.path >> req.protocol)) return false;
    // Split query string
    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        req.query = req.path.substr(qpos + 1);
        req.path = req.path.substr(0, qpos);
    }
    // Headers
    std::string header;
    while (std::getline(ss, header) && header != "\r") {
        auto pos = header.find(':');
        if (pos != std::string::npos) {
            std::string key = header.substr(0, pos);
            std::string value = header.substr(pos+1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            req.headers[key] = value;
        }
    }
    // Body
    std::string body;
    std::getline(ss, body, '\0');
    req.body = body;
    return true;
}

std::string get_auth_token(const HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it != req.headers.end()) {
        std::string v = it->second;
        if (v.find("Bearer ") == 0)
            return v.substr(7);
        else
            return v;
    }
    // Try from cookie?
    return "";
}

// ---- Helper: URL Decoding ----
std::string url_decode(const std::string& in) {
    std::string out;
    char ch;
    int i, ii;
    for (i=0; i < in.length(); ++i) {
        if (in[i] == '%') {
            sscanf(in.substr(i+1,2).c_str(), "%x", &ii);
            ch=static_cast<char>(ii);
            out+=ch;
            i=i+2;
        } else if (in[i]=='+') out+=' ';
        else out+=in[i];
    }
    return out;
}

std::map<std::string,std::string> parse_query(const std::string& query) {
    std::map<std::string,std::string> m;
    size_t start = 0, end = 0;
    while ((end = query.find('&', start)) != std::string::npos) {
        std::string kv = query.substr(start, end-start);
        size_t eq = kv.find('=');
        if (eq != std::string::npos)
            m[url_decode(kv.substr(0,eq))] = url_decode(kv.substr(eq+1));
        start = end+1;
    }
    if (start < query.length()) {
        std::string kv = query.substr(start);
        size_t eq = kv.find('=');
        if (eq != std::string::npos)
            m[url_decode(kv.substr(0,eq))] = url_decode(kv.substr(eq+1));
    }
    return m;
}

// --- Device Simulation Functions (Replace with real SDK/RTSP!) ---
bool sdk_initialized = false;
bool device_logged_in = false;
std::string current_device_user;

bool login_device(const std::string& user, const std::string& pass) {
    if (user == DEVICE_USER && pass == DEVICE_PASS) {
        device_logged_in = true;
        current_device_user = user;
        return true;
    }
    return false;
}
void logout_device() {
    device_logged_in = false;
    current_device_user.clear();
}
bool sdk_action(const std::string& action) {
    if (action == "init") { sdk_initialized = true; return true; }
    if (action == "cleanup") { sdk_initialized = false; return true; }
    return false;
}
bool system_action(const std::string& action) {
    if (action == "reboot" || action == "shutdown") {
        // Simulate reboot/shutdown
        return true;
    }
    if (action == "upgrade") return true;
    return false;
}
bool decode_action(const std::string& action, const Json::Value&) {
    return (action == "start" || action == "stop");
}
bool playback_action(const std::string& action, const Json::Value&) {
    return (action == "start" || action == "stop" || action == "pause" || action == "resume");
}
Json::Value get_device_status() {
    Json::Value v;
    v["model"] = "DS-64XXHD_S";
    v["channels"] = 16;
    v["decoding"] = sdk_initialized && device_logged_in;
    v["playback"] = false;
    v["alarm"] = false;
    v["stream"] = "rtsp://"+DEVICE_IP+":554/Streaming/Channels/101";
    return v;
}
Json::Value get_device_info() {
    Json::Value v;
    v["device_name"] = "Decoder Device";
    v["device_model"] = "DS-64XXHD_S";
    v["manufacturer"] = "Hikvision";
    v["device_type"] = "Network Video Decoder";
    v["sdk_version"] = "5.3.9";
    v["error_codes"] = Json::arrayValue;
    v["status"] = "OK";
    return v;
}
Json::Value get_config(const std::string& type) {
    std::lock_guard<std::mutex> lock(g_device_mutex);
    if (type == "display") return g_device_config.displayConfig;
    if (type == "scene") return g_device_config.sceneConfig;
    if (type == "other") return g_device_config.otherConfig;
    Json::Value v;
    v["display"] = g_device_config.displayConfig;
    v["scene"] = g_device_config.sceneConfig;
    v["other"] = g_device_config.otherConfig;
    return v;
}
void set_config(const std::string& type, const Json::Value& val) {
    std::lock_guard<std::mutex> lock(g_device_mutex);
    if (type == "display") g_device_config.displayConfig = val;
    if (type == "scene") g_device_config.sceneConfig = val;
    if (type == "other") g_device_config.otherConfig = val;
}

// --- RTSP to HTTP MJPEG Proxy (Fake Stream) ---
void write_mjpeg_stream(int connfd) {
    // Simulate MJPEG stream with random images (black frame)
    const std::string boundary = "--myboundary";
    for (int i = 0; i < 100; ++i) {
        usleep(100000); // 10 FPS
        unsigned char jpeg_fake[200] = {
            0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0x00,
            // ... just a fake jpeg header to make browser happy ...
            0xff, 0xd9
        };
        std::ostringstream oss;
        oss << boundary << "\r\n";
        oss << "Content-Type: image/jpeg\r\n";
        oss << "Content-Length: " << sizeof(jpeg_fake) << "\r\n\r\n";
        oss.write((const char*)jpeg_fake, sizeof(jpeg_fake));
        oss << "\r\n";
        std::string chunk = oss.str();
        if (send(connfd, chunk.data(), chunk.size(), 0) < 0) break;
    }
}

// --- Main HTTP Handler Table ---
using HandlerFn = std::function<HttpResponse(const HttpRequest&)>;
std::unordered_map<std::string, std::map<std::string, HandlerFn>> http_routes;

#define REQUIRE_AUTH \
    std::string token = get_auth_token(req); \
    if (token.empty() || !is_token_valid(token)) { \
        return HttpResponse{401, "Unauthorized", {{"Content-Type", "application/json"}}, "{\"error\":\"Unauthorized\"}"}; \
    }

// --- API Handlers Implementation ---
void register_handlers() {
    // POST /login (for backward compatibility, also /session POST is supported)
    http_routes["POST"]["/login"] = [](const HttpRequest& req) -> HttpResponse {
        Json::Value v;
        Json::Reader reader;
        if (!reader.parse(req.body, v)) {
            return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid JSON\"}"};
        }
        std::string user = v.get("username", "").asString();
        std::string pass = v.get("password", "").asString();
        if (login_device(user, pass)) {
            std::string token = generate_token();
            Session sess{token, user, std::time(nullptr) + SESSION_TIMEOUT_SEC};
            {
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                g_sessions[token] = sess;
            }
            Json::Value resp;
            resp["token"] = token;
            return {200, "OK", {{"Content-Type","application/json"}}, Json::FastWriter().write(resp)};
        } else {
            return {401, "Unauthorized", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid credentials\"}"};
        }
    };

    // POST /session (login)
    http_routes["POST"]["/session"] = http_routes["POST"]["/login"];

    // DELETE /session (logout)
    http_routes["DELETE"]["/session"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        invalidate_token(token);
        logout_device();
        return {200, "OK", {{"Content-Type","application/json"}}, "{\"message\":\"Logged out\"}"};
    };

    // POST /sdk
    http_routes["POST"]["/sdk"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        Json::Value v;
        Json::Reader reader;
        if (!reader.parse(req.body, v)) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid JSON\"}"};
        std::string action = v.get("action", "").asString();
        if (action.empty()) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Missing action\"}"};
        if (sdk_action(action)) {
            return {200, "OK", {{"Content-Type","application/json"}}, "{\"message\":\"SDK action successful\"}"};
        } else {
            return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid SDK action\"}"};
        }
    };

    // POST /system
    http_routes["POST"]["/system"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        Json::Value v;
        Json::Reader reader;
        if (!reader.parse(req.body, v)) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid JSON\"}"};
        std::string action = v.get("action", "").asString();
        if (action.empty()) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Missing action\"}"};
        if (system_action(action)) {
            return {200, "OK", {{"Content-Type","application/json"}}, "{\"message\":\"System action successful\"}"};
        } else {
            return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid system action\"}"};
        }
    };

    // POST /decode
    http_routes["POST"]["/decode"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        Json::Value v;
        Json::Reader reader;
        if (!reader.parse(req.body, v)) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid JSON\"}"};
        std::string action = v.get("action", "").asString();
        if (action.empty()) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Missing action\"}"};
        if (decode_action(action, v)) {
            return {200, "OK", {{"Content-Type","application/json"}}, "{\"message\":\"Decode action successful\"}"};
        } else {
            return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid decode action\"}"};
        }
    };

    // POST /playback
    http_routes["POST"]["/playback"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        Json::Value v;
        Json::Reader reader;
        if (!reader.parse(req.body, v)) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid JSON\"}"};
        std::string action = v.get("action", "").asString();
        if (action.empty()) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Missing action\"}"};
        if (playback_action(action, v)) {
            return {200, "OK", {{"Content-Type","application/json"}}, "{\"message\":\"Playback action successful\"}"};
        } else {
            return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid playback action\"}"};
        }
    };

    // POST /upgrade
    http_routes["POST"]["/upgrade"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        // Simulate upgrade
        return {200, "OK", {{"Content-Type","application/json"}}, "{\"message\":\"Upgrade started\"}"};
    };

    // POST /reboot
    http_routes["POST"]["/reboot"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        // Simulate reboot/shutdown
        return {200, "OK", {{"Content-Type","application/json"}}, "{\"message\":\"Reboot command issued\"}"};
    };

    // GET /device
    http_routes["GET"]["/device"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        Json::Value v = get_device_info();
        return {200, "OK", {{"Content-Type","application/json"}}, Json::FastWriter().write(v)};
    };

    // GET /status
    http_routes["GET"]["/status"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        Json::Value v = get_device_status();
        return {200, "OK", {{"Content-Type","application/json"}}, Json::FastWriter().write(v)};
    };

    // GET /config
    http_routes["GET"]["/config"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        std::string type;
        if (!req.query.empty()) {
            auto params = parse_query(req.query);
            type = params["type"];
        }
        Json::Value v = get_config(type);
        return {200, "OK", {{"Content-Type","application/json"}}, Json::FastWriter().write(v)};
    };

    // PUT /config
    http_routes["PUT"]["/config"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        Json::Value v;
        Json::Reader reader;
        if (!reader.parse(req.body, v)) return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Invalid JSON\"}"};
        std::string type = v.get("type", "").asString();
        if (type.empty() || !v.isMember("config")) {
            return {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Missing type or config\"}"};
        }
        set_config(type, v["config"]);
        return {200, "OK", {{"Content-Type","application/json"}}, "{\"message\":\"Config updated\"}"};
    };

    // MJPEG HTTP proxy from RTSP (simulate): GET /stream
    http_routes["GET"]["/stream"] = [](const HttpRequest& req) -> HttpResponse {
        REQUIRE_AUTH
        // Special: handled in main loop as chunked MJPEG stream
        return {200, "OK", {{"X-MJPEG-Stream","1"}}, ""};
    };
}

// --- Main HTTP Server Loop ---
void handle_client(int connfd) {
    char buffer[RECV_BUF_SIZE] = {0};
    int n = recv(connfd, buffer, RECV_BUF_SIZE-1, 0);
    if (n <= 0) { close(connfd); return; }
    buffer[n] = 0;
    std::string raw_req(buffer);

    HttpRequest req;
    if (!parse_http_request(raw_req, req)) {
        HttpResponse resp = {400, "Bad Request", {{"Content-Type","application/json"}}, "{\"error\":\"Malformed request\"}"};
        std::string resp_str = http_response_to_string(resp);
        send(connfd, resp_str.c_str(), resp_str.size(), 0);
        close(connfd); return;
    }

    // MJPEG stream endpoint
    if (req.method == "GET" && req.path == "/stream") {
        std::string token = get_auth_token(req);
        if (token.empty() || !is_token_valid(token)) {
            HttpResponse resp = {401, "Unauthorized", {{"Content-Type","application/json"}}, "{\"error\":\"Unauthorized\"}"};
            std::string resp_str = http_response_to_string(resp);
            send(connfd, resp_str.c_str(), resp_str.size(), 0);
            close(connfd); return;
        }
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n";
        oss << "Content-Type: multipart/x-mixed-replace;boundary=--myboundary\r\n";
        oss << "Connection: close\r\n\r\n";
        std::string hdr = oss.str();
        send(connfd, hdr.c_str(), hdr.size(), 0);
        write_mjpeg_stream(connfd);
        close(connfd);
        return;
    }

    // Find API handler
    auto it = http_routes.find(req.method);
    if (it != http_routes.end()) {
        auto jt = it->second.find(req.path);
        if (jt != it->second.end()) {
            HttpResponse resp = jt->second(req);
            std::string resp_str = http_response_to_string(resp);
            send(connfd, resp_str.c_str(), resp_str.size(), 0);
            close(connfd);
            return;
        }
    }
    // Default: 404
    HttpResponse resp = {404, "Not Found", {{"Content-Type","application/json"}}, "{\"error\":\"Not found\"}"};
    std::string resp_str = http_response_to_string(resp);
    send(connfd, resp_str.c_str(), resp_str.size(), 0);
    close(connfd);
}

void http_server_run() {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); exit(1); }
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(HTTP_HOST.c_str());
    addr.sin_port = htons(HTTP_PORT);
    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(listenfd, 8) < 0) { perror("listen"); exit(1); }
    std::cout << "[HTTP] Listening on " << HTTP_HOST << ":" << HTTP_PORT << std::endl;
    for (;;) {
        struct sockaddr_in cliaddr; socklen_t len = sizeof(cliaddr);
        int connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &len);
        if (connfd < 0) continue;
        std::thread th(handle_client, connfd);
        th.detach();
    }
}

int main(int argc, char* argv[]) {
    srand(time(0));
    load_env();
    register_handlers();
    http_server_run();
    return 0;
}