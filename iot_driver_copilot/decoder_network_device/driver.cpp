// hikvision_decoder_http_driver.cpp

#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <json/json.h>

// Mock SDK API (replace with actual HCNetSDK headers and libs)
struct DeviceSession {
    bool logged_in;
    std::string session_token;
    std::string username;
    std::string password;
    // Add more as needed
};
std::mutex g_sdk_mutex;
DeviceSession g_device_session = {false, "", "", ""};

bool SDK_Login(const std::string& ip, int port, const std::string& user, const std::string& pwd) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    g_device_session.logged_in = true;
    g_device_session.username = user;
    g_device_session.password = pwd;
    g_device_session.session_token = "sess_" + user + "_token";
    return true;
}
bool SDK_Logout() {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    g_device_session.logged_in = false;
    g_device_session.session_token = "";
    return true;
}
bool SDK_GetStatus(Json::Value& out) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    out["status"] = g_device_session.logged_in ? "online" : "offline";
    out["channels"] = 16;
    out["display"] = "4x4";
    out["alarm"] = false;
    out["upgrade_in_progress"] = false;
    out["error_code"] = 0;
    return true;
}
bool SDK_GetConfig(const std::string& type, Json::Value& out) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if(type == "display") out["layout"] = "4x4";
    else if(type == "channel") out["channels"] = 16;
    else if(type == "decode") out["mode"] = "dynamic";
    else out["info"] = "unknown config type";
    return true;
}
bool SDK_SetConfig(const std::string& type, const Json::Value& cfg) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    return true;
}
bool SDK_DecodeControl(const std::string& action, const std::string& mode) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    return true;
}
bool SDK_Reboot(int delay_ms = 0, bool shutdown = false) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    return true;
}
bool SDK_Upgrade(const Json::Value& upgrade_params) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    return true;
}

// Utility: getenv with fallback
std::string getenv_or(const char* k, const char* def) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(def);
}

// HTTP helpers
void send_http_response(int client_sock, int code, const std::string& status, const std::string& body, const std::string& content_type = "application/json") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " " << status << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    std::string resp = oss.str();
    send(client_sock, resp.c_str(), resp.size(), 0);
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

// Parse HTTP request (very simple, not robust)
bool parse_http_request(const std::string& raw, HttpRequest& req) {
    std::istringstream iss(raw);
    std::string line;
    if(!std::getline(iss, line)) return false;
    std::istringstream lss(line);
    if(!(lss >> req.method)) return false;
    std::string path_query;
    if(!(lss >> path_query)) return false;
    auto qpos = path_query.find('?');
    if(qpos != std::string::npos) {
        req.path = path_query.substr(0, qpos);
        req.query = path_query.substr(qpos+1);
    } else {
        req.path = path_query;
        req.query = "";
    }
    // Headers
    while(std::getline(iss, line) && line != "\r") {
        auto cpos = line.find(':');
        if(cpos != std::string::npos) {
            std::string k = line.substr(0, cpos);
            std::string v = line.substr(cpos+1);
            k.erase(k.find_last_not_of(" \t\r")+1);
            v.erase(0, v.find_first_not_of(" \t"));
            v.erase(v.find_last_not_of("\r")+1);
            req.headers[k] = v;
        }
    }
    // Body
    if(std::getline(iss, line)) {
        req.body = line;
        while(std::getline(iss, line)) req.body += "\n"+line;
    }
    return true;
}

// Query string parsing
std::map<std::string, std::string> parse_query(const std::string& q) {
    std::map<std::string, std::string> out;
    std::istringstream iss(q);
    std::string kv;
    while(std::getline(iss, kv, '&')) {
        auto eq = kv.find('=');
        if(eq != std::string::npos)
            out[kv.substr(0,eq)] = kv.substr(eq+1);
    }
    return out;
}

// Session token handling
bool require_auth(const HttpRequest& req, std::string& out_err) {
    auto it = req.headers.find("Authorization");
    if(it == req.headers.end()) {
        out_err = "Authorization header required";
        return false;
    }
    std::string val = it->second;
    if(val.find("Bearer ") != 0) {
        out_err = "Malformed auth header";
        return false;
    }
    std::string token = val.substr(7);
    if(token != g_device_session.session_token || !g_device_session.logged_in) {
        out_err = "Invalid session token";
        return false;
    }
    return true;
}

// Route Handlers

void handle_login(int client_sock, const HttpRequest& req) {
    Json::Value resp;
    Json::Value jbody;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream ibody(req.body);
    bool parsed = Json::parseFromStream(builder, ibody, &jbody, &errs);
    if(!parsed) {
        resp["error"] = "Malformed JSON";
        send_http_response(client_sock, 400, "Bad Request", resp.toStyledString());
        return;
    }
    std::string username = jbody.get("username", "").asString();
    std::string password = jbody.get("password", "").asString();
    std::string ip = getenv_or("DEVICE_IP", "192.168.1.64");
    int port = std::stoi(getenv_or("DEVICE_PORT", "8000"));
    if(username.empty() || password.empty()) {
        resp["error"] = "Missing credentials";
        send_http_response(client_sock, 400, "Bad Request", resp.toStyledString());
        return;
    }
    if(!SDK_Login(ip, port, username, password)) {
        resp["error"] = "Login failed";
        send_http_response(client_sock, 401, "Unauthorized", resp.toStyledString());
        return;
    }
    resp["session_token"] = g_device_session.session_token;
    send_http_response(client_sock, 200, "OK", resp.toStyledString());
}

void handle_logout(int client_sock, const HttpRequest& req) {
    Json::Value resp;
    std::string auth_err;
    if(!require_auth(req, auth_err)) {
        resp["error"] = auth_err;
        send_http_response(client_sock, 401, "Unauthorized", resp.toStyledString());
        return;
    }
    SDK_Logout();
    resp["result"] = "logged out";
    send_http_response(client_sock, 200, "OK", resp.toStyledString());
}

void handle_get_status(int client_sock, const HttpRequest& req) {
    Json::Value resp;
    std::string auth_err;
    if(!require_auth(req, auth_err)) {
        resp["error"] = auth_err;
        send_http_response(client_sock, 401, "Unauthorized", resp.toStyledString());
        return;
    }
    Json::Value stat;
    if(!SDK_GetStatus(stat)) {
        resp["error"] = "Failed to get status";
        send_http_response(client_sock, 500, "Internal Server Error", resp.toStyledString());
        return;
    }
    send_http_response(client_sock, 200, "OK", stat.toStyledString());
}

void handle_get_config(int client_sock, const HttpRequest& req) {
    Json::Value resp;
    std::string auth_err;
    if(!require_auth(req, auth_err)) {
        resp["error"] = auth_err;
        send_http_response(client_sock, 401, "Unauthorized", resp.toStyledString());
        return;
    }
    auto qs = parse_query(req.query);
    std::string type = qs.count("type") ? qs["type"] : "";
    Json::Value cfg;
    if(!SDK_GetConfig(type, cfg)) {
        resp["error"] = "Failed to get config";
        send_http_response(client_sock, 500, "Internal Server Error", resp.toStyledString());
        return;
    }
    send_http_response(client_sock, 200, "OK", cfg.toStyledString());
}

void handle_put_config(int client_sock, const HttpRequest& req) {
    Json::Value resp;
    std::string auth_err;
    if(!require_auth(req, auth_err)) {
        resp["error"] = auth_err;
        send_http_response(client_sock, 401, "Unauthorized", resp.toStyledString());
        return;
    }
    auto qs = parse_query(req.query);
    std::string type = qs.count("type") ? qs["type"] : "";
    Json::Value jbody;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream ibody(req.body);
    bool parsed = Json::parseFromStream(builder, ibody, &jbody, &errs);
    if(!parsed) {
        resp["error"] = "Malformed JSON";
        send_http_response(client_sock, 400, "Bad Request", resp.toStyledString());
        return;
    }
    if(!SDK_SetConfig(type, jbody)) {
        resp["error"] = "Failed to set config";
        send_http_response(client_sock, 500, "Internal Server Error", resp.toStyledString());
        return;
    }
    resp["result"] = "ok";
    send_http_response(client_sock, 200, "OK", resp.toStyledString());
}

void handle_decode(int client_sock, const HttpRequest& req) {
    Json::Value resp;
    std::string auth_err;
    if(!require_auth(req, auth_err)) {
        resp["error"] = auth_err;
        send_http_response(client_sock, 401, "Unauthorized", resp.toStyledString());
        return;
    }
    Json::Value jbody;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream ibody(req.body);
    bool parsed = Json::parseFromStream(builder, ibody, &jbody, &errs);
    if(!parsed) {
        resp["error"] = "Malformed JSON";
        send_http_response(client_sock, 400, "Bad Request", resp.toStyledString());
        return;
    }
    std::string action = jbody.get("action", "").asString();
    std::string mode = jbody.get("mode", "").asString();
    if(action.empty() || mode.empty()) {
        resp["error"] = "Missing 'action' or 'mode'";
        send_http_response(client_sock, 400, "Bad Request", resp.toStyledString());
        return;
    }
    if(!SDK_DecodeControl(action, mode)) {
        resp["error"] = "Decode control failed";
        send_http_response(client_sock, 500, "Internal Server Error", resp.toStyledString());
        return;
    }
    resp["result"] = "ok";
    send_http_response(client_sock, 200, "OK", resp.toStyledString());
}

void handle_command_decode(int client_sock, const HttpRequest& req) {
    // Alias for handle_decode (could be extended)
    handle_decode(client_sock, req);
}

void handle_reboot(int client_sock, const HttpRequest& req) {
    Json::Value resp;
    std::string auth_err;
    if(!require_auth(req, auth_err)) {
        resp["error"] = auth_err;
        send_http_response(client_sock, 401, "Unauthorized", resp.toStyledString());
        return;
    }
    Json::Value jbody;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream ibody(req.body);
    bool parsed = Json::parseFromStream(builder, ibody, &jbody, &errs);
    int delay = jbody.get("delay", 0).asInt();
    bool shutdown = jbody.get("shutdown", false).asBool();
    if(!SDK_Reboot(delay, shutdown)) {
        resp["error"] = "Reboot/Shutdown failed";
        send_http_response(client_sock, 500, "Internal Server Error", resp.toStyledString());
        return;
    }
    resp["result"] = shutdown ? "shutdown initiated" : "reboot initiated";
    send_http_response(client_sock, 200, "OK", resp.toStyledString());
}

void handle_command_reboot(int client_sock, const HttpRequest& req) {
    handle_reboot(client_sock, req);
}

void handle_upgrade(int client_sock, const HttpRequest& req) {
    Json::Value resp;
    std::string auth_err;
    if(!require_auth(req, auth_err)) {
        resp["error"] = auth_err;
        send_http_response(client_sock, 401, "Unauthorized", resp.toStyledString());
        return;
    }
    Json::Value jbody;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream ibody(req.body);
    bool parsed = Json::parseFromStream(builder, ibody, &jbody, &errs);
    if(!parsed) {
        resp["error"] = "Malformed JSON";
        send_http_response(client_sock, 400, "Bad Request", resp.toStyledString());
        return;
    }
    if(!SDK_Upgrade(jbody)) {
        resp["error"] = "Upgrade failed";
        send_http_response(client_sock, 500, "Internal Server Error", resp.toStyledString());
        return;
    }
    resp["result"] = "upgrade started";
    send_http_response(client_sock, 200, "OK", resp.toStyledString());
}

void handle_command_upgrade(int client_sock, const HttpRequest& req) {
    handle_upgrade(client_sock, req);
}

// Simple router
void handle_request(int client_sock, const HttpRequest& req) {
    // All endpoints are case-sensitive
    if(req.method == "POST" && req.path == "/login") {
        handle_login(client_sock, req);
    } else if(req.method == "POST" && req.path == "/logout") {
        handle_logout(client_sock, req);
    } else if(req.method == "GET" && req.path == "/status") {
        handle_get_status(client_sock, req);
    } else if(req.method == "GET" && req.path == "/config") {
        handle_get_config(client_sock, req);
    } else if(req.method == "PUT" && req.path == "/config") {
        handle_put_config(client_sock, req);
    } else if(req.method == "POST" && req.path == "/decode") {
        handle_decode(client_sock, req);
    } else if(req.method == "POST" && req.path == "/command/decode") {
        handle_command_decode(client_sock, req);
    } else if(req.method == "POST" && req.path == "/reboot") {
        handle_reboot(client_sock, req);
    } else if(req.method == "POST" && req.path == "/command/reboot") {
        handle_command_reboot(client_sock, req);
    } else if(req.method == "POST" && req.path == "/upgrade") {
        handle_upgrade(client_sock, req);
    } else if(req.method == "POST" && req.path == "/command/upgrade") {
        handle_command_upgrade(client_sock, req);
    } else {
        Json::Value resp;
        resp["error"] = "Not found";
        send_http_response(client_sock, 404, "Not Found", resp.toStyledString());
    }
}

// Threaded client handler
void client_thread(int client_sock) {
    char buf[8192];
    int n = recv(client_sock, buf, sizeof(buf)-1, 0);
    if(n <= 0) {
        close(client_sock);
        return;
    }
    buf[n] = 0;
    HttpRequest req;
    if(!parse_http_request(buf, req)) {
        Json::Value resp; resp["error"] = "Malformed HTTP request";
        send_http_response(client_sock, 400, "Bad Request", resp.toStyledString());
        close(client_sock);
        return;
    }
    handle_request(client_sock, req);
    close(client_sock);
}

// Main server loop
int main() {
    std::string http_host = getenv_or("HTTP_SERVER_HOST", "0.0.0.0");
    int http_port = std::stoi(getenv_or("HTTP_SERVER_PORT", "8080"));

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(http_port);

    if(bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        close(server_fd);
        return 1;
    }
    if(listen(server_fd, 8) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return 1;
    }
    std::cout << "HTTP server listening on port " << http_port << std::endl;

    while(true) {
        sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int client_sock = accept(server_fd, (sockaddr*)&caddr, &clen);
        if(client_sock < 0) continue;
        std::thread(client_thread, client_sock).detach();
    }
    close(server_fd);
    return 0;
}