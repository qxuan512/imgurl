#include <iostream>
#include <cstdlib>
#include <map>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <json/json.h>
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

// HCNetSDK includes would go here, e.g. #include "HCNetSDK.h"
// For this example, SDK calls are stubbed/mocked.

#define BUFFER_SIZE 8192

std::mutex sdk_mutex;

// Environment variable helpers
std::string get_env(const char* name, const char* defval = "") {
    const char* val = std::getenv(name);
    if (!val) return std::string(defval);
    return std::string(val);
}

// --- Mock SDK Layer --- //
struct SDKSession {
    bool logged_in;
    std::string username;
    std::string password;
    std::string ip;
    int port;
    int user_id;
    std::string sdk_version;
};

SDKSession g_sdk_session = {false, "", "", "", 0, -1, "V5.3.0"};

bool SDK_Initialize() {
    // Actual implementation: NET_DVR_Init();
    return true;
}

void SDK_Cleanup() {
    // Actual implementation: NET_DVR_Cleanup();
}

bool SDK_Login(const std::string& ip, int port, const std::string& username, const std::string& password, int& user_id) {
    // Actual implementation: NET_DVR_Login_V40, etc.
    static int uid = 100;
    user_id = uid++;
    g_sdk_session = {true, username, password, ip, port, user_id, "V5.3.0"};
    return true;
}

bool SDK_Logout(int user_id) {
    g_sdk_session.logged_in = false;
    g_sdk_session.user_id = -1;
    return true;
}

Json::Value SDK_GetStatus() {
    Json::Value root;
    root["device"] = "DS-64XXHD_S";
    root["sdk_version"] = g_sdk_session.sdk_version;
    root["user_id"] = g_sdk_session.user_id;
    root["decoder_status"] = "running";
    root["channel_status"] = "ok";
    root["alarm_status"] = "none";
    root["playback_progress"] = 75;
    root["error_code"] = 0;
    return root;
}

bool SDK_ControlDecoder(const Json::Value& cmd, Json::Value& resp) {
    resp["result"] = "OK";
    resp["cmd"] = cmd;
    return true;
}

bool SDK_SetDisplayConfig(const Json::Value& cfg, Json::Value& resp) {
    resp["result"] = "OK";
    resp["config"] = cfg;
    return true;
}

bool SDK_ControlPlayback(const Json::Value& cmd, Json::Value& resp) {
    resp["result"] = "OK";
    resp["cmd"] = cmd;
    return true;
}

bool SDK_Reboot(Json::Value& resp) {
    resp["result"] = "rebooting";
    return true;
}

// --- HTTP Server Layer --- //
class HttpServer {
public:
    HttpServer(const std::string& host, int port) : host_(host), port_(port), running_(false) {}

    void start() {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
        running_ = true;
        server_thread_ = std::thread(&HttpServer::run, this);
    }
    void stop() {
        running_ = false;
#ifdef _WIN32
        closesocket(server_fd_);
        WSACleanup();
#else
        close(server_fd_);
#endif
        if (server_thread_.joinable()) server_thread_.join();
    }

private:
    std::string host_;
    int port_;
    int server_fd_;
    bool running_;
    std::thread server_thread_;

    void run() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return;
        int opt=1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr(host_.c_str());
        address.sin_port = htons(port_);
        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) return;
        listen(server_fd_, 8);
        while (running_) {
            sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &addrlen);
            if (client_fd < 0) continue;
            std::thread(&HttpServer::handle_client, this, client_fd).detach();
        }
    }

    void handle_client(int client_fd) {
        char buffer[BUFFER_SIZE+1];
        int len = recv(client_fd, buffer, BUFFER_SIZE, 0);
        if (len <= 0) {
#ifdef _WIN32
            closesocket(client_fd);
#else
            close(client_fd);
#endif
            return;
        }
        buffer[len] = 0;
        std::string req(buffer, len);

        std::string method, path, version, body;
        std::map<std::string, std::string> headers;
        parse_http_request(req, method, path, version, headers, body);
        std::string response;

        // Route
        if (method == "GET" && path == "/status") {
            response = handle_get_status();
        } else if (method == "POST" && path == "/auth/login") {
            response = handle_post_login(body);
        } else if (method == "POST" && path == "/auth/logout") {
            response = handle_post_logout();
        } else if (method == "POST" && path == "/control/decoder") {
            response = handle_post_control_decoder(body);
        } else if (method == "PUT" && path == "/config/display") {
            response = handle_put_config_display(body);
        } else if (method == "POST" && path == "/control/playback") {
            response = handle_post_control_playback(body);
        } else if (method == "POST" && path == "/sys/reboot") {
            response = handle_post_sys_reboot();
        } else {
            response = build_http_response(404, "application/json", "{\"error\":\"not found\"}");
        }

        send(client_fd, response.c_str(), response.size(), 0);
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
    }

    void parse_http_request(const std::string& req, std::string& method, std::string& path, std::string& version,
                            std::map<std::string, std::string>& headers, std::string& body) {
        size_t pos = 0, lpos = 0;
        pos = req.find("\r\n");
        std::string reqline = req.substr(0, pos);
        std::istringstream rl(reqline);
        rl >> method >> path >> version;
        lpos = pos+2;
        while (true) {
            pos = req.find("\r\n", lpos);
            if (pos == std::string::npos) break;
            std::string line = req.substr(lpos, pos-lpos);
            lpos = pos+2;
            if (line.empty()) break;
            size_t sc = line.find(":");
            if (sc != std::string::npos) {
                std::string key = line.substr(0, sc);
                std::string val = line.substr(sc+1);
                while (!val.empty() && val[0]==' ') val = val.substr(1);
                headers[key] = val;
            }
        }
        // Body
        body = req.substr(lpos);
    }

    // --- Endpoint Handlers --- //
    std::string handle_get_status() {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (!g_sdk_session.logged_in) {
            return build_http_response(401, "application/json", "{\"error\":\"unauthorized\"}");
        }
        Json::Value status = SDK_GetStatus();
        Json::FastWriter writer;
        return build_http_response(200, "application/json", writer.write(status));
    }

    std::string handle_post_login(const std::string& body) {
        Json::Value req, resp;
        Json::Reader reader;
        if (!reader.parse(body, req)) {
            return build_http_response(400, "application/json", "{\"error\":\"invalid json\"}");
        }
        std::string ip = req.get("ip", get_env("DEVICE_IP")).asString();
        int port = req.get("port", atoi(get_env("DEVICE_PORT","8000").c_str())).asInt();
        std::string username = req.get("username", get_env("DEVICE_USER")).asString();
        std::string password = req.get("password", get_env("DEVICE_PASS")).asString();
        if (ip.empty() || username.empty() || password.empty()) {
            return build_http_response(400, "application/json", "{\"error\":\"missing credentials\"}");
        }
        int user_id = -1;
        {
            std::lock_guard<std::mutex> lock(sdk_mutex);
            if (!SDK_Initialize()) {
                return build_http_response(500, "application/json", "{\"error\":\"sdk init failed\"}");
            }
            if (!SDK_Login(ip, port, username, password, user_id)) {
                SDK_Cleanup();
                return build_http_response(401, "application/json", "{\"error\":\"login failed\"}");
            }
        }
        resp["user_id"] = user_id;
        resp["message"] = "login successful";
        Json::FastWriter writer;
        return build_http_response(200, "application/json", writer.write(resp));
    }

    std::string handle_post_logout() {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (!g_sdk_session.logged_in) {
            return build_http_response(401, "application/json", "{\"error\":\"not logged in\"}");
        }
        SDK_Logout(g_sdk_session.user_id);
        SDK_Cleanup();
        Json::Value resp;
        resp["message"] = "logout successful";
        Json::FastWriter writer;
        return build_http_response(200, "application/json", writer.write(resp));
    }

    std::string handle_post_control_decoder(const std::string& body) {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (!g_sdk_session.logged_in) {
            return build_http_response(401, "application/json", "{\"error\":\"unauthorized\"}");
        }
        Json::Value req, resp;
        Json::Reader reader;
        if (!reader.parse(body, req)) {
            return build_http_response(400, "application/json", "{\"error\":\"invalid json\"}");
        }
        if (!SDK_ControlDecoder(req, resp)) {
            return build_http_response(500, "application/json", "{\"error\":\"decoder control failed\"}");
        }
        Json::FastWriter writer;
        return build_http_response(200, "application/json", writer.write(resp));
    }

    std::string handle_put_config_display(const std::string& body) {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (!g_sdk_session.logged_in) {
            return build_http_response(401, "application/json", "{\"error\":\"unauthorized\"}");
        }
        Json::Value req, resp;
        Json::Reader reader;
        if (!reader.parse(body, req)) {
            return build_http_response(400, "application/json", "{\"error\":\"invalid json\"}");
        }
        if (!SDK_SetDisplayConfig(req, resp)) {
            return build_http_response(500, "application/json", "{\"error\":\"set display config failed\"}");
        }
        Json::FastWriter writer;
        return build_http_response(200, "application/json", writer.write(resp));
    }

    std::string handle_post_control_playback(const std::string& body) {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (!g_sdk_session.logged_in) {
            return build_http_response(401, "application/json", "{\"error\":\"unauthorized\"}");
        }
        Json::Value req, resp;
        Json::Reader reader;
        if (!reader.parse(body, req)) {
            return build_http_response(400, "application/json", "{\"error\":\"invalid json\"}");
        }
        if (!SDK_ControlPlayback(req, resp)) {
            return build_http_response(500, "application/json", "{\"error\":\"playback control failed\"}");
        }
        Json::FastWriter writer;
        return build_http_response(200, "application/json", writer.write(resp));
    }

    std::string handle_post_sys_reboot() {
        std::lock_guard<std::mutex> lock(sdk_mutex);
        if (!g_sdk_session.logged_in) {
            return build_http_response(401, "application/json", "{\"error\":\"unauthorized\"}");
        }
        Json::Value resp;
        if (!SDK_Reboot(resp)) {
            return build_http_response(500, "application/json", "{\"error\":\"reboot failed\"}");
        }
        Json::FastWriter writer;
        return build_http_response(200, "application/json", writer.write(resp));
    }

    std::string build_http_response(int status, const std::string& content_type, const std::string& body) {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << " ";
        switch (status) {
            case 200: oss << "OK"; break;
            case 400: oss << "Bad Request"; break;
            case 401: oss << "Unauthorized"; break;
            case 404: oss << "Not Found"; break;
            case 500: oss << "Internal Server Error"; break;
            default: oss << "Unknown";
        }
        oss << "\r\n";
        oss << "Content-Type: " << content_type << "\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Connection: close\r\n\r\n";
        oss << body;
        return oss.str();
    }
};

// --- Main Entrypoint --- //
int main() {
    std::string host = get_env("SERVER_HOST", "0.0.0.0");
    int port = atoi(get_env("SERVER_PORT", "8080").c_str());
    HttpServer server(host, port);
    server.start();
    std::cout << "HTTP Driver for Hikvision Decoder running on " << host << ":" << port << std::endl;
    std::cout << "Press Ctrl+C to exit." << std::endl;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    server.stop();
    return 0;
}