```cpp
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <sstream>
#include <fstream>
#include <vector>
#include <memory>
#include <json/json.h>
#include <microhttpd.h>
#include "HCNetSDK.h"

#define RESPONSE_BUFFER_SIZE 8192
#define MAX_SESSIONS 32

// Utility Macro for Env Var
#define GET_ENV(VAR, DEF) (std::getenv(VAR) ? std::getenv(VAR) : DEF)

// ==== Global Config ====
const char* SERVER_HOST = GET_ENV("HTTP_SERVER_HOST", "0.0.0.0");
const uint16_t SERVER_PORT = (uint16_t)atoi(GET_ENV("HTTP_SERVER_PORT", "8080"));
const char* DEVICE_IP = GET_ENV("DEVICE_IP", "192.168.1.100");
const int DEVICE_PORT = atoi(GET_ENV("DEVICE_PORT", "8000"));
const char* DEVICE_USERNAME = GET_ENV("DEVICE_USERNAME", "admin");
const char* DEVICE_PASSWORD = GET_ENV("DEVICE_PASSWORD", "12345");

// ==== Session Management ====
struct SessionInfo {
    LONG lUserID;
    std::string token;
};

std::mutex g_sessionMutex;
std::unordered_map<std::string, SessionInfo> g_sessions;

// ==== HCNetSDK Wrapper ====
class HikvisionDevice {
public:
    HikvisionDevice() : lUserID(-1), initialized(false) {
        std::lock_guard<std::mutex> lk(g_sdkMutex);
        if (!initialized) {
            NET_DVR_Init();
            initialized = true;
        }
    }

    ~HikvisionDevice() {
        logout();
    }

    bool login(const std::string& ip, int port, const std::string& user, const std::string& pwd) {
        logout();
        NET_DVR_DEVICEINFO_V40 deviceInfo = {0};
        lUserID = NET_DVR_Login_V40(ip.c_str(), port, user.c_str(), pwd.c_str(), &deviceInfo);
        return lUserID >= 0;
    }

    void logout() {
        if (lUserID >= 0) {
            NET_DVR_Logout(lUserID);
            lUserID = -1;
        }
    }

    LONG user() const { return lUserID; }

    // --- Device Operations ---

    bool getConfig(const std::string& type, Json::Value& out) {
        if (lUserID < 0) return false;

        // Only a few types are stubbed for illustration. Expand for more.
        if (type == "display") {
            // Fetch display config via XML/SDK
            char buffer[RESPONSE_BUFFER_SIZE] = {0};
            DWORD retLen = 0;
            if (NET_DVR_GetDeviceAbility(lUserID, NET_DVR_XML_CONFIG, "display", NULL, 0, buffer, sizeof(buffer), &retLen)) {
                std::string xml(buffer, retLen);
                out["type"] = "display";
                out["config_xml"] = xml;
                return true;
            }
            return false;
        } else if (type == "channel") {
            // Fetch channel config via XML/SDK
            char buffer[RESPONSE_BUFFER_SIZE] = {0};
            DWORD retLen = 0;
            if (NET_DVR_GetDeviceAbility(lUserID, NET_DVR_XML_CONFIG, "channel", NULL, 0, buffer, sizeof(buffer), &retLen)) {
                std::string xml(buffer, retLen);
                out["type"] = "channel";
                out["config_xml"] = xml;
                return true;
            }
            return false;
        }
        // else, stub
        out["type"] = type;
        out["config"] = "Not implemented";
        return true;
    }

    bool setConfig(const std::string& type, const Json::Value& in, Json::Value& out) {
        if (lUserID < 0) return false;

        // Only a few types are stubbed for illustration. Expand for more.
        if (type == "display") {
            // Would push config via NET_DVR_SetDeviceAbility or similar
            out["type"] = "display";
            out["result"] = "Display config update not implemented";
            return true;
        } else if (type == "channel") {
            out["type"] = "channel";
            out["result"] = "Channel config update not implemented";
            return true;
        }
        out["type"] = type;
        out["result"] = "Not implemented";
        return true;
    }

    bool getStatus(Json::Value& out) {
        if (lUserID < 0) return false;
        // Example: get device status via ability
        char buffer[RESPONSE_BUFFER_SIZE] = {0};
        DWORD retLen = 0;
        if (NET_DVR_GetDeviceAbility(lUserID, NET_DVR_XML_CONFIG, "status", NULL, 0, buffer, sizeof(buffer), &retLen)) {
            std::string xml(buffer, retLen);
            out["status_xml"] = xml;
            return true;
        }
        out["status"] = "Not implemented";
        return true;
    }

    bool controlDecode(const Json::Value& in, Json::Value& out) {
        // Example stub
        out["decode"] = "Decode control not implemented";
        return true;
    }

    bool upgrade(const Json::Value& in, Json::Value& out) {
        // Example stub
        out["upgrade"] = "Firmware upgrade not implemented";
        return true;
    }

    bool reboot(const Json::Value& in, Json::Value& out) {
        if (lUserID < 0) return false;
        if (NET_DVR_RebootDVR(lUserID)) {
            out["result"] = "Device rebooting";
            return true;
        }
        out["error"] = "Failed to reboot";
        return false;
    }

private:
    LONG lUserID;
    static bool initialized;
    static std::mutex g_sdkMutex;
};
bool HikvisionDevice::initialized = false;
std::mutex HikvisionDevice::g_sdkMutex;

// ==== Token Utilities ====
std::string generate_token() {
    static int counter = 0;
    return "session_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(counter++);
}

bool parse_json(const char* data, size_t size, Json::Value& out) {
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    std::istringstream iss(std::string(data, size));
    return Json::parseFromStream(rbuilder, iss, &out, &errs);
}

std::string json_to_str(const Json::Value& v) {
    Json::StreamWriterBuilder wbuilder;
    return Json::writeString(wbuilder, v);
}

// ==== HTTP Server Handler ====
struct ReqContext {
    std::string session_token;
    std::shared_ptr<HikvisionDevice> device;
};

static int send_response(struct MHD_Connection* connection, int status_code, const char* content_type, const std::string& body) {
    struct MHD_Response* response = MHD_create_response_from_buffer(body.size(), (void*)body.data(), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", content_type);
    int ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

static bool get_session(const char* token, std::shared_ptr<HikvisionDevice>& device) {
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    auto it = g_sessions.find(token ? token : "");
    if (it != g_sessions.end()) {
        if (!it->second.token.empty()) {
            device = std::make_shared<HikvisionDevice>();
            device->login(DEVICE_IP, DEVICE_PORT, DEVICE_USERNAME, DEVICE_PASSWORD);
            return true;
        }
    }
    return false;
}

// ==== HTTP Routing ====
static int api_handler(void* cls, struct MHD_Connection* connection,
                       const char* url, const char* method,
                       const char* version, const char* upload_data,
                       size_t* upload_data_size, void** con_cls) {

    static std::unordered_map<std::string, std::string> session_tokens; // For login/logout POST
    static std::unordered_map<std::string, std::shared_ptr<HikvisionDevice>> cached_devices;

    static std::string last_token;
    static std::shared_ptr<HikvisionDevice> last_device;

    // Session context for this request
    ReqContext* context = static_cast<ReqContext*>(*con_cls);

    // For new connection, initialize context
    if (!context) {
        context = new ReqContext();
        *con_cls = context;
    }

    // Only handle POST data on first call (upload_data_size > 0)
    if (*upload_data_size > 0) {
        if (context->session_token.empty())
            context->session_token.assign(upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }

    std::string path(url ? url : "");
    std::string m(method ? method : "");
    std::string response;
    int status = MHD_HTTP_OK;
    Json::Value out;

    // Extract session token from header (if any)
    const char* token_hdr = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Session-Token");
    std::string session_token = token_hdr ? token_hdr : "";

    // --------- ROUTE HANDLING ---------
    if (path == "/login" && m == "POST") {
        // Parse JSON credentials
        char* json_data = context->session_token.empty() ? nullptr : &context->session_token[0];
        Json::Value creds;
        if (!json_data || !parse_json(json_data, strlen(json_data), creds)) {
            out["error"] = "Invalid JSON";
            response = json_to_str(out);
            status = MHD_HTTP_BAD_REQUEST;
            goto END;
        }
        std::string user = creds.get("username", DEVICE_USERNAME).asString();
        std::string pwd = creds.get("password", DEVICE_PASSWORD).asString();
        auto device = std::make_shared<HikvisionDevice>();
        if (!device->login(DEVICE_IP, DEVICE_PORT, user, pwd)) {
            out["error"] = "Failed to login to device";
            response = json_to_str(out);
            status = MHD_HTTP_UNAUTHORIZED;
            goto END;
        }
        std::string token = generate_token();
        {
            std::lock_guard<std::mutex> lk(g_sessionMutex);
            if (g_sessions.size() > MAX_SESSIONS) g_sessions.clear();
            g_sessions[token] = {device->user(), token};
        }
        out["session_token"] = token;
        response = json_to_str(out);
        status = MHD_HTTP_OK;
        goto END;
    }
    if (path == "/logout" && m == "POST") {
        if (session_token.empty()) {
            out["error"] = "No session token";
            response = json_to_str(out);
            status = MHD_HTTP_UNAUTHORIZED;
            goto END;
        }
        std::lock_guard<std::mutex> lk(g_sessionMutex);
        auto it = g_sessions.find(session_token);
        if (it != g_sessions.end()) {
            g_sessions.erase(it);
            out["result"] = "Session terminated";
            response = json_to_str(out);
            status = MHD_HTTP_OK;
        } else {
            out["error"] = "Session not found";
            response = json_to_str(out);
            status = MHD_HTTP_UNAUTHORIZED;
        }
        goto END;
    }

    // All other endpoints require session
    std::shared_ptr<HikvisionDevice> device;
    if (!get_session(session_token.c_str(), device)) {
        out["error"] = "Unauthorized: Invalid or missing session token";
        response = json_to_str(out);
        status = MHD_HTTP_UNAUTHORIZED;
        goto END;
    }

    // ---- GET /status ----
    if ((path == "/status") && (m == "GET")) {
        if (!device->getStatus(out)) {
            out["error"] = "Failed to retrieve device status";
            status = MHD_HTTP_INTERNAL_SERVER_ERROR;
        }
        response = json_to_str(out);
        goto END;
    }

    // ---- GET /config ----
    if ((path == "/config") && (m == "GET")) {
        // Query param: type
        const char* type = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "type");
        std::string conf_type = type ? type : "display";
        if (!device->getConfig(conf_type, out)) {
            out["error"] = "Failed to fetch config";
            status = MHD_HTTP_INTERNAL_SERVER_ERROR;
        }
        response = json_to_str(out);
        goto END;
    }

    // ---- PUT /config ----
    if ((path == "/config") && (m == "PUT")) {
        // Query param: type
        const char* type = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "type");
        std::string conf_type = type ? type : "display";
        // JSON payload in context->session_token
        Json::Value payload;
        if (!parse_json(context->session_token.c_str(), context->session_token.size(), payload)) {
            out["error"] = "Invalid JSON";
            status = MHD_HTTP_BAD_REQUEST;
            response = json_to_str(out);
            goto END;
        }
        if (!device->setConfig(conf_type, payload, out)) {
            out["error"] = "Failed to update config";
            status = MHD_HTTP_INTERNAL_SERVER_ERROR;
        }
        response = json_to_str(out);
        goto END;
    }

    // ---- POST /command/decode or /decode ----
    if ((path == "/command/decode" || path == "/decode") && (m == "POST")) {
        Json::Value payload;
        if (!parse_json(context->session_token.c_str(), context->session_token.size(), payload)) {
            out["error"] = "Invalid JSON";
            status = MHD_HTTP_BAD_REQUEST;
            response = json_to_str(out);
            goto END;
        }
        if (!device->controlDecode(payload, out)) {
            out["error"] = "Failed to control decode";
            status = MHD_HTTP_INTERNAL_SERVER_ERROR;
        }
        response = json_to_str(out);
        goto END;
    }

    // ---- POST /command/reboot or /reboot ----
    if ((path == "/command/reboot" || path == "/reboot") && (m == "POST")) {
        Json::Value payload;
        if (!context->session_token.empty() &&
            !parse_json(context->session_token.c_str(), context->session_token.size(), payload)) {
            out["error"] = "Invalid JSON";
            status = MHD_HTTP_BAD_REQUEST;
            response = json_to_str(out);
            goto END;
        }
        if (!device->reboot(payload, out)) {
            out["error"] = "Failed to reboot";
            status = MHD_HTTP_INTERNAL_SERVER_ERROR;
        }
        response = json_to_str(out);
        goto END;
    }

    // ---- POST /upgrade or /command/upgrade ----
    if ((path == "/upgrade" || path == "/command/upgrade") && (m == "POST")) {
        Json::Value payload;
        if (!parse_json(context->session_token.c_str(), context->session_token.size(), payload)) {
            out["error"] = "Invalid JSON";
            status = MHD_HTTP_BAD_REQUEST;
            response = json_to_str(out);
            goto END;
        }
        if (!device->upgrade(payload, out)) {
            out["error"] = "Failed to start upgrade";
            status = MHD_HTTP_INTERNAL_SERVER_ERROR;
        }
        response = json_to_str(out);
        goto END;
    }

    // ---- Not Found ----
    out["error"] = "Endpoint not found";
    response = json_to_str(out);
    status = MHD_HTTP_NOT_FOUND;

END:
    delete context;
    *con_cls = nullptr;
    return send_response(connection, status, "application/json", response);
}

// ==== MAIN SERVER STARTUP ====
int main(int argc, char* argv[]) {
    // Load HCNetSDK library
    NET_DVR_Init();
    // Start HTTP server
    struct MHD_Daemon* daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY,
        SERVER_PORT,
        NULL, NULL,
        &api_handler, NULL,
        MHD_OPTION_END
    );
    if (!daemon) {
        std::cerr << "Failed to start HTTP server on port " << SERVER_PORT << std::endl;
        return 1;
    }
    std::cout << "HTTP server started on port " << SERVER_PORT << std::endl;
    while (1) std::this_thread::sleep_for(std::chrono::seconds(60));
    MHD_stop_daemon(daemon);
    NET_DVR_Cleanup();
    return 0;
}
```
