#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <iostream>
#include <cstdio>
#include <json/json.h>
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib,"ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

// Mocked Device SDK API for demonstration (replace with real SDK in production)
namespace DeviceSDK {
    struct Session {
        std::string token;
        bool loggedIn;
        Session(): loggedIn(false) {}
    };
    static Session deviceSession;
    static std::mutex sdkMutex;
    bool login(const std::string& user, const std::string& pass, std::string& token) {
        std::lock_guard<std::mutex> lock(sdkMutex);
        if (user=="admin" && pass=="12345") {
            token = "SESSION_" + std::to_string(std::time(nullptr));
            deviceSession.token = token;
            deviceSession.loggedIn = true;
            return true;
        }
        return false;
    }
    bool logout(const std::string& token) {
        std::lock_guard<std::mutex> lock(sdkMutex);
        if (deviceSession.token == token) {
            deviceSession.loggedIn = false;
            deviceSession.token = "";
            return true;
        }
        return false;
    }
    bool getStatus(Json::Value& status) {
        std::lock_guard<std::mutex> lock(sdkMutex);
        status["device"] = "DS_64XXHD_S";
        status["error_code"] = 0;
        status["alarm"] = false;
        status["network"] = "192.168.1.100";
        status["health"] = "OK";
        return true;
    }
    bool getConfig(const std::string& type, Json::Value& config) {
        std::lock_guard<std::mutex> lock(sdkMutex);
        config["type"] = type;
        if (type == "display") config["display_mode"] = "split_4";
        else if (type == "channel") config["channels"] = 16;
        else config["info"] = "default";
        return true;
    }
    bool setConfig(const std::string& type, const Json::Value& cfg) {
        std::lock_guard<std::mutex> lock(sdkMutex);
        return true;
    }
    bool decodeControl(const std::string& action, const std::string& mode) {
        std::lock_guard<std::mutex> lock(sdkMutex);
        return action == "start" || action == "stop";
    }
    bool reboot(const std::string& op) {
        std::lock_guard<std::mutex> lock(sdkMutex);
        return op == "reboot" || op == "shutdown";
    }
    bool upgrade(const Json::Value& params) {
        std::lock_guard<std::mutex> lock(sdkMutex);
        return params.isMember("firmware");
    }
}

// Simple HTTP Server Implementation
class HttpServer {
public:
    HttpServer(const std::string& host, int port)
        : host_(host), port_(port), running_(false) {}
    void start() {
        running_ = true;
        serverThread_ = std::thread([this]() { this->run(); });
    }
    void stop() {
        running_ = false;
#ifdef _WIN32
        closesocket(serverSock_);
        WSACleanup();
#else
        close(serverSock_);
#endif
        if (serverThread_.joinable()) serverThread_.join();
    }
    ~HttpServer() { stop(); }
private:
    std::string host_;
    int port_;
    volatile bool running_;
    int serverSock_;
    std::thread serverThread_;

    static void sendResponse(int clientSock, int code, const std::string& contentType, const std::string& body) {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << code << " "
            << (code==200?"OK":(code==201?"Created":(code==400?"Bad Request":(code==401?"Unauthorized":(code==404?"Not Found":"Error")))))
            << "\r\nContent-Type: " << contentType << "\r\nContent-Length: " << body.size()
            << "\r\nAccess-Control-Allow-Origin: *\r\n\r\n" << body;
#ifdef _WIN32
        send(clientSock, oss.str().c_str(), (int)oss.str().size(), 0);
        closesocket(clientSock);
#else
        send(clientSock, oss.str().c_str(), oss.str().size(), 0);
        close(clientSock);
#endif
    }
    static std::string urlDecode(const std::string& src) {
        std::string ret;
        char ch;
        int i, ii;
        for (i = 0; i < src.length(); ++i) {
            if (int(src[i]) == 37) {
                sscanf(src.substr(i + 1, 2).c_str(), "%x", &ii);
                ch = static_cast<char>(ii);
                ret += ch;
                i = i + 2;
            } else {
                ret += src[i];
            }
        }
        return ret;
    }
    static std::map<std::string, std::string> parseQuery(const std::string& query) {
        std::map<std::string, std::string> params;
        std::istringstream ss(query);
        std::string item;
        while (std::getline(ss, item, '&')) {
            auto eq = item.find('=');
            if (eq != std::string::npos) {
                params[item.substr(0, eq)] = urlDecode(item.substr(eq+1));
            }
        }
        return params;
    }
    static bool parseRequest(int clientSock, std::string& method, std::string& path, std::string& query,
                             std::map<std::string, std::string>& headers, std::string& body) {
        char buffer[4096];
        int received = 0;
        std::string request;
        while ((received = recv(clientSock, buffer, sizeof(buffer), 0)) > 0) {
            request.append(buffer, received);
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }
        if (request.empty()) return false;
        // Parse request line
        auto firstLineEnd = request.find("\r\n");
        if (firstLineEnd == std::string::npos) return false;
        auto firstLine = request.substr(0, firstLineEnd);
        std::istringstream fls(firstLine);
        fls >> method;
        std::string fullPath;
        fls >> fullPath;
        auto qm = fullPath.find('?');
        if (qm != std::string::npos) {
            path = fullPath.substr(0, qm);
            query = fullPath.substr(qm+1);
        } else {
            path = fullPath;
            query = "";
        }
        // Parse headers
        size_t pos = firstLineEnd+2;
        while (true) {
            auto lineEnd = request.find("\r\n", pos);
            if (lineEnd == std::string::npos || lineEnd == pos) break;
            auto line = request.substr(pos, lineEnd-pos);
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto key = line.substr(0, colon);
                auto value = line.substr(colon+1);
                while (!value.empty() && value.front()==' ') value.erase(0,1);
                headers[key] = value;
            }
            pos = lineEnd+2;
        }
        // Parse body
        auto bodyPos = request.find("\r\n\r\n");
        if (bodyPos != std::string::npos) {
            body = request.substr(bodyPos+4);
            // If Content-Length, read more if needed
            auto it = headers.find("Content-Length");
            if (it != headers.end()) {
                int contentLen = std::stoi(it->second);
                while ((int)body.size() < contentLen) {
                    received = recv(clientSock, buffer, sizeof(buffer), 0);
                    if (received <= 0) break;
                    body.append(buffer, received);
                }
            }
        }
        return true;
    }
    void handleRequest(int clientSock) {
        std::string method, path, query, body;
        std::map<std::string, std::string> headers;
        if (!parseRequest(clientSock, method, path, query, headers, body)) {
            sendResponse(clientSock, 400, "application/json", "{\"error\":\"Bad Request\"}");
            return;
        }
        std::map<std::string, std::string> qparams = parseQuery(query);
        // Routing
        if (path == "/login" && method == "POST") {
            Json::Value req;
            Json::Reader reader;
            if (!reader.parse(body, req) || !req.isMember("username") || !req.isMember("password")) {
                sendResponse(clientSock, 400, "application/json", "{\"error\":\"Missing credentials\"}");
                return;
            }
            std::string token;
            if (DeviceSDK::login(req["username"].asString(), req["password"].asString(), token)) {
                Json::Value resp;
                resp["session_token"] = token;
                sendResponse(clientSock, 200, "application/json", Json::FastWriter().write(resp));
            } else {
                sendResponse(clientSock, 401, "application/json", "{\"error\":\"Authentication failed\"}");
            }
        }
        else if (path == "/logout" && method == "POST") {
            Json::Value req;
            Json::Reader reader;
            if (!reader.parse(body, req) || !req.isMember("session_token")) {
                sendResponse(clientSock, 400, "application/json", "{\"error\":\"Missing session token\"}");
                return;
            }
            if (DeviceSDK::logout(req["session_token"].asString())) {
                sendResponse(clientSock, 200, "application/json", "{\"message\":\"Logged out\"}");
            } else {
                sendResponse(clientSock, 401, "application/json", "{\"error\":\"Invalid session token\"}");
            }
        }
        else if (path == "/status" && method == "GET") {
            Json::Value status;
            if (DeviceSDK::getStatus(status)) {
                sendResponse(clientSock, 200, "application/json", Json::FastWriter().write(status));
            } else {
                sendResponse(clientSock, 500, "application/json", "{\"error\":\"Unable to get status\"}");
            }
        }
        else if (path == "/config" && method == "GET") {
            std::string type = qparams.count("type")?qparams["type"]:"";
            Json::Value config;
            if (DeviceSDK::getConfig(type, config)) {
                sendResponse(clientSock, 200, "application/json", Json::FastWriter().write(config));
            } else {
                sendResponse(clientSock, 500, "application/json", "{\"error\":\"Unable to get config\"}");
            }
        }
        else if (path == "/config" && method == "PUT") {
            std::string type = qparams.count("type")?qparams["type"]:"";
            Json::Value req;
            Json::Reader reader;
            if (!reader.parse(body, req)) {
                sendResponse(clientSock, 400, "application/json", "{\"error\":\"Invalid JSON payload\"}");
                return;
            }
            if (DeviceSDK::setConfig(type, req)) {
                sendResponse(clientSock, 200, "application/json", "{\"message\":\"Config updated\"}");
            } else {
                sendResponse(clientSock, 500, "application/json", "{\"error\":\"Unable to update config\"}");
            }
        }
        else if (path == "/decode" && method == "POST") {
            Json::Value req;
            Json::Reader reader;
            if (!reader.parse(body, req) || !req.isMember("action") || !req.isMember("mode")) {
                sendResponse(clientSock, 400, "application/json", "{\"error\":\"Missing action or mode\"}");
                return;
            }
            if (DeviceSDK::decodeControl(req["action"].asString(), req["mode"].asString())) {
                sendResponse(clientSock, 200, "application/json", "{\"message\":\"Decoder command sent\"}");
            } else {
                sendResponse(clientSock, 500, "application/json", "{\"error\":\"Decoder control failed\"}");
            }
        }
        else if (path == "/reboot" && method == "POST") {
            Json::Value req;
            Json::Reader reader;
            if (!reader.parse(body, req) || !req.isMember("operation")) {
                sendResponse(clientSock, 400, "application/json", "{\"error\":\"Missing operation\"}");
                return;
            }
            if (DeviceSDK::reboot(req["operation"].asString())) {
                sendResponse(clientSock, 200, "application/json", "{\"message\":\"Device reboot/shutdown initiated\"}");
            } else {
                sendResponse(clientSock, 500, "application/json", "{\"error\":\"Reboot/Shutdown failed\"}");
            }
        }
        else if (path == "/upgrade" && method == "POST") {
            Json::Value req;
            Json::Reader reader;
            if (!reader.parse(body, req) || !req.isMember("firmware")) {
                sendResponse(clientSock, 400, "application/json", "{\"error\":\"Missing firmware reference\"}");
                return;
            }
            if (DeviceSDK::upgrade(req)) {
                sendResponse(clientSock, 200, "application/json", "{\"message\":\"Upgrade started\"}");
            } else {
                sendResponse(clientSock, 500, "application/json", "{\"error\":\"Upgrade failed\"}");
            }
        }
        else {
            sendResponse(clientSock, 404, "application/json", "{\"error\":\"Not found\"}");
        }
    }
    void run() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2),&wsa);
#endif
        serverSock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSock_ < 0) return;
        int opt = 1;
        setsockopt(serverSock_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(serverSock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) return;
        listen(serverSock_, 8);
        while (running_) {
            sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientSock = accept(serverSock_, (struct sockaddr*)&clientAddr, &clientLen);
            if (clientSock < 0) continue;
            std::thread(&HttpServer::handleRequest, this, clientSock).detach();
        }
#ifdef _WIN32
        closesocket(serverSock_);
        WSACleanup();
#else
        close(serverSock_);
#endif
    }
};

// Entry
int main() {
    // Environment variables
    const char* env_host = std::getenv("HTTP_SERVER_HOST");
    const char* env_port = std::getenv("HTTP_SERVER_PORT");
    std::string host = env_host ? env_host : "0.0.0.0";
    int port = env_port ? std::atoi(env_port) : 8080;

    HttpServer server(host, port);
    server.start();

    std::cout << "HTTP server running on " << host << ":" << port << std::endl;
    while (1) std::this_thread::sleep_for(std::chrono::seconds(1000));
    return 0;
}