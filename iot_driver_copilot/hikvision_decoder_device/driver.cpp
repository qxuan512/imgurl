#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#include <cstring>
#include <map>
#include <mutex>
#include <condition_variable>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 8192
#define MAX_CONNECTIONS 10

// Simple HTTP Response Helpers
std::string http_200(const std::string& content, const std::string& content_type = "application/json") {
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << content.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << content;
    return oss.str();
}

std::string http_401(const std::string& msg = "Unauthorized") {
    std::ostringstream oss;
    oss << "HTTP/1.1 401 Unauthorized\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << msg.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << msg;
    return oss.str();
}

std::string http_404() {
    std::string msg = "404 Not Found";
    std::ostringstream oss;
    oss << "HTTP/1.1 404 Not Found\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << msg.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << msg;
    return oss.str();
}

std::string http_405() {
    std::string msg = "405 Method Not Allowed";
    std::ostringstream oss;
    oss << "HTTP/1.1 405 Method Not Allowed\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << msg.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << msg;
    return oss.str();
}

std::string http_500(const std::string &msg = "Internal Server Error") {
    std::ostringstream oss;
    oss << "HTTP/1.1 500 Internal Server Error\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << msg.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << msg;
    return oss.str();
}

// Session store for authentication
struct SessionStore {
    std::mutex mutex;
    std::string session_token;
    bool valid = false;

    void set(const std::string& token) {
        std::lock_guard<std::mutex> lock(mutex);
        session_token = token;
        valid = true;
    }
    bool check(const std::string& token) {
        std::lock_guard<std::mutex> lock(mutex);
        return valid && session_token == token;
    }
    void invalidate() {
        std::lock_guard<std::mutex> lock(mutex);
        valid = false;
        session_token.clear();
    }
} session_store;

// Util: parse HTTP headers
std::map<std::string, std::string> parse_headers(const std::string& headers) {
    std::map<std::string, std::string> result;
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        size_t pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            // Trim
            key.erase(key.find_last_not_of(" \r\n")+1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r\n")+1);
            result[key] = value;
        }
    }
    return result;
}

// Util: parse POST body key=val&key2=val2
std::map<std::string, std::string> parse_urlencoded(const std::string& body) {
    std::map<std::string, std::string> result;
    std::istringstream iss(body);
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        size_t pos = pair.find('=');
        if (pos != std::string::npos) {
            std::string key = pair.substr(0, pos);
            std::string value = pair.substr(pos + 1);
            result[key] = value;
        }
    }
    return result;
}

// -- Hikvision Device SDK Mockup/Simulation --
// In practice, you'd use the vendor SDK headers and link to the library. Here, we simulate responses.

std::string mock_device_ip;
std::string mock_device_user;
std::string mock_device_pass;

bool hikvision_login(const std::string& user, const std::string& pass) {
    // Simulate login
    return user == mock_device_user && pass == mock_device_pass;
}

std::string hikvision_capture_jpeg() {
    // Simulate capturing a JPEG image from the decoder device (should be replaced with real SDK call)
    // For demo, return a fixed JPEG header + dummy data
    static const unsigned char jpeg_header[] = {
        0xFF, 0xD8, // SOI
        0xFF, 0xE0, // APP0
        0x00, 0x10, // length
        'J', 'F', 'I', 'F', 0x00
    };
    std::string jpeg((const char*)jpeg_header, sizeof(jpeg_header));
    jpeg += std::string(1024, '\xAA'); // Dummy image payload
    jpeg += std::string("\xFF\xD9", 2); // EOI
    return jpeg;
}

// -- HTTP Server Logic --

struct Request {
    std::string method;
    std::string path;
    std::string headers;
    std::string body;
    std::map<std::string, std::string> header_map;
    std::string session_token;
};

bool parse_request(const std::string& req, Request& out) {
    size_t pos1 = req.find("\r\n");
    if (pos1 == std::string::npos) return false;
    std::string req_line = req.substr(0, pos1);
    std::istringstream iss(req_line);
    iss >> out.method >> out.path;
    size_t pos2 = req.find("\r\n\r\n");
    if (pos2 == std::string::npos) return false;
    out.headers = req.substr(pos1+2, pos2-pos1-2);
    out.body = req.substr(pos2+4);
    out.header_map = parse_headers(out.headers);

    // Try get session token from headers
    auto it = out.header_map.find("Authorization");
    if (it != out.header_map.end()) {
        if (it->second.find("Bearer ") == 0) {
            out.session_token = it->second.substr(7);
        }
    }
    return true;
}

std::string gen_session_token() {
    static int counter = 1;
    std::ostringstream oss;
    oss << "sess-" << getpid() << "-" << time(nullptr) << "-" << (counter++);
    return oss.str();
}

void handle_login(const Request& req, int client_sock) {
    if (req.method != "POST") {
        std::string resp = http_405();
        send(client_sock, resp.data(), resp.size(), 0);
        return;
    }
    // Parse body as urlencoded
    auto params = parse_urlencoded(req.body);
    auto it_user = params.find("username");
    auto it_pass = params.find("password");
    if (it_user == params.end() || it_pass == params.end()) {
        std::string resp = http_401("Missing credentials");
        send(client_sock, resp.data(), resp.size(), 0);
        return;
    }
    if (hikvision_login(it_user->second, it_pass->second)) {
        std::string token = gen_session_token();
        session_store.set(token);
        std::string resp = http_200("{\"session_token\":\"" + token + "\"}");
        send(client_sock, resp.data(), resp.size(), 0);
    } else {
        std::string resp = http_401("Invalid credentials");
        send(client_sock, resp.data(), resp.size(), 0);
    }
}

void handle_capture(const Request& req, int client_sock) {
    if (req.method != "POST") {
        std::string resp = http_405();
        send(client_sock, resp.data(), resp.size(), 0);
        return;
    }
    if (req.session_token.empty() || !session_store.check(req.session_token)) {
        std::string resp = http_401();
        send(client_sock, resp.data(), resp.size(), 0);
        return;
    }
    std::string jpeg = hikvision_capture_jpeg();
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: image/jpeg\r\n"
        << "Content-Length: " << jpeg.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    send(client_sock, oss.str().data(), oss.str().size(), 0);
    send(client_sock, jpeg.data(), jpeg.size(), 0);
}

void handle_client(int client_sock) {
    char buffer[BUFFER_SIZE];
    ssize_t received = recv(client_sock, buffer, BUFFER_SIZE-1, 0);
    if (received <= 0) {
        close(client_sock);
        return;
    }
    buffer[received] = 0;
    std::string req_str(buffer);

    Request req;
    if (!parse_request(req_str, req)) {
        std::string resp = http_500("Malformed HTTP request");
        send(client_sock, resp.data(), resp.size(), 0);
        close(client_sock);
        return;
    }

    if (req.path == "/login") {
        handle_login(req, client_sock);
    } else if (req.path == "/capture") {
        handle_capture(req, client_sock);
    } else {
        std::string resp = http_404();
        send(client_sock, resp.data(), resp.size(), 0);
    }
    close(client_sock);
}

int main(int argc, char* argv[]) {
    // Configuration from env
    const char* env_host = getenv("SERVER_HOST");
    const char* env_port = getenv("SERVER_PORT");
    const char* env_device_ip = getenv("DEVICE_IP");
    const char* env_device_user = getenv("DEVICE_USER");
    const char* env_device_pass = getenv("DEVICE_PASS");

    std::string server_host = env_host ? env_host : "0.0.0.0";
    int server_port = env_port ? atoi(env_port) : 8080;
    mock_device_ip = env_device_ip ? env_device_ip : "192.168.1.100";
    mock_device_user = env_device_user ? env_device_user : "admin";
    mock_device_pass = env_device_pass ? env_device_pass : "12345";

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_host.c_str(), &addr.sin_addr);

    if (bind(server_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, MAX_CONNECTIONS) < 0) {
        std::cerr << "Listen failed\n";
        close(server_sock);
        return 1;
    }

    std::cout << "HTTP server listening on " << server_host << ":" << server_port << std::endl;

    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) continue;
        std::thread(handle_client, client_sock).detach();
    }

    close(server_sock);
    return 0;
}