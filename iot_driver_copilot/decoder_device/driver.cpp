#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <unordered_map>
#include <json/json.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

// ========== Configuration via Environment Variables ==========
struct Config {
    std::string device_ip;
    int device_port;
    std::string device_user;
    std::string device_pass;
    std::string server_host;
    int server_port;
    Config() {
        device_ip = getenv("DEVICE_IP") ? getenv("DEVICE_IP") : "192.168.1.100";
        device_port = getenv("DEVICE_PORT") ? atoi(getenv("DEVICE_PORT")) : 8000;
        device_user = getenv("DEVICE_USER") ? getenv("DEVICE_USER") : "admin";
        device_pass = getenv("DEVICE_PASS") ? getenv("DEVICE_PASS") : "12345";
        server_host = getenv("HTTP_SERVER_HOST") ? getenv("HTTP_SERVER_HOST") : "0.0.0.0";
        server_port = getenv("HTTP_SERVER_PORT") ? atoi(getenv("HTTP_SERVER_PORT")) : 8080;
    }
};

// ========== Minimal HTTP Server Infrastructure ==========

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> query;
};

struct HttpResponse {
    int status;
    std::string content_type;
    std::string body;
    std::map<std::string, std::string> headers;
    HttpResponse() : status(200), content_type("application/json") {}
};

using HandlerFunc = std::function<void(const HttpRequest&, HttpResponse&)>;

class Router {
public:
    void addRoute(const std::string& method, const std::string& path, HandlerFunc handler) {
        routes_[method + ":" + path] = handler;
    }
    HandlerFunc findHandler(const std::string& method, const std::string& path) {
        auto it = routes_.find(method + ":" + path);
        if (it != routes_.end()) return it->second;
        return nullptr;
    }
private:
    std::unordered_map<std::string, HandlerFunc> routes_;
};

std::string urlDecode(const std::string& str) {
    std::string decoded;
    char a, b;
    for (size_t i = 0; i < str.size(); ++i) {
        if ((str[i] == '%') && ((a = str[i + 1]) && (b = str[i + 2])) &&
            (isxdigit(a) && isxdigit(b))) {
            a = (a <= '9') ? a - '0' : std::tolower(a) - 'a' + 10;
            b = (b <= '9') ? b - '0' : std::tolower(b) - 'a' + 10;
            decoded += 16 * a + b;
            i += 2;
        } else if (str[i] == '+') {
            decoded += ' ';
        } else {
            decoded += str[i];
        }
    }
    return decoded;
}

void parseQuery(const std::string& query_string, std::map<std::string, std::string>& query) {
    std::istringstream ss(query_string);
    std::string item;
    while (std::getline(ss, item, '&')) {
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            query[urlDecode(item.substr(0, eq))] = urlDecode(item.substr(eq+1));
        }
    }
}

bool parseHttpRequest(const std::string& raw, HttpRequest& req) {
    std::istringstream ss(raw);
    std::string line;
    if (!std::getline(ss, line)) return false;
    std::istringstream ls(line);
    if (!(ls >> req.method >> req.path)) return false;
    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string query_str = req.path.substr(qpos+1);
        req.path = req.path.substr(0, qpos);
        parseQuery(query_str, req.query);
    }
    while (std::getline(ss, line) && line != "\r") {
        if (line.empty() || line == "\r\n" || line == "\n") break;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon+1);
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            req.headers[key] = val;
        }
    }
    std::string body;
    while (std::getline(ss, line)) body += line + "\n";
    req.body = body;
    if (!body.empty() && body[body.size()-1] == '\n')
        req.body.pop_back();
    return true;
}

void writeHttpResponse(int client_fd, const HttpResponse& resp) {
    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << " ";
    switch (resp.status) {
        case 200: out << "OK"; break;
        case 201: out << "Created"; break;
        case 204: out << "No Content"; break;
        case 400: out << "Bad Request"; break;
        case 401: out << "Unauthorized"; break;
        case 403: out << "Forbidden"; break;
        case 404: out << "Not Found"; break;
        case 500: out << "Internal Server Error"; break;
        default: out << "Unknown"; break;
    }
    out << "\r\n";
    out << "Content-Type: " << resp.content_type << "\r\n";
    out << "Content-Length: " << resp.body.size() << "\r\n";
    for (auto& h : resp.headers) {
        out << h.first << ": " << h.second << "\r\n";
    }
    out << "\r\n";
    out << resp.body;
    std::string outstr = out.str();
    send(client_fd, outstr.c_str(), outstr.size(), 0);
}

// ========== Session Management ==========
struct SessionData {
    std::string user;
    std::string token;
    time_t created;
};
std::mutex session_mutex;
std::map<std::string, SessionData> sessions;

std::string randomToken() {
    static const char alphanum[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string token;
    srand(time(NULL) ^ rand());
    for (int i = 0; i < 32; ++i)
        token += alphanum[rand() % (sizeof(alphanum) - 1)];
    return token;
}

bool validateSession(const HttpRequest& req, std::string& user) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) return false;
    std::string token = it->second;
    std::lock_guard<std::mutex> lock(session_mutex);
    auto sit = sessions.find(token);
    if (sit == sessions.end()) return false;
    user = sit->second.user;
    return true;
}

void destroySession(const std::string& token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    sessions.erase(token);
}

// ========== Device Layer (Stub/Simulation) ==========
// In real implementation, would call actual SDK, RTSP, TCP or interact with device.

struct DeviceState {
    std::string model = "DS-64XXHD_S";
    std::string status = "Online";
    std::string sdk_version = "5.1.2";
    int error_code = 0;
    Json::Value config;
    DeviceState() {
        config["display"]["mode"] = "quad";
        config["scene"]["current"] = "default";
        config["ability"]["max_dec_channels"] = 16;
    }
} device;

// ========== API Handlers ==========

void handle_login(const HttpRequest& req, HttpResponse& resp, const Config& config) {
    Json::Value req_json;
    Json::Reader reader;
    if (!reader.parse(req.body, req_json)) {
        resp.status = 400;
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return;
    }
    std::string user = req_json.get("username", "").asString();
    std::string pass = req_json.get("password", "").asString();
    if (user.empty() || pass.empty() ||
        user != config.device_user || pass != config.device_pass) {
        resp.status = 401;
        resp.body = "{\"error\":\"Authentication failed\"}";
        return;
    }
    std::string token = randomToken();
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        SessionData sess{user, token, time(NULL)};
        sessions[token] = sess;
    }
    Json::Value res;
    res["token"] = token;
    res["expires_in"] = 3600;
    resp.body = res.toStyledString();
}

void handle_create_session(const HttpRequest& req, HttpResponse& resp, const Config& config) {
    handle_login(req, resp, config);
}

void handle_delete_session(const HttpRequest& req, HttpResponse& resp) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        resp.status = 401;
        resp.body = "{\"error\":\"No token provided\"}";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        auto sit = sessions.find(it->second);
        if (sit == sessions.end()) {
            resp.status = 401;
            resp.body = "{\"error\":\"Invalid session\"}";
            return;
        }
        sessions.erase(sit);
    }
    resp.status = 204;
    resp.body = "";
}

void handle_device(const HttpRequest& req, HttpResponse& resp) {
    Json::Value res;
    res["device_name"] = "Decoder Device";
    res["device_model"] = device.model;
    res["manufacturer"] = "Hikvision";
    res["device_type"] = "Network Video Decoder";
    res["status"] = device.status;
    res["sdk_version"] = device.sdk_version;
    res["error_code"] = device.error_code;
    resp.body = res.toStyledString();
}

void handle_status(const HttpRequest& req, HttpResponse& resp) {
    Json::Value res;
    res["decoding_channels"] = 8;
    res["alarm_status"] = "normal";
    res["playback_progress"] = 0;
    res["stream_info"] = Json::arrayValue;
    Json::Value stream;
    stream["channel"] = 1;
    stream["status"] = "playing";
    res["stream_info"].append(stream);
    resp.body = res.toStyledString();
}

void handle_config_get(const HttpRequest& req, HttpResponse& resp) {
    std::string type;
    auto it = req.query.find("type");
    if (it != req.query.end()) type = it->second;
    if (type.empty()) {
        resp.body = device.config.toStyledString();
        return;
    }
    if (device.config.isMember(type)) {
        resp.body = device.config[type].toStyledString();
        return;
    }
    resp.status = 404;
    resp.body = "{\"error\":\"Config section not found\"}";
}

void handle_config_put(const HttpRequest& req, HttpResponse& resp) {
    std::string user;
    if (!validateSession(req, user)) {
        resp.status = 401;
        resp.body = "{\"error\":\"Unauthorized\"}";
        return;
    }
    Json::Value req_json;
    Json::Reader reader;
    if (!reader.parse(req.body, req_json)) {
        resp.status = 400;
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return;
    }
    for (auto it = req_json.begin(); it != req_json.end(); ++it) {
        device.config[it.key().asString()] = *it;
    }
    resp.body = device.config.toStyledString();
}

void handle_sdk(const HttpRequest& req, HttpResponse& resp) {
    std::string user;
    if (!validateSession(req, user)) {
        resp.status = 401;
        resp.body = "{\"error\":\"Unauthorized\"}";
        return;
    }
    Json::Value req_json;
    Json::Reader reader;
    if (!reader.parse(req.body, req_json)) {
        resp.status = 400;
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return;
    }
    std::string action = req_json.get("action", "").asString();
    Json::Value res;
    if (action == "init") {
        device.status = "SDK Initialized";
        res["result"] = "SDK initialized";
    } else if (action == "cleanup") {
        device.status = "SDK Cleaned Up";
        res["result"] = "SDK cleaned up";
    } else {
        resp.status = 400;
        resp.body = "{\"error\":\"Unknown action\"}";
        return;
    }
    resp.body = res.toStyledString();
}

void handle_decode(const HttpRequest& req, HttpResponse& resp) {
    std::string user;
    if (!validateSession(req, user)) {
        resp.status = 401;
        resp.body = "{\"error\":\"Unauthorized\"}";
        return;
    }
    Json::Value req_json;
    Json::Reader reader;
    if (!reader.parse(req.body, req_json)) {
        resp.status = 400;
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return;
    }
    std::string action = req_json.get("action", "").asString();
    std::string mode = req_json.get("mode", "dynamic").asString();
    Json::Value res;
    if (action == "start") {
        res["result"] = "Decoding started";
        res["mode"] = mode;
    } else if (action == "stop") {
        res["result"] = "Decoding stopped";
        res["mode"] = mode;
    } else {
        resp.status = 400;
        resp.body = "{\"error\":\"Unknown action\"}";
        return;
    }
    resp.body = res.toStyledString();
}

void handle_playback(const HttpRequest& req, HttpResponse& resp) {
    std::string user;
    if (!validateSession(req, user)) {
        resp.status = 401;
        resp.body = "{\"error\":\"Unauthorized\"}";
        return;
    }
    Json::Value req_json;
    Json::Reader reader;
    if (!reader.parse(req.body, req_json)) {
        resp.status = 400;
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return;
    }
    std::string action = req_json.get("action", "").asString();
    Json::Value res;
    if (action == "start") {
        res["result"] = "Playback started";
    } else if (action == "stop") {
        res["result"] = "Playback stopped";
    } else if (action == "pause") {
        res["result"] = "Playback paused";
    } else if (action == "resume") {
        res["result"] = "Playback resumed";
    } else {
        resp.status = 400;
        resp.body = "{\"error\":\"Unknown action\"}";
        return;
    }
    resp.body = res.toStyledString();
}

void handle_reboot(const HttpRequest& req, HttpResponse& resp) {
    std::string user;
    if (!validateSession(req, user)) {
        resp.status = 401;
        resp.body = "{\"error\":\"Unauthorized\"}";
        return;
    }
    device.status = "Rebooting";
    Json::Value res;
    res["result"] = "Reboot initiated";
    resp.body = res.toStyledString();
}

void handle_system(const HttpRequest& req, HttpResponse& resp) {
    std::string user;
    if (!validateSession(req, user)) {
        resp.status = 401;
        resp.body = "{\"error\":\"Unauthorized\"}";
        return;
    }
    Json::Value req_json;
    Json::Reader reader;
    if (!reader.parse(req.body, req_json)) {
        resp.status = 400;
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return;
    }
    std::string action = req_json.get("action", "").asString();
    Json::Value res;
    if (action == "upgrade") {
        device.status = "Upgrading";
        res["result"] = "Upgrade started";
    } else if (action == "reboot") {
        device.status = "Rebooting";
        res["result"] = "Reboot initiated";
    } else if (action == "shutdown") {
        device.status = "Shutdown";
        res["result"] = "Shutdown initiated";
    } else {
        resp.status = 400;
        resp.body = "{\"error\":\"Unknown action\"}";
        return;
    }
    resp.body = res.toStyledString();
}

void handle_upgrade(const HttpRequest& req, HttpResponse& resp) {
    std::string user;
    if (!validateSession(req, user)) {
        resp.status = 401;
        resp.body = "{\"error\":\"Unauthorized\"}";
        return;
    }
    Json::Value req_json;
    Json::Reader reader;
    if (!reader.parse(req.body, req_json)) {
        resp.status = 400;
        resp.body = "{\"error\":\"Invalid JSON\"}";
        return;
    }
    device.status = "Upgrading";
    Json::Value res;
    res["result"] = "Upgrade started";
    resp.body = res.toStyledString();
}

// ========== HTTP Server Loop ==========

void http_server(const Config& config, Router& router) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) exit(1);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.server_port);
    addr.sin_addr.s_addr = inet_addr(config.server_host.c_str());
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) exit(1);
    listen(server_fd, 16);
    while (true) {
        sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(server_fd, (sockaddr*)&cli_addr, &cli_len);
        if (client_fd < 0) continue;
        std::thread([client_fd, &router, &config]() {
            char buf[8192];
            int n = recv(client_fd, buf, sizeof(buf)-1, 0);
            if (n <= 0) { close(client_fd); return; }
            buf[n] = 0;
            HttpRequest req;
            if (!parseHttpRequest(buf, req)) {
                HttpResponse resp;
                resp.status = 400;
                resp.body = "{\"error\":\"Malformed HTTP request\"}";
                writeHttpResponse(client_fd, resp);
                close(client_fd);
                return;
            }
            HttpResponse resp;
            HandlerFunc handler = router.findHandler(req.method, req.path);
            if (!handler) {
                resp.status = 404;
                resp.body = "{\"error\":\"Not found\"}";
            } else {
                handler(req, resp);
            }
            writeHttpResponse(client_fd, resp);
            close(client_fd);
        }).detach();
    }
}

// ========== Main Entrypoint ==========

int main() {
    Config config;
    Router router;

    // Route registration per API
    router.addRoute("POST", "/login", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_login(req, resp, config);
    });
    router.addRoute("POST", "/session", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_create_session(req, resp, config);
    });
    router.addRoute("DELETE", "/session", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_delete_session(req, resp);
    });
    router.addRoute("GET", "/device", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_device(req, resp);
    });
    router.addRoute("GET", "/status", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_status(req, resp);
    });
    router.addRoute("GET", "/config", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_config_get(req, resp);
    });
    router.addRoute("PUT", "/config", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_config_put(req, resp);
    });
    router.addRoute("POST", "/config", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_config_put(req, resp);
    });
    router.addRoute("POST", "/sdk", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_sdk(req, resp);
    });
    router.addRoute("POST", "/decode", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_decode(req, resp);
    });
    router.addRoute("POST", "/playback", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_playback(req, resp);
    });
    router.addRoute("POST", "/reboot", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_reboot(req, resp);
    });
    router.addRoute("POST", "/system", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_system(req, resp);
    });
    router.addRoute("POST", "/upgrade", [&](const HttpRequest& req, HttpResponse& resp) {
        handle_upgrade(req, resp);
    });

    std::cout << "HTTP Server running on " << config.server_host << ":" << config.server_port << std::endl;
    http_server(config, router);
    return 0;
}