```cpp
#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <map>
#include <mutex>
#include <vector>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <json/json.h> // Requires jsoncpp, but header-only usage is allowed
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

// ---------- Utility for Environment Variables ----------
std::string getenvOrDefault(const char* key, const std::string& def) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : def;
}

// ---------- HTTP Server Definitions ----------
#define MAX_HEADER 8192
#define MAX_BODY   65536

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::map<std::string, std::string> query_params;
};

struct HttpResponse {
    int status;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string httpStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

std::string urlDecode(const std::string& str) {
    std::string ret;
    char ch;
    int i, ii;
    for (i=0; i<str.length(); i++) {
        if (int(str[i])==37) {
            sscanf(str.substr(i+1,2).c_str(), "%x", &ii);
            ch=static_cast<char>(ii);
            ret+=ch;
            i=i+2;
        } else {
            ret+=str[i];
        }
    }
    return ret;
}

void parseQuery(const std::string& query, std::map<std::string, std::string>& params) {
    std::istringstream ss(query);
    std::string item;
    while (std::getline(ss, item, '&')) {
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            params[urlDecode(item.substr(0, eq))] = urlDecode(item.substr(eq+1));
        }
    }
}

bool startsWith(const std::string& str, const std::string& prefix) {
    return str.substr(0, prefix.size()) == prefix;
}

// ---------- Device "SDK" Layer (STUBS) ----------
struct DeviceSession {
    bool loggedIn = false;
    std::string token;
    std::mutex mtx;
};

struct DeviceInfo {
    std::string name;
    std::string model;
    std::string manufacturer;
    std::string type;
};

// In a real driver these would wrap actual SDK calls!
class HikvisionDecoderDriver {
public:
    HikvisionDecoderDriver() {
        // Device info could be fetched from the device or configured
        info.name = getenvOrDefault("DEVICE_NAME", "Decoder TV Wall");
        info.model = getenvOrDefault("DEVICE_MODEL", "DS-64XXHD-S");
        info.manufacturer = getenvOrDefault("DEVICE_MANUFACTURER", "Hikvision");
        info.type = getenvOrDefault("DEVICE_TYPE", "Network Video Decoder, TV Wall");
    }

    bool login(const std::string& user, const std::string& pass, std::string& token) {
        std::lock_guard<std::mutex> lock(sess.mtx);
        if (user == getenvOrDefault("DEVICE_USER", "admin") && pass == getenvOrDefault("DEVICE_PASS", "12345")) {
            sess.loggedIn = true;
            // Generate a pseudo-random token
            token = std::to_string(std::time(nullptr)) + "_token";
            sess.token = token;
            return true;
        }
        return false;
    }

    bool isAuthenticated(const std::string& token) {
        std::lock_guard<std::mutex> lock(sess.mtx);
        return sess.loggedIn && token == sess.token;
    }

    void logout() {
        std::lock_guard<std::mutex> lock(sess.mtx);
        sess.loggedIn = false;
        sess.token = "";
    }

    DeviceInfo getDeviceInfo() {
        return info;
    }

    Json::Value getDisplayConfig() {
        // Stub: Return sample display info
        Json::Value root;
        root["screen_count"] = 4;
        root["active_scene"] = "Main";
        root["brightness"] = 70;
        root["standby"] = false;
        return root;
    }

    bool setDisplayConfig(const Json::Value& cfg) {
        // Stub: Accept and "apply" config
        (void)cfg;
        return true;
    }

    Json::Value getChannels() {
        Json::Value root(Json::arrayValue);
        for (int i = 1; i <= 4; ++i) {
            Json::Value chan;
            chan["channel_id"] = i;
            chan["decoder_status"] = i%2==0 ? "idle" : "decoding";
            chan["input_stream"] = "rtsp://example.com/stream"+std::to_string(i);
            chan["output"] = "HDMI"+std::to_string(i);
            root.append(chan);
        }
        return root;
    }

    bool setChannels(const Json::Value& cfg) {
        // Stub: Accept and "apply" config
        (void)cfg;
        return true;
    }

    Json::Value getStatus(const std::map<std::string, std::string>& filters) {
        Json::Value root;
        root["sdk_state"] = "normal";
        root["decoder_status"] = "running";
        root["error_codes"] = 0;
        root["alarm_status"] = false;
        for (const auto& kv : filters) {
            root["filter_" + kv.first] = kv.second;
        }
        return root;
    }

    bool reboot() {
        // Stub: Pretend to reboot
        return true;
    }

private:
    DeviceSession sess;
    DeviceInfo info;
};

// ----------------- HTTP HANDLERS -----------------
class HttpApiServer {
public:
    HttpApiServer(const std::string& host, int port)
        : m_host(host), m_port(port) {}

    void run() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2),&wsa);
#endif
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "Socket creation failed\n";
            return;
        }
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(m_host.c_str());
        addr.sin_port = htons(m_port);
        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Bind failed\n";
            return;
        }
        listen(server_fd, 8);
        std::cout << "HTTP server running on " << m_host << ":" << m_port << "\n";
        while (true) {
            sockaddr_in client_addr;
            socklen_t clilen = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &clilen);
            if (client_fd < 0) continue;
            std::thread([this,client_fd](){
                this->handleClient(client_fd);
#ifdef _WIN32
                closesocket(client_fd);
#else
                close(client_fd);
#endif
            }).detach();
        }
    }
private:
    std::string m_host;
    int m_port;
    HikvisionDecoderDriver m_driver;

    void handleClient(int client_fd) {
        char buf[MAX_HEADER+MAX_BODY+1];
        int n = recv(client_fd, buf, MAX_HEADER, 0);
        if (n <= 0) return;
        buf[n] = 0;
        std::string request(buf);
        HttpRequest req;
        if (!parseHttpRequest(request, req)) {
            sendHttpError(client_fd, 400, "Malformed request");
            return;
        }
        if (req.headers.count("content-length")) {
            int cl = std::stoi(req.headers["content-length"]);
            int already = n - (request.find("\r\n\r\n")+4);
            while (req.body.length() < cl) {
                int m = recv(client_fd, buf, std::min(cl-req.body.length(), MAX_BODY), 0);
                if (m <= 0) break;
                req.body.append(buf, m);
            }
        }
        HttpResponse resp = route(req);
        sendHttpResponse(client_fd, resp);
    }

    void sendHttpError(int fd, int code, const std::string& msg) {
        HttpResponse resp;
        resp.status = code;
        resp.status_text = httpStatusText(code);
        resp.headers["Content-Type"] = "application/json";
        Json::Value err;
        err["error"] = msg;
        resp.body = err.toStyledString();
        sendHttpResponse(fd, resp);
    }

    void sendHttpResponse(int fd, const HttpResponse& resp) {
        std::ostringstream ss;
        ss << "HTTP/1.1 " << resp.status << " " << (resp.status_text.empty() ? httpStatusText(resp.status) : resp.status_text) << "\r\n";
        for (const auto& kv : resp.headers) {
            ss << kv.first << ": " << kv.second << "\r\n";
        }
        ss << "Content-Length: " << resp.body.size() << "\r\n";
        ss << "Connection: close\r\n";
        ss << "\r\n";
        ss << resp.body;
        std::string out = ss.str();
        send(fd, out.c_str(), out.size(), 0);
    }

    bool parseHttpRequest(const std::string& raw, HttpRequest& req) {
        size_t pos = raw.find("\r\n\r\n");
        if (pos == std::string::npos) return false;
        std::istringstream ss(raw.substr(0,pos));
        std::string line;
        if (!std::getline(ss, line)) return false;
        if (line.back() == '\r') line.pop_back();
        std::istringstream lss(line);
        lss >> req.method;
        std::string fullpath;
        lss >> fullpath;
        size_t q = fullpath.find('?');
        if (q != std::string::npos) {
            req.path = fullpath.substr(0, q);
            parseQuery(fullpath.substr(q+1), req.query_params);
        } else {
            req.path = fullpath;
        }
        while (std::getline(ss, line)) {
            if (line.back() == '\r') line.pop_back();
            size_t c = line.find(':');
            if (c != std::string::npos) {
                std::string key = line.substr(0, c);
                std::string val = line.substr(c+1);
                while (val.size() && val[0]==' ') val.erase(0,1);
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                req.headers[key] = val;
            }
        }
        req.body = raw.substr(pos+4);
        return true;
    }

    // --------- API Routing ---------
    HttpResponse route(const HttpRequest& req) {
        // Authentication for protected endpoints
        bool needs_auth = (
            (req.method == "PUT" && (req.path == "/display" || req.path == "/channels")) ||
            (req.method == "POST" && req.path == "/cmd/reboot")
        );
        std::string token;
        if (req.headers.count("authorization")) {
            token = req.headers.at("authorization");
            if (startsWith(token, "Bearer ")) token = token.substr(7);
        }
        if (needs_auth && !m_driver.isAuthenticated(token)) {
            HttpResponse resp;
            resp.status = 401;
            resp.status_text = "Unauthorized";
            resp.headers["Content-Type"] = "application/json";
            Json::Value j;
            j["error"] = "Unauthorized";
            resp.body = j.toStyledString();
            return resp;
        }
        // Route
        if (req.method == "POST" && req.path == "/auth/login") {
            return api_auth_login(req);
        } else if (req.method == "GET" && req.path == "/device") {
            return api_get_device(req);
        } else if (req.method == "GET" && req.path == "/display") {
            return api_get_display(req);
        } else if (req.method == "PUT" && req.path == "/display") {
            return api_put_display(req, token);
        } else if (req.method == "GET" && req.path == "/channels") {
            return api_get_channels(req);
        } else if (req.method == "PUT" && req.path == "/channels") {
            return api_put_channels(req, token);
        } else if (req.method == "GET" && req.path == "/status") {
            return api_get_status(req);
        } else if (req.method == "POST" && req.path == "/cmd/reboot") {
            return api_cmd_reboot(req, token);
        } else {
            HttpResponse resp;
            resp.status = 404;
            resp.status_text = "Not Found";
            resp.headers["Content-Type"] = "application/json";
            Json::Value j;
            j["error"] = "Not Found";
            resp.body = j.toStyledString();
            return resp;
        }
    }

    // --------- API Endpoint Implementations ---------
    HttpResponse api_auth_login(const HttpRequest& req) {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream ss(req.body);
        if (!Json::parseFromStream(builder, ss, &root, &errs)) {
            return makeJsonError(400, "Invalid JSON");
        }
        std::string user = root.get("username", "").asString();
        std::string pass = root.get("password", "").asString();
        std::string token;
        if (!m_driver.login(user, pass, token)) {
            return makeJsonError(401, "Login failed");
        }
        Json::Value respj;
        respj["token"] = token;
        HttpResponse resp;
        resp.status = 200;
        resp.status_text = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = respj.toStyledString();
        return resp;
    }

    HttpResponse api_get_device(const HttpRequest& req) {
        DeviceInfo di = m_driver.getDeviceInfo();
        Json::Value j;
        j["device_name"] = di.name;
        j["device_model"] = di.model;
        j["manufacturer"] = di.manufacturer;
        j["device_type"] = di.type;
        HttpResponse resp;
        resp.status = 200;
        resp.status_text = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = j.toStyledString();
        return resp;
    }

    HttpResponse api_get_display(const HttpRequest& req) {
        Json::Value j = m_driver.getDisplayConfig();
        HttpResponse resp;
        resp.status = 200;
        resp.status_text = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = j.toStyledString();
        return resp;
    }

    HttpResponse api_put_display(const HttpRequest& req, const std::string& token) {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream ss(req.body);
        if (!Json::parseFromStream(builder, ss, &root, &errs)) {
            return makeJsonError(400, "Invalid JSON");
        }
        if (!m_driver.setDisplayConfig(root)) {
            return makeJsonError(500, "Failed to update display config");
        }
        HttpResponse resp;
        resp.status = 204;
        resp.status_text = "No Content";
        return resp;
    }

    HttpResponse api_get_channels(const HttpRequest& req) {
        Json::Value j = m_driver.getChannels();
        HttpResponse resp;
        resp.status = 200;
        resp.status_text = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = j.toStyledString();
        return resp;
    }

    HttpResponse api_put_channels(const HttpRequest& req, const std::string& token) {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream ss(req.body);
        if (!Json::parseFromStream(builder, ss, &root, &errs)) {
            return makeJsonError(400, "Invalid JSON");
        }
        if (!m_driver.setChannels(root)) {
            return makeJsonError(500, "Failed to update channels");
        }
        HttpResponse resp;
        resp.status = 204;
        resp.status_text = "No Content";
        return resp;
    }

    HttpResponse api_get_status(const HttpRequest& req) {
        Json::Value j = m_driver.getStatus(req.query_params);
        HttpResponse resp;
        resp.status = 200;
        resp.status_text = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = j.toStyledString();
        return resp;
    }

    HttpResponse api_cmd_reboot(const HttpRequest& req, const std::string& token) {
        if (!m_driver.reboot()) {
            return makeJsonError(500, "Reboot failed");
        }
        HttpResponse resp;
        resp.status = 200;
        resp.status_text = "OK";
        Json::Value j;
        j["reboot"] = "initiated";
        resp.headers["Content-Type"] = "application/json";
        resp.body = j.toStyledString();
        return resp;
    }

    HttpResponse makeJsonError(int code, const std::string& msg) {
        HttpResponse resp;
        resp.status = code;
        resp.status_text = httpStatusText(code);
        resp.headers["Content-Type"] = "application/json";
        Json::Value j;
        j["error"] = msg;
        resp.body = j.toStyledString();
        return resp;
    }
};

// --------------- Main Entrypoint ---------------
int main() {
    std::string host = getenvOrDefault("HTTP_HOST", "0.0.0.0");
    int port = std::stoi(getenvOrDefault("HTTP_PORT", "8080"));
    HttpApiServer server(host, port);
    server.run();
    return 0;
}
```
