```cpp
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <vector>
#include <ctime>
#include <json/json.h>
#include "mongoose.h"

// HCNetSDK C API header (must be present in include path)
#include "HCNetSDK.h"

// --------------------------
// Environment Variables Util
// --------------------------
std::string getenv_str(const char* name, const char* def = "") {
    const char* val = std::getenv(name);
    return val ? val : def;
}

// --------------------------
// Device Session Management
// --------------------------
struct DeviceSession {
    LONG user_id;
    std::string session_token;
    std::time_t expires_at;
};

std::mutex g_session_mutex;
DeviceSession g_session = { -1, "", 0 };

std::string gen_token() {
    std::ostringstream oss;
    oss << std::hex << std::time(nullptr) << rand();
    return oss.str();
}

// --------------------------
// HCNetSDK RAII Helper
// --------------------------
class HCNetSDKGuard {
public:
    HCNetSDKGuard()  { NET_DVR_Init(); }
    ~HCNetSDKGuard() { NET_DVR_Cleanup(); }
};

// --------------------------
// JSON Helpers
// --------------------------
void send_json(struct mg_connection *nc, int status, const Json::Value& root) {
    std::string body = root.toStyledString();
    mg_printf(nc,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        status,
        (status == 200 ? "OK" : (status == 401 ? "Unauthorized" : (status == 400 ? "Bad Request" : "Error"))),
        body.size(), body.c_str()
    );
}

void send_error(struct mg_connection *nc, int status, const std::string& msg) {
    Json::Value err;
    err["error"] = msg;
    send_json(nc, status, err);
}

// --------------------------
// Session Auth
// --------------------------
bool check_auth(struct http_message *hm) {
    char token[128] = "";
    mg_get_http_header(hm, "Authorization", token, sizeof(token));
    std::string auth_token(token);
    if (auth_token.rfind("Bearer ", 0) == 0) {
        auth_token = auth_token.substr(7);
    }
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (g_session.user_id < 0 || g_session.session_token != auth_token) return false;
    if (std::time(nullptr) > g_session.expires_at) return false;
    return true;
}

// --------------------------
// HCNetSDK Device Operations
// --------------------------
bool device_login(const std::string& ip, int port, const std::string& user, const std::string& pwd, LONG& user_id, std::string& err) {
    NET_DVR_USER_LOGIN_INFO loginInfo = {0};
    NET_DVR_DEVICEINFO_V40 deviceInfo = {0};
    strncpy(loginInfo.sDeviceAddress, ip.c_str(), sizeof(loginInfo.sDeviceAddress)-1);
    strncpy(loginInfo.sUserName, user.c_str(), sizeof(loginInfo.sUserName)-1);
    strncpy(loginInfo.sPassword, pwd.c_str(), sizeof(loginInfo.sPassword)-1);
    loginInfo.wPort = port;
    loginInfo.bUseAsynLogin = 0;
    user_id = NET_DVR_Login_V40(&loginInfo, &deviceInfo);
    if (user_id < 0) {
        err = "Login failed: " + std::to_string(NET_DVR_GetLastError());
        return false;
    }
    return true;
}

void device_logout(LONG user_id) {
    if (user_id >= 0) {
        NET_DVR_Logout(user_id);
    }
}

bool get_device_status(LONG user_id, Json::Value& status) {
    // Dummy implementation; fill with actual SDK calls as necessary
    status["status"] = "ok";
    status["channels"] = 4;
    status["display"] = "normal";
    status["alarm"] = false;
    status["upgrade_progress"] = 0;
    status["error_code"] = 0;
    return true;
}

bool get_device_config(LONG user_id, const std::string& type, Json::Value& config) {
    // Dummy implementation; fill with actual SDK calls as necessary
    config["type"] = type;
    config["value"] = "demo_value";
    return true;
}

bool set_device_config(LONG user_id, const std::string& type, const Json::Value& cfg, std::string& err) {
    // Dummy implementation; fill with actual SDK calls as necessary
    (void)cfg;
    if (type.empty()) {
        err = "Missing configuration type";
        return false;
    }
    return true;
}

bool device_decode(LONG user_id, const Json::Value& payload, Json::Value& resp, std::string& err) {
    // Dummy implementation; fill with actual SDK calls as necessary
    if (!payload.isMember("action") || !payload.isMember("mode")) {
        err = "Missing required fields";
        return false;
    }
    resp["result"] = "decode_" + payload["action"].asString() + "_" + payload["mode"].asString();
    return true;
}

bool device_reboot(LONG user_id, const Json::Value& payload, std::string& err) {
    // Dummy implementation; fill with actual SDK calls as necessary
    (void)payload; (void)user_id;
    // NET_DVR_RebootDVR(user_id); // if available
    return true;
}

bool device_upgrade(LONG user_id, const Json::Value& payload, std::string& err) {
    // Dummy implementation; fill with actual SDK calls as necessary
    (void)payload; (void)user_id;
    // Real upgrade would require SDK call
    return true;
}

// --------------------------
// HTTP Handler
// --------------------------
void ev_handler(struct mg_connection *nc, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_REQUEST) return;
    struct http_message *hm = (struct http_message *) ev_data;

    std::string method(hm->method.p, hm->method.len);
    std::string uri(hm->uri.p, hm->uri.len);

    // Parse Query
    std::unordered_map<std::string, std::string> query_params;
    if (hm->query_string.len > 0) {
        std::string qs(hm->query_string.p, hm->query_string.len);
        std::istringstream iss(qs);
        std::string kv;
        while (std::getline(iss, kv, '&')) {
            auto eq = kv.find('=');
            if (eq != std::string::npos) {
                std::string k = kv.substr(0, eq);
                std::string v = kv.substr(eq+1);
                query_params[k] = v;
            }
        }
    }

    // Body to JSON
    Json::Value body_json;
    if (hm->body.len > 0 && (method == "POST" || method == "PUT")) {
        std::string body(hm->body.p, hm->body.len);
        Json::Reader reader;
        if (!reader.parse(body, body_json)) {
            send_error(nc, 400, "Malformed JSON");
            return;
        }
    }

    // Session/User Management
    std::string device_ip   = getenv_str("DEVICE_IP");
    int device_port         = std::stoi(getenv_str("DEVICE_PORT", "8000"));
    std::string device_user = getenv_str("DEVICE_USER");
    std::string device_pwd  = getenv_str("DEVICE_PWD");

    // Route/Dispatch
    if (uri == "/login" && method == "POST") {
        // Authentication
        if (!body_json.isMember("username") || !body_json.isMember("password")) {
            send_error(nc, 400, "Missing username or password");
            return;
        }
        std::string username = body_json["username"].asString();
        std::string password = body_json["password"].asString();

        LONG user_id;
        std::string err;
        HCNetSDKGuard sdk_guard;
        if (!device_login(device_ip, device_port, username, password, user_id, err)) {
            send_error(nc, 401, err);
            return;
        }
        std::lock_guard<std::mutex> lock(g_session_mutex);
        g_session.user_id = user_id;
        g_session.session_token = gen_token();
        g_session.expires_at = std::time(nullptr) + 3600;
        Json::Value resp;
        resp["token"] = g_session.session_token;
        send_json(nc, 200, resp);
        return;
    }

    if (uri == "/logout" && method == "POST") {
        if (!check_auth(hm)) {
            send_error(nc, 401, "Session invalid");
            return;
        }
        std::lock_guard<std::mutex> lock(g_session_mutex);
        device_logout(g_session.user_id);
        g_session.user_id = -1;
        g_session.session_token = "";
        g_session.expires_at = 0;
        Json::Value resp;
        resp["success"] = true;
        send_json(nc, 200, resp);
        return;
    }

    if ((uri == "/status") && (method == "GET")) {
        if (!check_auth(hm)) {
            send_error(nc, 401, "Unauthorized");
            return;
        }
        Json::Value status;
        HCNetSDKGuard sdk_guard;
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            if (!get_device_status(g_session.user_id, status)) {
                send_error(nc, 500, "Failed to get status");
                return;
            }
        }
        send_json(nc, 200, status);
        return;
    }

    if ((uri == "/config") && (method == "GET")) {
        if (!check_auth(hm)) {
            send_error(nc, 401, "Unauthorized");
            return;
        }
        std::string type = "";
        auto it = query_params.find("type");
        if (it != query_params.end()) type = it->second;
        Json::Value config;
        HCNetSDKGuard sdk_guard;
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            if (!get_device_config(g_session.user_id, type, config)) {
                send_error(nc, 500, "Failed to get config");
                return;
            }
        }
        send_json(nc, 200, config);
        return;
    }

    if ((uri == "/config") && (method == "PUT")) {
        if (!check_auth(hm)) {
            send_error(nc, 401, "Unauthorized");
            return;
        }
        std::string type = "";
        auto it = query_params.find("type");
        if (it != query_params.end()) type = it->second;
        std::string err;
        HCNetSDKGuard sdk_guard;
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            if (!set_device_config(g_session.user_id, type, body_json, err)) {
                send_error(nc, 400, err);
                return;
            }
        }
        Json::Value resp;
        resp["success"] = true;
        send_json(nc, 200, resp);
        return;
    }

    if ((uri == "/decode" || uri == "/command/decode") && method == "POST") {
        if (!check_auth(hm)) {
            send_error(nc, 401, "Unauthorized");
            return;
        }
        Json::Value resp;
        std::string err;
        HCNetSDKGuard sdk_guard;
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            if (!device_decode(g_session.user_id, body_json, resp, err)) {
                send_error(nc, 400, err);
                return;
            }
        }
        send_json(nc, 200, resp);
        return;
    }

    if ((uri == "/reboot" || uri == "/command/reboot") && method == "POST") {
        if (!check_auth(hm)) {
            send_error(nc, 401, "Unauthorized");
            return;
        }
        std::string err;
        HCNetSDKGuard sdk_guard;
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            if (!device_reboot(g_session.user_id, body_json, err)) {
                send_error(nc, 400, err);
                return;
            }
        }
        Json::Value resp;
        resp["success"] = true;
        send_json(nc, 200, resp);
        return;
    }

    if ((uri == "/upgrade" || uri == "/command/upgrade") && method == "POST") {
        if (!check_auth(hm)) {
            send_error(nc, 401, "Unauthorized");
            return;
        }
        std::string err;
        HCNetSDKGuard sdk_guard;
        {
            std::lock_guard<std::mutex> lock(g_session_mutex);
            if (!device_upgrade(g_session.user_id, body_json, err)) {
                send_error(nc, 400, err);
                return;
            }
        }
        Json::Value resp;
        resp["success"] = true;
        send_json(nc, 200, resp);
        return;
    }

    // Fallback 404
    send_error(nc, 404, "Not Found");
}

// --------------------------
// Main Entrypoint
// --------------------------
int main() {
    std::string http_host = getenv_str("HTTP_HOST", "0.0.0.0");
    int http_port = std::stoi(getenv_str("HTTP_PORT", "8080"));

    std::ostringstream oss;
    oss << http_host << ":" << http_port;
    std::string http_addr = oss.str();

    struct mg_mgr mgr;
    mg_mgr_init(&mgr, NULL);
    struct mg_connection *nc = mg_bind(&mgr, http_addr.c_str(), ev_handler);

    if (!nc) {
        std::cerr << "Failed to bind to " << http_addr << std::endl;
        return 1;
    }
    mg_set_protocol_http_websocket(nc);

    std::cout << "HTTP server started on " << http_addr << std::endl;

    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }
    mg_mgr_free(&mgr);
    return 0;
}
```
