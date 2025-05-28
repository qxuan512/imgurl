#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <ctime>
#include <cstring>
#include <json/json.h>
#include <microhttpd.h>

extern "C" {
// Placeholders for HCNetSDK functions and types
typedef int LONG;
typedef struct { int dummy; } NET_DVR_DEVICEINFO_V30;
LONG NET_DVR_Init() { return 1; }
void NET_DVR_Cleanup() {}
LONG NET_DVR_Login_V30(const char* ip, int port, const char* user, const char* pwd, NET_DVR_DEVICEINFO_V30* devinfo) { return 1; }
LONG NET_DVR_Logout(LONG userID) { return 1; }
int NET_DVR_ActivateDevice(const char* ip, int port, const char* pass) { return 1; }
int NET_DVR_RebootDVR(LONG userID) { return 1; }
int NET_DVR_GetDeviceStatus(LONG userID, std::string& outStatus) { outStatus = "{\"device\":\"ok\",\"channels\":[{\"id\":1,\"state\":\"playing\"}]}"; return 1; }
int NET_DVR_RemotePlaybackControl(LONG userID, const std::string& cmd, Json::Value& params, std::string& outResult) { outResult = "{\"playback\":\"success\"}"; return 1; }
int NET_DVR_SetDisplayConfig(LONG userID, Json::Value& config, std::string& outResult) { outResult = "{\"config\":\"updated\"}"; return 1; }
}

#define PORT_ENV "HTTP_PORT"
#define DEVICE_IP_ENV "DEVICE_IP"
#define DEVICE_PORT_ENV "DEVICE_PORT"
#define DEVICE_USER_ENV "DEVICE_USER"
#define DEVICE_PASS_ENV "DEVICE_PASS"
#define SDK_ACTIVATE_PASS_ENV "SDK_ACTIVATE_PASS"

// Global session manager
struct Session {
    LONG userID;
    std::string token;
    std::time_t expiry;
};
std::mutex g_sessions_mutex;
std::map<std::string, Session> g_sessions;

static std::string generate_token() {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    int len = 32;
    std::string token;
    for (int i = 0; i < len; ++i)
        token += alphanum[rand() % (sizeof(alphanum) - 1)];
    return token;
}

static bool validate_token(const std::string& token, LONG& userID) {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto it = g_sessions.find(token);
    if (it == g_sessions.end()) return false;
    if (std::time(nullptr) > it->second.expiry) {
        g_sessions.erase(it);
        return false;
    }
    userID = it->second.userID;
    return true;
}

static void invalidate_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    g_sessions.erase(token);
}

static int send_response(struct MHD_Connection* connection, int status_code, const std::string& body, const char* content_type = "application/json") {
    struct MHD_Response* response = MHD_create_response_from_buffer(body.size(), (void*)body.c_str(), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", content_type);
    int ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

static bool parse_json_post(struct MHD_Connection* connection, std::string& out) {
    const char* length_str = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Length");
    if (!length_str) return false;
    long len = std::strtol(length_str, nullptr, 10);
    if (len <= 0 || len > 1024*1024) return false;
    char* buf = new char[len+1];
    int ret = MHD_get_connection_values(connection, MHD_POSTDATA_KIND, nullptr, nullptr); // force libmicrohttpd to make POST available
    int pos = 0;
    const char* data;
    size_t size;
    MHD_get_connection_values(connection, MHD_POSTDATA_KIND, [](void* cls, enum MHD_ValueKind kind, const char* key, const char* value) {
        std::string* out = (std::string*)cls;
        out->assign(value);
        return MHD_YES;
    }, &out);
    delete[] buf;
    return true;
}

// Utility: Get POST body
static std::string get_post_data(struct MHD_Connection* connection) {
    char* data = nullptr;
    size_t size = 0;
    MHD_get_connection_values(connection, MHD_POSTDATA_KIND, [](void* cls, enum MHD_ValueKind kind, const char* key, const char* value) {
        std::string* str = (std::string*)cls;
        if (value) str->assign(value);
        return MHD_YES;
    }, &data);
    std::string ret;
    if (data)
        ret = std::string(data);
    return ret;
}

// HTTP handler
static int request_handler(void* cls, struct MHD_Connection* connection, const char* url,
                          const char* method, const char* version,
                          const char* upload_data, size_t* upload_data_size, void** ptr) {
    static int dummy;
    if (&dummy != *ptr) {
        *ptr = &dummy;
        return MHD_YES;
    }
    Json::Value resp;
    std::string token, post_data;
    LONG userID = 0;
    int status_code = 200;

    if (strcmp(url, "/sessions") == 0 && strcmp(method, "POST") == 0) {
        post_data = get_post_data(connection);
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(post_data, root)) {
            resp["error"] = "Invalid JSON";
            return send_response(connection, 400, resp.toStyledString());
        }
        std::string ip = getenv(DEVICE_IP_ENV) ? getenv(DEVICE_IP_ENV) : "";
        int port = getenv(DEVICE_PORT_ENV) ? atoi(getenv(DEVICE_PORT_ENV)) : 8000;
        std::string user = root.get("username", "").asString();
        std::string pass = root.get("password", "").asString();
        if (ip.empty() || user.empty() || pass.empty()) {
            resp["error"] = "Missing credentials";
            return send_response(connection, 400, resp.toStyledString());
        }
        NET_DVR_DEVICEINFO_V30 devinfo;
        LONG uid = NET_DVR_Login_V30(ip.c_str(), port, user.c_str(), pass.c_str(), &devinfo);
        if (uid <= 0) {
            resp["error"] = "Login failed";
            return send_response(connection, 401, resp.toStyledString());
        }
        std::string new_token = generate_token();
        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            g_sessions[new_token] = {uid, new_token, std::time(nullptr) + 3600};
        }
        resp["token"] = new_token;
        return send_response(connection, 200, resp.toStyledString());
    }
    if (strcmp(url, "/sessions") == 0 && strcmp(method, "DELETE") == 0) {
        const char* auth = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
        if (!auth) {
            resp["error"] = "Missing token";
            return send_response(connection, 401, resp.toStyledString());
        }
        token = std::string(auth);
        invalidate_token(token);
        resp["result"] = "Logged out";
        return send_response(connection, 200, resp.toStyledString());
    }
    if (strcmp(url, "/commands/activate") == 0 && strcmp(method, "POST") == 0) {
        std::string ip = getenv(DEVICE_IP_ENV) ? getenv(DEVICE_IP_ENV) : "";
        int port = getenv(DEVICE_PORT_ENV) ? atoi(getenv(DEVICE_PORT_ENV)) : 8000;
        std::string pass = getenv(SDK_ACTIVATE_PASS_ENV) ? getenv(SDK_ACTIVATE_PASS_ENV) : "";
        if (ip.empty() || pass.empty()) {
            resp["error"] = "Missing device IP or activation password";
            return send_response(connection, 400, resp.toStyledString());
        }
        int ret = NET_DVR_ActivateDevice(ip.c_str(), port, pass.c_str());
        if (!ret) {
            resp["error"] = "Activation failed";
            return send_response(connection, 500, resp.toStyledString());
        }
        resp["result"] = "Device activated";
        return send_response(connection, 200, resp.toStyledString());
    }
    if (strcmp(url, "/status") == 0 && strcmp(method, "GET") == 0) {
        const char* auth = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
        if (!auth) {
            resp["error"] = "Missing token";
            return send_response(connection, 401, resp.toStyledString());
        }
        token = std::string(auth);
        if (!validate_token(token, userID)) {
            resp["error"] = "Invalid token";
            return send_response(connection, 403, resp.toStyledString());
        }
        std::string outStatus;
        int ret = NET_DVR_GetDeviceStatus(userID, outStatus);
        if (!ret) {
            resp["error"] = "Failed to get status";
            return send_response(connection, 500, resp.toStyledString());
        }
        return send_response(connection, 200, outStatus);
    }
    if (strcmp(url, "/commands/playback") == 0 && strcmp(method, "POST") == 0) {
        const char* auth = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
        if (!auth) {
            resp["error"] = "Missing token";
            return send_response(connection, 401, resp.toStyledString());
        }
        token = std::string(auth);
        if (!validate_token(token, userID)) {
            resp["error"] = "Invalid token";
            return send_response(connection, 403, resp.toStyledString());
        }
        post_data = get_post_data(connection);
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(post_data, root)) {
            resp["error"] = "Invalid JSON";
            return send_response(connection, 400, resp.toStyledString());
        }
        std::string cmd = root.get("command", "").asString();
        if (cmd.empty()) {
            resp["error"] = "Missing command";
            return send_response(connection, 400, resp.toStyledString());
        }
        std::string outResult;
        int ret = NET_DVR_RemotePlaybackControl(userID, cmd, root, outResult);
        if (!ret) {
            resp["error"] = "Playback operation failed";
            return send_response(connection, 500, resp.toStyledString());
        }
        return send_response(connection, 200, outResult);
    }
    if (strcmp(url, "/commands/reboot") == 0 && strcmp(method, "POST") == 0) {
        const char* auth = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
        if (!auth) {
            resp["error"] = "Missing token";
            return send_response(connection, 401, resp.toStyledString());
        }
        token = std::string(auth);
        if (!validate_token(token, userID)) {
            resp["error"] = "Invalid token";
            return send_response(connection, 403, resp.toStyledString());
        }
        int ret = NET_DVR_RebootDVR(userID);
        if (!ret) {
            resp["error"] = "Reboot failed";
            return send_response(connection, 500, resp.toStyledString());
        }
        resp["result"] = "Device rebooted";
        return send_response(connection, 200, resp.toStyledString());
    }
    if (strcmp(url, "/config/display") == 0 && strcmp(method, "PUT") == 0) {
        const char* auth = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
        if (!auth) {
            resp["error"] = "Missing token";
            return send_response(connection, 401, resp.toStyledString());
        }
        token = std::string(auth);
        if (!validate_token(token, userID)) {
            resp["error"] = "Invalid token";
            return send_response(connection, 403, resp.toStyledString());
        }
        post_data = get_post_data(connection);
        Json::Value config;
        Json::Reader reader;
        if (!reader.parse(post_data, config)) {
            resp["error"] = "Invalid JSON";
            return send_response(connection, 400, resp.toStyledString());
        }
        std::string outResult;
        int ret = NET_DVR_SetDisplayConfig(userID, config, outResult);
        if (!ret) {
            resp["error"] = "Display config update failed";
            return send_response(connection, 500, resp.toStyledString());
        }
        return send_response(connection, 200, outResult);
    }
    resp["error"] = "Not found";
    return send_response(connection, 404, resp.toStyledString());
}

int main(int argc, char** argv) {
    const char* portenv = getenv(PORT_ENV);
    int port = portenv ? atoi(portenv) : 8080;
    NET_DVR_Init();
    struct MHD_Daemon* daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, port, NULL, NULL,
                                                 &request_handler, NULL, MHD_OPTION_END);
    if (!daemon) {
        std::cerr << "Failed to start HTTP server\n";
        return 1;
    }
    std::cout << "HTTP server started on port " << port << std::endl;
    getchar();
    MHD_stop_daemon(daemon);
    NET_DVR_Cleanup();
    return 0;
}