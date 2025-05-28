#include <iostream>
#include <string>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>
#include <vector>
#include <ctime>
#include <condition_variable>
#include <json/json.h> // Requires jsoncpp library (header-only parts)
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// Dummy HCNetSDK simulation (replace this with actual SDK integration for real device)
namespace HCNetSDKSim {
    struct Session {
        std::string username;
        std::string token;
        bool valid;
    };
    static std::mutex session_mutex;
    static Session session = {"", "", false};
    static Json::Value current_config;
    static Json::Value device_status;
    static std::string last_error = "";

    bool login(const std::string& user, const std::string& pass, std::string& token) {
        std::lock_guard<std::mutex> lock(session_mutex);
        if (user == "admin" && pass == "12345") {
            session.username = user;
            session.token = "SESSION_" + std::to_string(std::time(nullptr));
            session.valid = true;
            token = session.token;
            return true;
        }
        last_error = "Invalid credentials";
        return false;
    }
    bool logout(const std::string& token) {
        std::lock_guard<std::mutex> lock(session_mutex);
        if (!session.valid || session.token != token) {
            last_error = "Invalid session";
            return false;
        }
        session = {"", "", false};
        return true;
    }
    bool check_session(const std::string& token) {
        std::lock_guard<std::mutex> lock(session_mutex);
        return session.valid && session.token == token;
    }
    Json::Value get_config(const std::string& type) {
        // Dummy configs
        if (type == "display") {
            Json::Value j;
            j["display_mode"] = "wall";
            j["wall_id"] = 1;
            return j;
        } else if (type == "channel") {
            Json::Value j;
            j["channels"] = Json::arrayValue;
            for (int i = 0; i < 4; ++i) {
                Json::Value c;
                c["id"] = i;
                c["enabled"] = true;
                j["channels"].append(c);
            }
            return j;
        }
        Json::Value j;
        j["info"] = "Unknown config type";
        return j;
    }
    bool set_config(const std::string& type, const Json::Value& cfg) {
        // Accept all in dummy
        current_config[type] = cfg;
        return true;
    }
    Json::Value get_status() {
        Json::Value j;
        j["device"] = "Hikvision Decoder";
        j["uptime"] = static_cast<Json::UInt64>(std::time(nullptr));
        j["channels_online"] = 4;
        j["alarm"] = false;
        j["upgrade_progress"] = 0;
        j["error_codes"] = Json::arrayValue;
        return j;
    }
    bool decode_control(const Json::Value& req, Json::Value& resp) {
        if (!req.isMember("action") || !req.isMember("mode")) {
            resp["error"] = "Missing action or mode";
            return false;
        }
        resp["result"] = "Decode " + req["action"].asString() + " in mode " + req["mode"].asString();
        return true;
    }
    bool reboot(const Json::Value& req, Json::Value& resp) {
        resp["result"] = "Reboot command accepted";
        return true;
    }
    bool upgrade(const Json::Value& req, Json::Value& resp) {
        resp["result"] = "Upgrade command accepted";
        return true;
    }
    const std::string& get_last_error() {
        return last_error;
    }
}

// HTTP utility
std::string http_status_message(int status) {
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

void send_http_response(int client, int status_code, const std::string& content_type, const std::string& body, const std::map<std::string, std::string>& headers = {}) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << http_status_message(status_code) << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.length() << "\r\n";
    for (const auto& h : headers) {
        oss << h.first << ": " << h.second << "\r\n";
    }
    oss << "Connection: close\r\n\r\n";
    oss << body;
    std::string resp = oss.str();
    send(client, resp.c_str(), resp.length(), 0);
}

std::string url_decode(const std::string& s) {
    std::string out;
    char ch;
    int i, ii;
    for (i = 0; i < s.length(); i++) {
        if (s[i] == '%') {
            sscanf(s.substr(i + 1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            out += ch;
            i = i + 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> m;
    std::istringstream iss(query);
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq));
            std::string val = url_decode(pair.substr(eq + 1));
            m[key] = val;
        }
    }
    return m;
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string session_token;
};

bool parse_http_request(const std::string& request, HttpRequest& req) {
    std::istringstream iss(request);
    std::string line;
    if (!std::getline(iss, line)) return false;
    std::istringstream l1(line);
    l1 >> req.method;
    std::string fullpath;
    l1 >> fullpath;
    size_t qpos = fullpath.find('?');
    if (qpos != std::string::npos) {
        req.path = fullpath.substr(0, qpos);
        req.query = fullpath.substr(qpos + 1);
    } else {
        req.path = fullpath;
        req.query = "";
    }
    while (std::getline(iss, line) && line != "\r") {
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            if (!val.empty() && val.back() == '\r') val.pop_back();
            req.headers[key] = val;
        }
    }
    if (req.headers.count("Cookie")) {
        std::string c = req.headers["Cookie"];
        size_t pos = c.find("session_token=");
        if (pos != std::string::npos) {
            size_t end = c.find(';', pos);
            req.session_token = c.substr(pos + 14, end == std::string::npos ? std::string::npos : end - (pos + 14));
        }
    }
    if (req.headers.count("Authorization")) {
        std::string a = req.headers["Authorization"];
        if (a.find("Bearer ") == 0)
            req.session_token = a.substr(7);
    }
    // Read body if present
    if (req.headers.count("Content-Length")) {
        int cl = std::stoi(req.headers["Content-Length"]);
        req.body.resize(cl);
        iss.read(&req.body[0], cl);
    }
    return true;
}

// Endpoint Handlers
void handle_login(const HttpRequest& req, int client) {
    Json::Value resp;
    Json::Value jreq;
    Json::Reader reader;
    if (!reader.parse(req.body, jreq) || !jreq.isMember("username") || !jreq.isMember("password")) {
        resp["error"] = "Invalid JSON or missing credentials";
        send_http_response(client, 400, "application/json", resp.toStyledString());
        return;
    }
    std::string user = jreq["username"].asString();
    std::string pass = jreq["password"].asString();
    std::string token;
    if (HCNetSDKSim::login(user, pass, token)) {
        resp["session_token"] = token;
        send_http_response(client, 200, "application/json", resp.toStyledString(), {{"Set-Cookie", "session_token=" + token + "; Path=/"}});
    } else {
        resp["error"] = HCNetSDKSim::get_last_error();
        send_http_response(client, 401, "application/json", resp.toStyledString());
    }
}

void handle_logout(const HttpRequest& req, int client) {
    Json::Value resp;
    std::string token = req.session_token;
    if (!HCNetSDKSim::check_session(token)) {
        resp["error"] = "Invalid or missing session token";
        send_http_response(client, 401, "application/json", resp.toStyledString());
        return;
    }
    if (HCNetSDKSim::logout(token)) {
        resp["result"] = "Logged out";
        send_http_response(client, 200, "application/json", resp.toStyledString(), {{"Set-Cookie", "session_token=; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Path=/"}} );
    } else {
        resp["error"] = HCNetSDKSim::get_last_error();
        send_http_response(client, 401, "application/json", resp.toStyledString());
    }
}

void handle_get_config(const HttpRequest& req, int client) {
    if (!HCNetSDKSim::check_session(req.session_token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, "application/json", resp.toStyledString());
        return;
    }
    std::map<std::string, std::string> q = parse_query(req.query);
    std::string type = q.count("type") ? q["type"] : "";
    Json::Value cfg = HCNetSDKSim::get_config(type);
    send_http_response(client, 200, "application/json", cfg.toStyledString());
}

void handle_put_config(const HttpRequest& req, int client) {
    if (!HCNetSDKSim::check_session(req.session_token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, "application/json", resp.toStyledString());
        return;
    }
    std::map<std::string, std::string> q = parse_query(req.query);
    std::string type = q.count("type") ? q["type"] : "";
    Json::Value jreq;
    Json::Reader reader;
    if (!reader.parse(req.body, jreq)) {
        Json::Value resp;
        resp["error"] = "Invalid JSON";
        send_http_response(client, 400, "application/json", resp.toStyledString());
        return;
    }
    if (HCNetSDKSim::set_config(type, jreq)) {
        Json::Value resp;
        resp["result"] = "Configuration updated";
        send_http_response(client, 200, "application/json", resp.toStyledString());
    } else {
        Json::Value resp;
        resp["error"] = "Failed to update config";
        send_http_response(client, 500, "application/json", resp.toStyledString());
    }
}

void handle_status(const HttpRequest& req, int client) {
    if (!HCNetSDKSim::check_session(req.session_token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, "application/json", resp.toStyledString());
        return;
    }
    Json::Value stat = HCNetSDKSim::get_status();
    send_http_response(client, 200, "application/json", stat.toStyledString());
}

void handle_decode(const HttpRequest& req, int client) {
    if (!HCNetSDKSim::check_session(req.session_token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, "application/json", resp.toStyledString());
        return;
    }
    Json::Value jreq;
    Json::Reader reader;
    if (!reader.parse(req.body, jreq)) {
        Json::Value resp;
        resp["error"] = "Invalid JSON";
        send_http_response(client, 400, "application/json", resp.toStyledString());
        return;
    }
    Json::Value resp;
    if (HCNetSDKSim::decode_control(jreq, resp)) {
        send_http_response(client, 200, "application/json", resp.toStyledString());
    } else {
        send_http_response(client, 400, "application/json", resp.toStyledString());
    }
}

void handle_reboot(const HttpRequest& req, int client) {
    if (!HCNetSDKSim::check_session(req.session_token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, "application/json", resp.toStyledString());
        return;
    }
    Json::Value jreq;
    Json::Reader reader;
    if (!req.body.empty() && !reader.parse(req.body, jreq)) {
        Json::Value resp;
        resp["error"] = "Invalid JSON";
        send_http_response(client, 400, "application/json", resp.toStyledString());
        return;
    }
    Json::Value resp;
    if (HCNetSDKSim::reboot(jreq, resp)) {
        send_http_response(client, 200, "application/json", resp.toStyledString());
    } else {
        send_http_response(client, 500, "application/json", resp.toStyledString());
    }
}

void handle_upgrade(const HttpRequest& req, int client) {
    if (!HCNetSDKSim::check_session(req.session_token)) {
        Json::Value resp;
        resp["error"] = "Unauthorized";
        send_http_response(client, 401, "application/json", resp.toStyledString());
        return;
    }
    Json::Value jreq;
    Json::Reader reader;
    if (!reader.parse(req.body, jreq)) {
        Json::Value resp;
        resp["error"] = "Invalid JSON";
        send_http_response(client, 400, "application/json", resp.toStyledString());
        return;
    }
    Json::Value resp;
    if (HCNetSDKSim::upgrade(jreq, resp)) {
        send_http_response(client, 200, "application/json", resp.toStyledString());
    } else {
        send_http_response(client, 500, "application/json", resp.toStyledString());
    }
}

struct Route {
    std::string method;
    std::string path;
    void (*handler)(const HttpRequest&, int);
};

#define ROUTE_ENTRY(M, P, H) {M, P, H}

// Routing
const std::vector<Route> routes = {
    ROUTE_ENTRY("POST", "/login", handle_login),
    ROUTE_ENTRY("POST", "/logout", handle_logout),
    ROUTE_ENTRY("GET", "/config", handle_get_config),
    ROUTE_ENTRY("PUT", "/config", handle_put_config),
    ROUTE_ENTRY("POST", "/decode", handle_decode),
    ROUTE_ENTRY("POST", "/command/decode", handle_decode), // Alias
    ROUTE_ENTRY("GET", "/status", handle_status),
    ROUTE_ENTRY("POST", "/reboot", handle_reboot),
    ROUTE_ENTRY("POST", "/command/reboot", handle_reboot), // Alias
    ROUTE_ENTRY("POST", "/upgrade", handle_upgrade),
    ROUTE_ENTRY("POST", "/command/upgrade", handle_upgrade) // Alias
};

void handle_404(int client) {
    Json::Value resp;
    resp["error"] = "Not Found";
    send_http_response(client, 404, "application/json", resp.toStyledString());
}

// Main HTTP server loop
void http_server_loop(const std::string& host, int port) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "socket failed\n";
        exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        std::cerr << "setsockopt\n";
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = (host == "0.0.0.0") ? INADDR_ANY : inet_addr(host.c_str());
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "bind failed\n";
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 16) < 0) {
        std::cerr << "listen\n";
        exit(EXIT_FAILURE);
    }
    std::cout << "HTTP server listening on " << host << ":" << port << std::endl;
    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            std::cerr << "accept\n";
            continue;
        }
        std::thread([new_socket]() {
            char buffer[8192] = {0};
            int valread = recv(new_socket, buffer, sizeof(buffer) - 1, 0);
            if (valread > 0) {
                std::string reqstr(buffer, valread);
                HttpRequest req;
                if (!parse_http_request(reqstr, req)) {
                    send_http_response(new_socket, 400, "application/json", "{\"error\": \"Malformed Request\"}");
                    close(new_socket);
                    return;
                }
                bool matched = false;
                for (const auto& r : routes) {
                    if (req.method == r.method && req.path == r.path) {
                        r.handler(req, new_socket);
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    handle_404(new_socket);
                }
            }
            close(new_socket);
        }).detach();
    }
}

// Entry point
int main() {
    std::string device_ip = getenv("DEVICE_IP") ? getenv("DEVICE_IP") : "127.0.0.1";
    std::string server_host = getenv("SERVER_HOST") ? getenv("SERVER_HOST") : "0.0.0.0";
    int server_port = getenv("SERVER_PORT") ? atoi(getenv("SERVER_PORT")) : 8080;

    // (device_ip not used in simulation; would be used in real HCNetSDK integration)
    http_server_loop(server_host, server_port);
    return 0;
}