#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <json/json.h> // Requires jsoncpp library for JSON handling
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

// --- Environment Variable Helper ---
std::string getenv_default(const std::string& key, const std::string& def) {
    const char* val = std::getenv(key.c_str());
    return val ? val : def;
}

// --- Mock SDK/Device State (Replace with actual SDK integration) ---
struct DeviceSession {
    std::string token;
    std::string username;
    std::time_t expire;
};
std::mutex sessions_mutex;
std::unordered_map<std::string, DeviceSession> sessions;

bool sdk_initialized = false;
bool device_logged_in = false;
std::string last_device_user = "";
std::string current_session_token = "";

Json::Value mock_device_config() {
    Json::Value config;
    config["display_channel"] = "4x4";
    config["scene"]["current"] = "Default";
    config["abilities"]["max_windows"] = 16;
    config["abilities"]["supports_playback"] = true;
    return config;
}
Json::Value mock_device_status() {
    Json::Value status;
    status["device"] = "ok";
    status["channels"][0]["status"] = "decoding";
    status["channels"][0]["playback"] = "stopped";
    status["alarm"] = false;
    status["stream"]["bitrate"] = 4096;
    return status;
}

// --- HTTP Utilities ---
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
        default: return "OK";
    }
}

void send_http_response(SOCKET client, int code, const std::string& body, const std::string& content_type = "application/json", const std::map<std::string, std::string>& headers = {}) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " " << http_status_message(code) << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    for (const auto& h : headers) {
        oss << h.first << ": " << h.second << "\r\n";
    }
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    std::string resp = oss.str();
    send(client, resp.c_str(), (int)resp.size(), 0);
}

// --- Token/session helpers ---
std::string generate_token() {
    static const char alphanum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string token;
    for (int i = 0; i < 32; ++i)
        token += alphanum[rand() % (sizeof(alphanum) - 1)];
    return token;
}

bool has_valid_session(const std::string& token) {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    auto it = sessions.find(token);
    if (it != sessions.end()) {
        if (std::time(nullptr) < it->second.expire)
            return true;
        sessions.erase(it);
    }
    return false;
}

// --- HTTP Request Parsing ---
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string http_version;
    std::map<std::string, std::string> headers;
    std::string body;
};
HttpRequest parse_http_request(const std::string& req) {
    HttpRequest http;
    std::istringstream iss(req);
    std::string line;
    getline(iss, line);
    std::istringstream line_ss(line);
    line_ss >> http.method;
    std::string full_path;
    line_ss >> full_path;
    size_t q = full_path.find('?');
    if (q != std::string::npos) {
        http.path = full_path.substr(0, q);
        http.query = full_path.substr(q + 1);
    } else {
        http.path = full_path;
    }
    line_ss >> http.http_version;
    while (getline(iss, line) && line != "\r") {
        size_t pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            http.headers[key] = val;
        }
    }
    if (http.headers.count("content-length")) {
        int len = atoi(http.headers["content-length"].c_str());
        std::string rest;
        std::getline(iss, rest, '\0');
        if (rest.size() < len) http.body += rest;
        else http.body = rest.substr(0, len);
    }
    return http;
}

// --- Query Params ---
std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> params;
    std::istringstream iss(query);
    std::string p;
    while (getline(iss, p, '&')) {
        size_t eq = p.find('=');
        if (eq != std::string::npos)
            params[p.substr(0, eq)] = p.substr(eq + 1);
    }
    return params;
}

// --- Endpoint Handlers ---
void handle_sdk(SOCKET client, const HttpRequest& req) {
    Json::Value resp;
    if (req.method != "POST") {
        resp["error"] = "Method Not Allowed";
        send_http_response(client, 405, resp.toStyledString());
        return;
    }
    Json::Reader reader;
    Json::Value body;
    if (!reader.parse(req.body, body) || !body.isMember("action")) {
        resp["error"] = "Invalid payload";
        send_http_response(client, 400, resp.toStyledString());
        return;
    }
    const std::string action = body["action"].asString();
    if (action == "init") {
        sdk_initialized = true;
        resp["status"] = "SDK initialized";
        send_http_response(client, 200, resp.toStyledString());
    } else if (action == "cleanup") {
        sdk_initialized = false;
        resp["status"] = "SDK cleaned up";
        send_http_response(client, 200, resp.toStyledString());
    } else {
        resp["error"] = "Unknown action";
        send_http_response(client, 400, resp.toStyledString());
    }
}

void handle_session_post(SOCKET client, const HttpRequest& req) {
    Json::Value resp;
    Json::Reader reader;
    Json::Value body;
    if (!reader.parse(req.body, body) || !body.isMember("username") || !body.isMember("password")) {
        resp["error"] = "Missing credentials";
        send_http_response(client, 400, resp.toStyledString());
        return;
    }
    std::string username = body["username"].asString();
    std::string password = body["password"].asString();
    // Simulate login
    if (username.empty() || password.empty()) {
        resp["error"] = "Invalid credentials";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    device_logged_in = true;
    last_device_user = username;
    std::string token = generate_token();
    current_session_token = token;
    DeviceSession session;
    session.token = token;
    session.username = username;
    session.expire = std::time(nullptr) + 3600; // 1 hour
    {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        sessions[token] = session;
    }
    resp["session_token"] = token;
    send_http_response(client, 200, resp.toStyledString());
}

void handle_session_delete(SOCKET client, const HttpRequest& req) {
    Json::Value resp;
    std::string token = "";
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) token = it->second;
    if (token.empty()) {
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        sessions.erase(token);
    }
    device_logged_in = false;
    resp["status"] = "Session terminated";
    send_http_response(client, 200, resp.toStyledString());
}

void handle_config_get(SOCKET client, const HttpRequest& req) {
    std::string token = "";
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) token = it->second;
    if (!has_valid_session(token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    auto params = parse_query(req.query);
    Json::Value config = mock_device_config();
    if (params.count("section")) {
        Json::Value filtered;
        if (config.isMember(params["section"]))
            filtered[params["section"]] = config[params["section"]];
        else
            filtered["error"] = "No such section";
        send_http_response(client, 200, filtered.toStyledString());
    } else {
        send_http_response(client, 200, config.toStyledString());
    }
}

void handle_config_put(SOCKET client, const HttpRequest& req) {
    std::string token = "";
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) token = it->second;
    if (!has_valid_session(token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    Json::Reader reader;
    Json::Value body;
    if (!reader.parse(req.body, body)) {
        Json::Value resp;
        resp["error"] = "Invalid JSON";
        send_http_response(client, 400, resp.toStyledString());
        return;
    }
    // Simulate config update
    Json::Value resp;
    resp["status"] = "Configuration updated";
    resp["applied"] = body;
    send_http_response(client, 200, resp.toStyledString());
}

void handle_status_get(SOCKET client, const HttpRequest& req) {
    std::string token = "";
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) token = it->second;
    if (!has_valid_session(token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    auto params = parse_query(req.query);
    Json::Value status = mock_device_status();
    if (params.count("type")) {
        if (status.isMember(params["type"])) {
            Json::Value filtered;
            filtered[params["type"]] = status[params["type"]];
            send_http_response(client, 200, filtered.toStyledString());
            return;
        }
    }
    send_http_response(client, 200, status.toStyledString());
}

void handle_reboot_post(SOCKET client, const HttpRequest& req) {
    std::string token = "";
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) token = it->second;
    if (!has_valid_session(token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    Json::Value resp;
    resp["status"] = "Device rebooting";
    send_http_response(client, 200, resp.toStyledString());
}

void handle_playback_post(SOCKET client, const HttpRequest& req) {
    std::string token = "";
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) token = it->second;
    if (!has_valid_session(token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    Json::Reader reader;
    Json::Value body;
    if (!reader.parse(req.body, body) || !body.isMember("action")) {
        Json::Value resp;
        resp["error"] = "Invalid payload";
        send_http_response(client, 400, resp.toStyledString());
        return;
    }
    std::string action = body["action"].asString();
    Json::Value resp;
    if (action == "start") {
        resp["status"] = "Playback started";
    } else if (action == "stop") {
        resp["status"] = "Playback stopped";
    } else {
        resp["error"] = "Unknown action";
        send_http_response(client, 400, resp.toStyledString());
        return;
    }
    send_http_response(client, 200, resp.toStyledString());
}

void handle_decode_post(SOCKET client, const HttpRequest& req) {
    std::string token = "";
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) token = it->second;
    if (!has_valid_session(token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    Json::Reader reader;
    Json::Value body;
    if (!reader.parse(req.body, body) || !body.isMember("action")) {
        Json::Value resp;
        resp["error"] = "Invalid payload";
        send_http_response(client, 400, resp.toStyledString());
        return;
    }
    std::string action = body["action"].asString();
    Json::Value resp;
    if (action == "start") {
        resp["status"] = "Dynamic decode started";
    } else if (action == "stop") {
        resp["status"] = "Dynamic decode stopped";
    } else {
        resp["error"] = "Unknown action";
        send_http_response(client, 400, resp.toStyledString());
        return;
    }
    send_http_response(client, 200, resp.toStyledString());
}

void handle_upgrade_post(SOCKET client, const HttpRequest& req) {
    std::string token = "";
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) token = it->second;
    if (!has_valid_session(token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, resp.toStyledString());
        return;
    }
    Json::Value resp;
    resp["status"] = "Upgrade started";
    send_http_response(client, 200, resp.toStyledString());
}

// --- HTTP Router ---
void route_request(SOCKET client, const HttpRequest& req) {
    // endpoint: /sdk
    if (req.path == "/sdk") {
        handle_sdk(client, req);
        return;
    }
    // endpoint: /session
    if (req.path == "/session") {
        if (req.method == "POST") handle_session_post(client, req);
        else if (req.method == "DELETE") handle_session_delete(client, req);
        else {
            Json::Value resp; resp["error"] = "Method Not Allowed";
            send_http_response(client, 405, resp.toStyledString());
        }
        return;
    }
    // endpoint: /config
    if (req.path == "/config") {
        if (req.method == "GET") handle_config_get(client, req);
        else if (req.method == "PUT") handle_config_put(client, req);
        else {
            Json::Value resp; resp["error"] = "Method Not Allowed";
            send_http_response(client, 405, resp.toStyledString());
        }
        return;
    }
    // endpoint: /status
    if (req.path == "/status" && req.method == "GET") {
        handle_status_get(client, req);
        return;
    }
    // endpoint: /reboot
    if (req.path == "/reboot" && req.method == "POST") {
        handle_reboot_post(client, req);
        return;
    }
    // endpoint: /playback
    if (req.path == "/playback" && req.method == "POST") {
        handle_playback_post(client, req);
        return;
    }
    // endpoint: /decode
    if (req.path == "/decode" && req.method == "POST") {
        handle_decode_post(client, req);
        return;
    }
    // endpoint: /upgrade
    if (req.path == "/upgrade" && req.method == "POST") {
        handle_upgrade_post(client, req);
        return;
    }
    // unknown
    Json::Value resp;
    resp["error"] = "Not Found";
    send_http_response(client, 404, resp.toStyledString());
}

// --- HTTP Server ---
void handle_client(SOCKET client) {
    char buffer[8192];
    int received = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
        return;
    }
    buffer[received] = 0;
    std::string request_string(buffer);
    HttpRequest req = parse_http_request(request_string);
    route_request(client, req);
#ifdef _WIN32
    closesocket(client);
#else
    close(client);
#endif
}

void start_http_server(const std::string& host, int port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        std::cerr << "Failed to create socket\n";
        return;
    }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    addr.sin_port = htons(port);
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind\n";
#ifdef _WIN32
        closesocket(server);
        WSACleanup();
#else
        close(server);
#endif
        return;
    }
    listen(server, 10);
    std::cout << "HTTP server listening on " << host << ":" << port << std::endl;
    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        SOCKET client = accept(server, (sockaddr*)&client_addr, &client_len);
        if (client == INVALID_SOCKET) continue;
        std::thread(handle_client, client).detach();
    }
#ifdef _WIN32
    closesocket(server);
    WSACleanup();
#else
    close(server);
#endif
}

// -- MAIN --
int main() {
    srand((unsigned int)time(nullptr));
    std::string host = getenv_default("DRIVER_HTTP_HOST", "0.0.0.0");
    int port = std::stoi(getenv_default("DRIVER_HTTP_PORT", "8080"));
    start_http_server(host, port);
    return 0;
}