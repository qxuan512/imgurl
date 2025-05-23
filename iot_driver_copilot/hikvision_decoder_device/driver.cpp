#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <map>
#include <sstream>
#include <vector>
#include <functional>
#include <condition_variable>

#ifdef _WIN32
#include <winsock2.h>
typedef int socklen_t;
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

// ---- Simple HTTP Server Helpers ----

#define HTTP_OK "HTTP/1.1 200 OK\r\n"
#define HTTP_UNAUTHORIZED "HTTP/1.1 401 Unauthorized\r\n"
#define HTTP_BAD_REQUEST "HTTP/1.1 400 Bad Request\r\n"
#define HTTP_NOT_FOUND "HTTP/1.1 404 Not Found\r\n"
#define HTTP_SERVER_ERROR "HTTP/1.1 500 Internal Server Error\r\n"

#define MAX_REQUEST_SIZE 8192

// ---- Device SDK Simulation & Session ----

struct DeviceSession {
    std::string username;
    std::string token;
    bool logged_in = false;
    // In a real driver, this would hold SDK handles/resources
};

std::mutex session_mutex;
DeviceSession g_session;

// Simulate token generation
std::string generate_token(const std::string& user) {
    return "session_" + user + "_token_" + std::to_string(rand() % 100000);
}

// Simulated Hikvision device login
bool hikvision_login(const std::string& ip, int port, const std::string& user, const std::string& pass, std::string& token) {
    // TODO: Replace with actual SDK logic
    if (user == "admin" && pass == "12345") {
        token = generate_token(user);
        return true;
    }
    return false;
}

// Simulated capture (returns a fake JPEG binary)
std::vector<uint8_t> hikvision_capture(const std::string& token) {
    // TODO: Replace with actual SDK binary stream
    // For demonstration, return a small static JPEG header
    static const uint8_t dummy_jpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46,
        0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x60,
        0x00, 0x60, 0x00, 0x00, 0xFF, 0xD9
    };
    return std::vector<uint8_t>(dummy_jpeg, dummy_jpeg + sizeof(dummy_jpeg));
}

// ---- Environment Variables ----

std::string env(const char* name, const char* def = "") {
    const char* val = std::getenv(name);
    if (!val) return def;
    return val;
}

int env_int(const char* name, int def) {
    const char* val = std::getenv(name);
    if (!val) return def;
    return atoi(val);
}

// ---- HTTP Parsing ----

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::map<std::string, std::string> headers;
};

bool parse_http_request(const char* buffer, size_t len, HttpRequest& req) {
    std::istringstream ss(std::string(buffer, len));
    std::string line;
    if (!std::getline(ss, line)) return false;
    std::istringstream lss(line);
    lss >> req.method >> req.path;
    // Headers
    while (std::getline(ss, line) && line != "\r") {
        if (line.empty() || line == "\n") break;
        size_t sep = line.find(':');
        if (sep != std::string::npos) {
            std::string key = line.substr(0, sep);
            std::string val = line.substr(sep+1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0,1);
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
            req.headers[key] = val;
        }
    }
    // Body
    if (req.headers.count("Content-Length")) {
        int clen = atoi(req.headers["Content-Length"].c_str());
        if (clen > 0) {
            req.body.resize(clen);
            ss.read(&req.body[0], clen);
        }
    }
    return true;
}

// ---- HTTP Server Core ----

void send_response(int client, const std::string& header, const std::string& content_type, const std::string& body) {
    std::ostringstream resp;
    resp << header;
    resp << "Content-Type: " << content_type << "\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    resp << "Connection: close\r\n\r\n";
    resp << body;
    std::string s = resp.str();
#ifdef _WIN32
    send(client, s.c_str(), (int)s.size(), 0);
#else
    ::send(client, s.c_str(), s.size(), 0);
#endif
}

void send_response_binary(int client, const std::string& header, const std::string& content_type, const std::vector<uint8_t>& data) {
    std::ostringstream resp;
    resp << header;
    resp << "Content-Type: " << content_type << "\r\n";
    resp << "Content-Length: " << data.size() << "\r\n";
    resp << "Connection: close\r\n\r\n";
    std::string head = resp.str();
#ifdef _WIN32
    send(client, head.c_str(), (int)head.size(), 0);
    send(client, (const char*)data.data(), (int)data.size(), 0);
#else
    ::send(client, head.c_str(), head.size(), 0);
    ::send(client, data.data(), data.size(), 0);
#endif
}

// ---- API Handlers ----

void handle_login(const HttpRequest& req, int client) {
    // Expects JSON {"username": "...", "password": "..."}
    std::string user, pass;
    size_t up = req.body.find("\"username\"");
    size_t pp = req.body.find("\"password\"");
    if (up != std::string::npos) {
        size_t q1 = req.body.find('"', up+10);
        size_t q2 = req.body.find('"', q1+1);
        user = req.body.substr(q1+1, q2-q1-1);
    }
    if (pp != std::string::npos) {
        size_t q1 = req.body.find('"', pp+10);
        size_t q2 = req.body.find('"', q1+1);
        pass = req.body.substr(q1+1, q2-q1-1);
    }
    if (user.empty() || pass.empty()) {
        send_response(client, HTTP_BAD_REQUEST, "application/json", "{\"error\":\"Missing username or password\"}");
        return;
    }
    std::string device_ip = env("DEVICE_IP", "127.0.0.1");
    int device_port = env_int("DEVICE_PORT", 8000);

    std::string token;
    if (hikvision_login(device_ip, device_port, user, pass, token)) {
        std::lock_guard<std::mutex> lk(session_mutex);
        g_session.username = user;
        g_session.token = token;
        g_session.logged_in = true;
        send_response(client, HTTP_OK, "application/json", "{\"token\":\"" + token + "\"}");
    } else {
        send_response(client, HTTP_UNAUTHORIZED, "application/json", "{\"error\":\"Invalid credentials\"}");
    }
}

void handle_capture(const HttpRequest& req, int client) {
    std::string token;
    auto it = req.headers.find("Authorization");
    if (it != req.headers.end()) {
        const std::string& val = it->second;
        if (val.find("Bearer ") == 0)
            token = val.substr(7);
        else
            token = val;
    } else {
        // Try from JSON body
        size_t tk = req.body.find("\"token\"");
        if (tk != std::string::npos) {
            size_t q1 = req.body.find('"', tk+7);
            size_t q2 = req.body.find('"', q1+1);
            token = req.body.substr(q1+1, q2-q1-1);
        }
    }

    {
        std::lock_guard<std::mutex> lk(session_mutex);
        if (!g_session.logged_in || token != g_session.token) {
            send_response(client, HTTP_UNAUTHORIZED, "application/json", "{\"error\":\"Not authorized\"}");
            return;
        }
    }
    std::vector<uint8_t> jpeg = hikvision_capture(token);
    send_response_binary(client, HTTP_OK, "image/jpeg", jpeg);
}

// ---- Routing ----

void handle_request(int client, const HttpRequest& req) {
    if (req.method == "POST" && req.path == "/login") {
        handle_login(req, client);
    } else if (req.method == "POST" && req.path == "/capture") {
        handle_capture(req, client);
    } else {
        send_response(client, HTTP_NOT_FOUND, "application/json", "{\"error\":\"Not found\"}");
    }
}

void client_thread(int client) {
    char buffer[MAX_REQUEST_SIZE];
#ifdef _WIN32
    int r = recv(client, buffer, sizeof(buffer), 0);
#else
    int r = ::recv(client, buffer, sizeof(buffer), 0);
#endif
    if (r <= 0) {
#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
        return;
    }
    HttpRequest req;
    if (!parse_http_request(buffer, r, req)) {
        send_response(client, HTTP_BAD_REQUEST, "application/json", "{\"error\":\"Malformed request\"}");
#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
        return;
    }
    handle_request(client, req);
#ifdef _WIN32
    closesocket(client);
#else
    close(client);
#endif
}

// ---- Main Server ----

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
    std::string listen_host = env("SERVER_HOST", "0.0.0.0");
    int listen_port = env_int("SERVER_PORT", 8080);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    addr.sin_addr.s_addr = inet_addr(listen_host.c_str());

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        return 1;
    }
    if (listen(s, 8) < 0) {
        std::cerr << "Listen failed\n";
        return 1;
    }
    std::cout << "HTTP server listening on " << listen_host << ":" << listen_port << std::endl;
    while (true) {
        sockaddr_in cli_addr;
        socklen_t clilen = sizeof(cli_addr);
        int cli = accept(s, (sockaddr*)&cli_addr, &clilen);
        if (cli < 0) continue;
        std::thread t(client_thread, cli);
        t.detach();
    }
#ifdef _WIN32
    closesocket(s);
    WSACleanup();
#else
    close(s);
#endif
    return 0;
}