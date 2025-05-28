#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <json/json.h>

// ========== ENVIRONMENT VARIABLE HELPERS ==========
std::string get_env(const std::string &var, const std::string &def = "") {
    const char* val = std::getenv(var.c_str());
    return val ? std::string(val) : def;
}

// ========== MOCK HCNetSDK API & DATA STRUCTS ==========

// Replace these with actual SDK includes and calls in production
struct DeviceInfo {
    std::string name;
    std::string model;
    std::string manufacturer;
    std::string type;
};

struct DisplayConfig {
    std::string mode;
    int sceneId;
    // ...other display/scene parameters
};

struct ChannelConfig {
    int channelId;
    std::string streamType;
    std::string decoderStatus;
    // ...other channel parameters
};

struct DeviceStatus {
    std::string sdkState;
    std::string decoderStatus;
    int errorCode;
    std::string alarmStatus;
};

std::mutex sdk_mutex;
bool sdk_initialized = false;
bool sdk_logged_in = false;
std::string sdk_session_token;
DeviceInfo g_device_info = {"Decoder TV Wall", "DS-64XXHD-S", "Hikvision", "Network Video Decoder, TV Wall"};
DisplayConfig g_display_config = {"active", 1};
std::vector<ChannelConfig> g_channels = { {1,"main","ok"}, {2,"sub","ok"} };
DeviceStatus g_device_status = {"online", "ok", 0, "none"};

int SDK_Init() {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    sdk_initialized = true;
    return 0;
}
int SDK_Cleanup() {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    sdk_initialized = false;
    return 0;
}
int SDK_Login(const std::string &user, const std::string &pass, const std::string &ip, int port) {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    if (user == get_env("DEVICE_USER") && pass == get_env("DEVICE_PASS"))
    {
        sdk_logged_in = true;
        sdk_session_token = "session_token_example";
        return 0;
    }
    return -1;
}
int SDK_Logout() {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    sdk_logged_in = false;
    sdk_session_token.clear();
    return 0;
}
DeviceInfo SDK_GetDeviceInfo() {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    return g_device_info;
}
DisplayConfig SDK_GetDisplayConfig() {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    return g_display_config;
}
void SDK_SetDisplayConfig(const DisplayConfig &cfg) {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    g_display_config = cfg;
}
std::vector<ChannelConfig> SDK_GetChannels() {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    return g_channels;
}
void SDK_SetChannels(const std::vector<ChannelConfig> &cfgs) {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    g_channels = cfgs;
}
DeviceStatus SDK_GetStatus() {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    return g_device_status;
}
void SDK_Reboot() {
    std::lock_guard<std::mutex> lock(sdk_mutex);
    g_device_status.sdkState = "rebooting";
    // Simulate reboot async
    std::thread([](){
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::lock_guard<std::mutex> lock(sdk_mutex);
        g_device_status.sdkState = "online";
    }).detach();
}

// ========== HTTP SERVER IMPLEMENTATION ==========

#define BUFFER_SIZE 8192

struct HttpRequest {
    std::string method;
    std::string path;
    std::string http_version;
    std::map<std::string, std::string> headers;
    std::string body;
    std::map<std::string,std::string> query_params;
};

struct HttpResponse {
    int status_code;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;
};

void send_response(int client_sock, const HttpResponse &resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_text << "\r\n";
    for (const auto& kv : resp.headers)
        oss << kv.first << ": " << kv.second << "\r\n";
    oss << "\r\n" << resp.body;
    std::string response = oss.str();
    send(client_sock, response.c_str(), response.size(), 0);
}

std::string url_decode(const std::string &src) {
    std::string ret;
    char ch;
    int i, ii;
    for (i=0; i<src.length(); i++) {
        if (src[i] == '%') {
            sscanf(src.substr(i+1,2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            ret += ch;
            i = i+2;
        } else if (src[i] == '+') {
            ret += ' ';
        } else {
            ret += src[i];
        }
    }
    return ret;
}

std::map<std::string, std::string> parse_query(const std::string &query) {
    std::map<std::string,std::string> params;
    std::istringstream ss(query);
    std::string token;
    while (getline(ss, token, '&')) {
        auto pos = token.find('=');
        if (pos != std::string::npos) {
            params[url_decode(token.substr(0,pos))] = url_decode(token.substr(pos+1));
        }
    }
    return params;
}

bool parse_http_request(const std::string &req_str, HttpRequest &req) {
    std::istringstream ss(req_str);
    std::string line;
    // Parse request line
    if (!std::getline(ss, line)) return false;
    std::istringstream ls(line);
    ls >> req.method >> req.path >> req.http_version;
    // Parse query params
    auto qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        req.query_params = parse_query(req.path.substr(qpos+1));
        req.path = req.path.substr(0,qpos);
    }
    // Parse headers
    while (std::getline(ss, line) && line != "\r") {
        if (line.back() == '\r') line.pop_back();
        auto cpos = line.find(':');
        if (cpos != std::string::npos) {
            std::string key = line.substr(0,cpos);
            std::string value = line.substr(cpos+1);
            while (!value.empty() && value[0] == ' ') value.erase(0,1);
            req.headers[key] = value;
        }
    }
    // Parse body (if any)
    if (req.headers.count("Content-Length")) {
        int len = std::stoi(req.headers["Content-Length"]);
        std::string body(len, '\0');
        ss.read(&body[0], len);
        req.body = body;
    }
    return true;
}

void json_error_response(HttpResponse &resp, int code, const std::string &msg) {
    resp.status_code = code;
    resp.status_text = "Error";
    resp.headers["Content-Type"] = "application/json";
    Json::Value root;
    root["error"] = msg;
    resp.body = root.toStyledString();
}

// ========== AUTHENTICATION (Simple Session Token) ==========
std::mutex session_mutex;
std::string valid_session_token;
bool check_auth(const HttpRequest &req) {
    auto it = req.headers.find("Authorization");
    if (it != req.headers.end()) {
        std::lock_guard<std::mutex> lock(session_mutex);
        return it->second == ("Bearer " + valid_session_token);
    }
    return false;
}

// ========== API ENDPOINT HANDLERS ==========

void handle_login(const HttpRequest &req, HttpResponse &resp) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(req.body, root)) {
        json_error_response(resp, 400, "Invalid JSON");
        return;
    }
    std::string user = root.get("username","").asString();
    std::string pass = root.get("password","").asString();
    std::string ip = get_env("DEVICE_IP", "127.0.0.1");
    int port = std::stoi(get_env("DEVICE_PORT","8000"));
    if (SDK_Login(user, pass, ip, port) == 0) {
        std::lock_guard<std::mutex> lock(session_mutex);
        valid_session_token = sdk_session_token;
        resp.status_code = 200;
        resp.status_text = "OK";
        resp.headers["Content-Type"] = "application/json";
        Json::Value out;
        out["token"] = valid_session_token;
        resp.body = out.toStyledString();
    } else {
        json_error_response(resp, 401, "Unauthorized");
    }
}

void handle_get_device(const HttpRequest &req, HttpResponse &resp) {
    if (!check_auth(req)) { json_error_response(resp, 401, "Unauthorized"); return; }
    DeviceInfo info = SDK_GetDeviceInfo();
    Json::Value root;
    root["device_name"] = info.name;
    root["device_model"] = info.model;
    root["manufacturer"] = info.manufacturer;
    root["device_type"] = info.type;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Content-Type"] = "application/json";
    resp.body = root.toStyledString();
}

void handle_get_display(const HttpRequest &req, HttpResponse &resp) {
    if (!check_auth(req)) { json_error_response(resp, 401, "Unauthorized"); return; }
    DisplayConfig cfg = SDK_GetDisplayConfig();
    Json::Value root;
    root["mode"] = cfg.mode;
    root["scene_id"] = cfg.sceneId;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Content-Type"] = "application/json";
    resp.body = root.toStyledString();
}

void handle_put_display(const HttpRequest &req, HttpResponse &resp) {
    if (!check_auth(req)) { json_error_response(resp, 401, "Unauthorized"); return; }
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(req.body, root)) {
        json_error_response(resp, 400, "Invalid JSON");
        return;
    }
    DisplayConfig cfg;
    cfg.mode = root.get("mode",g_display_config.mode).asString();
    cfg.sceneId = root.get("scene_id",g_display_config.sceneId).asInt();
    SDK_SetDisplayConfig(cfg);
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Content-Type"] = "application/json";
    Json::Value out;
    out["result"] = "success";
    resp.body = out.toStyledString();
}

void handle_get_channels(const HttpRequest &req, HttpResponse &resp) {
    if (!check_auth(req)) { json_error_response(resp, 401, "Unauthorized"); return; }
    std::vector<ChannelConfig> chs = SDK_GetChannels();
    Json::Value root(Json::arrayValue);
    for (const auto& ch : chs) {
        Json::Value jch;
        jch["channel_id"] = ch.channelId;
        jch["stream_type"] = ch.streamType;
        jch["decoder_status"] = ch.decoderStatus;
        root.append(jch);
    }
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Content-Type"] = "application/json";
    resp.body = root.toStyledString();
}

void handle_put_channels(const HttpRequest &req, HttpResponse &resp) {
    if (!check_auth(req)) { json_error_response(resp, 401, "Unauthorized"); return; }
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(req.body, root)) {
        json_error_response(resp, 400, "Invalid JSON");
        return;
    }
    if (!root.isArray()) { json_error_response(resp, 400, "Expected array of channels"); return; }
    std::vector<ChannelConfig> newchs;
    for (const auto &ch : root) {
        ChannelConfig c;
        c.channelId = ch.get("channel_id",0).asInt();
        c.streamType = ch.get("stream_type","main").asString();
        c.decoderStatus = ch.get("decoder_status","ok").asString();
        newchs.push_back(c);
    }
    SDK_SetChannels(newchs);
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Content-Type"] = "application/json";
    Json::Value out;
    out["result"] = "success";
    resp.body = out.toStyledString();
}

void handle_get_status(const HttpRequest &req, HttpResponse &resp) {
    if (!check_auth(req)) { json_error_response(resp, 401, "Unauthorized"); return; }
    DeviceStatus st = SDK_GetStatus();
    Json::Value root;
    root["sdk_state"] = st.sdkState;
    root["decoder_status"] = st.decoderStatus;
    root["error_code"] = st.errorCode;
    root["alarm_status"] = st.alarmStatus;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Content-Type"] = "application/json";
    resp.body = root.toStyledString();
}

void handle_reboot(const HttpRequest &req, HttpResponse &resp) {
    if (!check_auth(req)) { json_error_response(resp, 401, "Unauthorized"); return; }
    SDK_Reboot();
    resp.status_code = 202;
    resp.status_text = "Accepted";
    resp.headers["Content-Type"] = "application/json";
    Json::Value out;
    out["result"] = "rebooting";
    resp.body = out.toStyledString();
}

// ========== ROUTER ==========
typedef void(*Handler)(const HttpRequest&, HttpResponse&);

struct Route {
    std::string method;
    std::string path;
    Handler handler;
};

std::vector<Route> routes = {
    {"POST", "/auth/login", handle_login},
    {"GET",  "/device",     handle_get_device},
    {"GET",  "/display",    handle_get_display},
    {"PUT",  "/display",    handle_put_display},
    {"GET",  "/channels",   handle_get_channels},
    {"PUT",  "/channels",   handle_put_channels},
    {"GET",  "/status",     handle_get_status},
    {"POST", "/cmd/reboot", handle_reboot}
};

Handler match_route(const std::string &method, const std::string &path) {
    for (const auto& r : routes) {
        if (r.method == method && r.path == path) return r.handler;
    }
    return nullptr;
}

// ========== SERVER MAIN LOOP ==========

void client_thread(int client_sock) {
    char buffer[BUFFER_SIZE];
    ssize_t recvd = recv(client_sock, buffer, BUFFER_SIZE-1, 0);
    if (recvd <= 0) { close(client_sock); return; }
    buffer[recvd] = '\0';
    HttpRequest req;
    if (!parse_http_request(buffer, req)) {
        HttpResponse resp;
        json_error_response(resp, 400, "Bad Request");
        send_response(client_sock, resp);
        close(client_sock); return;
    }
    HttpResponse resp;
    Handler h = match_route(req.method, req.path);
    if (h) {
        h(req, resp);
    } else {
        resp.status_code = 404;
        resp.status_text = "Not Found";
        resp.headers["Content-Type"] = "application/json";
        Json::Value j;
        j["error"] = "Endpoint not found";
        resp.body = j.toStyledString();
    }
    send_response(client_sock, resp);
    close(client_sock);
}

int main() {
    std::string host = get_env("HTTP_HOST", "0.0.0.0");
    int port = std::stoi(get_env("HTTP_PORT", "8080"));
    SDK_Init();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "Socket error\n"; return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(host.c_str());
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed\n"; return 1;
    }
    if (listen(server_fd, 16) < 0) {
        std::cerr << "Listen failed\n"; return 1;
    }
    std::cout << "HTTP server running on " << host << ":" << port << std::endl;
    while (true) {
        int client_sock = accept(server_fd, NULL, NULL);
        if (client_sock < 0) continue;
        std::thread(client_thread, client_sock).detach();
    }
    SDK_Cleanup();
    close(server_fd);
    return 0;
}