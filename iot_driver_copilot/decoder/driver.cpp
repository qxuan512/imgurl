#include <iostream>
#include <string>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <json/json.h>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib,"ws2_32.lib")
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

// --- HCNetSDK C API stubs (to be replaced by actual linking in a real deployment) ---
typedef int LONG;
typedef unsigned int DWORD;
typedef unsigned char BYTE;

#define NET_DVR_NOERROR 0

struct NET_DVR_DEVICEINFO_V30 {
    BYTE sSerialNumber[48];
    BYTE byAlarmInPortNum;
    BYTE byAlarmOutPortNum;
    BYTE byDiskNum;
    BYTE byDVRType;
    BYTE byChanNum;
    BYTE byStartChan;
    BYTE byAudioChanNum;
    BYTE byIPChanNum;
    BYTE byZeroChanNum;
    BYTE bySupport;
    BYTE bySupport1;
    BYTE bySupport2;
    BYTE wDevType;
    BYTE bySupport3;
    BYTE byMultiStreamProto;
    BYTE byStartDChan;
    BYTE byStartDTalkChan;
    BYTE byHighDChanNum;
    BYTE bySupport4;
    BYTE byLanguageType;
    BYTE byVoiceInChanNum;
    BYTE byStartVoiceInChanNo;
    BYTE bySupport5;
    BYTE bySupport6;
    BYTE byMirrorChanNum;
    BYTE bySupport7;
    BYTE byRes2[2];
};

LONG NET_DVR_Login_V30(const char* sDVRIP, int wDVRPort,
                       const char* sUserName, const char* sPassword,
                       NET_DVR_DEVICEINFO_V30* lpDeviceInfo) {
    // For demonstration, always return 1 for success.
    if (strcmp(sUserName, "admin") == 0 && strcmp(sPassword, "12345") == 0) {
        if (lpDeviceInfo) memset(lpDeviceInfo, 0, sizeof(*lpDeviceInfo));
        return 1;
    }
    return -1;
}
bool NET_DVR_Logout(LONG lUserID) { return true; }
bool NET_DVR_Init() { return true; }
bool NET_DVR_Cleanup() { return true; }
DWORD NET_DVR_GetLastError() { return NET_DVR_NOERROR; }
bool NET_DVR_RebootDVR(LONG lUserID) { return true; }
bool NET_DVR_GetDVRConfig(LONG lUserID, int dwCommand, int lChannel, void *lpOutBuffer, DWORD dwOutBufferSize, DWORD *lpBytesReturned) { return true; }
bool NET_DVR_SetDVRConfig(LONG lUserID, int dwCommand, int lChannel, void *lpInBuffer, DWORD dwInBufferSize) { return true; }
bool NET_DVR_ControlDecoder(LONG lUserID, int command, void *param) { return true; }
bool NET_DVR_PlaybackControl(LONG lUserID, int command, void *param) { return true; }
// --- End of HCNetSDK API stubs ---

// Global state
std::mutex g_sdk_mutex;
bool g_sdk_initialized = false;
LONG g_user_id = -1;
std::string g_username;
std::string g_password;
std::string g_device_ip;
int g_device_port = 8000;

// ---- Helper functions ----
std::string getenv_str(const char* key, const char* def = "") {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string(def);
}
int getenv_int(const char* key, int def) {
    const char* v = std::getenv(key);
    return v ? std::atoi(v) : def;
}

void sdk_init() {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (!g_sdk_initialized) {
        NET_DVR_Init();
        g_sdk_initialized = true;
    }
}
void sdk_cleanup() {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (g_sdk_initialized) {
        NET_DVR_Cleanup();
        g_sdk_initialized = false;
    }
}

bool sdk_login(const std::string& ip, int port, const std::string& user, const std::string& pwd) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (!g_sdk_initialized) sdk_init();
    NET_DVR_DEVICEINFO_V30 devinfo;
    LONG uid = NET_DVR_Login_V30(ip.c_str(), port, user.c_str(), pwd.c_str(), &devinfo);
    if (uid < 0) return false;
    g_user_id = uid;
    g_username = user;
    g_password = pwd;
    g_device_ip = ip;
    g_device_port = port;
    return true;
}
bool sdk_logout() {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (g_user_id >= 0) {
        NET_DVR_Logout(g_user_id);
        g_user_id = -1;
        return true;
    }
    return false;
}

// --- JSON Response Helpers ---
std::string json_status(const std::string& status, const std::string& msg = "") {
    Json::Value root;
    root["status"] = status;
    if (!msg.empty()) root["message"] = msg;
    Json::StreamWriterBuilder wbuilder;
    return Json::writeString(wbuilder, root);
}
std::string json_error(const std::string& msg) {
    Json::Value root;
    root["status"] = "error";
    root["message"] = msg;
    Json::StreamWriterBuilder wbuilder;
    return Json::writeString(wbuilder, root);
}

// ---- Minimal HTTP server implementation ----
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status;
    std::string content_type;
    std::string body;
    std::map<std::string, std::string> headers;
};

std::string url_decode(const std::string& in) {
    std::string out; out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            char h1 = in[i+1], h2 = in[i+2];
            int v = (isdigit(h1)?h1-'0':tolower(h1)-'a'+10)*16 +
                    (isdigit(h2)?h2-'0':tolower(h2)-'a'+10);
            out += char(v);
            i+=2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

void split_path_query(const std::string& full, std::string& path, std::string& query) {
    auto pos = full.find('?');
    if (pos == std::string::npos) {
        path = full;
        query = "";
    } else {
        path = full.substr(0, pos);
        query = full.substr(pos+1);
    }
}
std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> params;
    std::istringstream ss(query);
    std::string item;
    while (std::getline(ss, item, '&')) {
        auto eq = item.find('=');
        if (eq != std::string::npos) {
            params[url_decode(item.substr(0, eq))] = url_decode(item.substr(eq+1));
        }
    }
    return params;
}

bool recv_line(int sock, std::string& line) {
    char c;
    line.clear();
    while (true) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\r') continue;
        if (c == '\n') break;
        line += c;
    }
    return true;
}

bool read_http_request(int sock, HttpRequest& req) {
    std::string line;
    if (!recv_line(sock, line)) return false;
    std::istringstream reqline(line);
    reqline >> req.method;
    std::string fullpath;
    reqline >> fullpath;
    split_path_query(fullpath, req.path, req.query);

    // Headers
    while (recv_line(sock, line) && !line.empty()) {
        auto pos = line.find(':');
        if (pos != std::string::npos) {
            std::string h = line.substr(0, pos);
            std::string v = line.substr(pos+1);
            while (!v.empty() && v[0] == ' ') v = v.substr(1);
            req.headers[h] = v;
        }
    }
    // Body
    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) {
        int length = std::stoi(it->second);
        req.body.resize(length);
        int recvd = 0;
        while (recvd < length) {
            int n = recv(sock, &req.body[recvd], length-recvd, 0);
            if (n <= 0) return false;
            recvd += n;
        }
    }
    return true;
}

void send_http_response(int sock, const HttpResponse& resp) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << resp.status << " ";
    switch (resp.status) {
        case 200: ss << "OK"; break;
        case 201: ss << "Created"; break;
        case 204: ss << "No Content"; break;
        case 400: ss << "Bad Request"; break;
        case 401: ss << "Unauthorized"; break;
        case 403: ss << "Forbidden"; break;
        case 404: ss << "Not Found"; break;
        case 405: ss << "Method Not Allowed"; break;
        case 500: ss << "Internal Server Error"; break;
        default: ss << "Error"; break;
    }
    ss << "\r\n";
    ss << "Content-Type: " << resp.content_type << "\r\n";
    ss << "Content-Length: " << resp.body.size() << "\r\n";
    for (const auto& kv : resp.headers) {
        ss << kv.first << ": " << kv.second << "\r\n";
    }
    ss << "Connection: close\r\n\r\n";
    ss << resp.body;
    std::string out = ss.str();
    send(sock, out.data(), out.size(), 0);
}

// ---- Actual API logic ----

HttpResponse handle_login(const HttpRequest& req) {
    Json::CharReaderBuilder rbuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(req.body);
    if (!Json::parseFromStream(rbuilder, ss, &root, &errs)) {
        return {400, "application/json", json_error("Invalid JSON payload")};
    }
    std::string user = root.get("username","").asString();
    std::string pwd = root.get("password","").asString();
    std::string ip = root.get("ip", getenv_str("DEVICE_IP").c_str()).asString();
    int port = root.isMember("port") ? root["port"].asInt() : getenv_int("DEVICE_PORT", 8000);

    if (user.empty() || pwd.empty() || ip.empty()) {
        return {400, "application/json", json_error("Missing login fields")};
    }
    if (!sdk_login(ip, port, user, pwd)) {
        return {401, "application/json", json_error("Login failed")};
    }
    Json::Value jresp;
    jresp["status"] = "success";
    jresp["message"] = "Login successful";
    jresp["ip"] = ip;
    jresp["port"] = port;
    jresp["username"] = user;
    Json::StreamWriterBuilder wbuilder;
    return {200, "application/json", Json::writeString(wbuilder, jresp)};
}

HttpResponse handle_logout(const HttpRequest&) {
    if (!sdk_logout()) {
        return {400, "application/json", json_error("Logout failed or not logged in")};
    }
    return {200, "application/json", json_status("success", "Logged out")};
}

HttpResponse handle_status(const HttpRequest&) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (g_user_id < 0) return {401, "application/json", json_error("Not logged in")};
    // For demonstration, fake some data. Replace with actual SDK calls.
    Json::Value status;
    status["device"] = "Hikvision Decoder";
    status["model"] = "DS-64XXHD_S";
    status["sdk_state"] = "connected";
    status["version"] = "V5.3.0";
    status["channel_status"] = "ok";
    status["alarm_status"] = "none";
    status["playback_progress"] = 0.0;
    status["error_code"] = NET_DVR_GetLastError();

    Json::StreamWriterBuilder wbuilder;
    return {200, "application/json", Json::writeString(wbuilder, status)};
}

HttpResponse handle_decoder_control(const HttpRequest& req) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (g_user_id < 0) return {401, "application/json", json_error("Not logged in")};
    Json::CharReaderBuilder rbuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(req.body);
    if (!Json::parseFromStream(rbuilder, ss, &root, &errs)) {
        return {400, "application/json", json_error("Invalid JSON payload")};
    }
    std::string command = root.get("command","").asString();
    int channel = root.get("channel", 0).asInt();
    if (command.empty()) return {400, "application/json", json_error("Missing command")};
    // Fake response; integrate with SDK as needed
    if (!NET_DVR_ControlDecoder(g_user_id, 0, nullptr)) {
        return {500, "application/json", json_error("Decoder control failed")};
    }
    return {200, "application/json", json_status("success", "Decoder control executed")};
}

HttpResponse handle_display_config(const HttpRequest& req) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (g_user_id < 0) return {401, "application/json", json_error("Not logged in")};
    auto params = parse_query(req.query);
    // Fake: In reality, use SDK to set display config/window info as per params and/or body
    return {200, "application/json", json_status("success", "Display config updated")};
}

HttpResponse handle_playback_control(const HttpRequest& req) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (g_user_id < 0) return {401, "application/json", json_error("Not logged in")};
    Json::CharReaderBuilder rbuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(req.body);
    if (!Json::parseFromStream(rbuilder, ss, &root, &errs)) {
        return {400, "application/json", json_error("Invalid JSON payload")};
    }
    std::string command = root.get("command","").asString();
    int channel = root.get("channel",0).asInt();
    if (command.empty()) return {400, "application/json", json_error("Missing command")};
    // Fake
    if (!NET_DVR_PlaybackControl(g_user_id, 0, nullptr)) {
        return {500, "application/json", json_error("Playback control failed")};
    }
    return {200, "application/json", json_status("success", "Playback control executed")};
}

HttpResponse handle_reboot(const HttpRequest&) {
    std::lock_guard<std::mutex> lock(g_sdk_mutex);
    if (g_user_id < 0) return {401, "application/json", json_error("Not logged in")};
    if (!NET_DVR_RebootDVR(g_user_id)) {
        return {500, "application/json", json_error("Reboot failed")};
    }
    return {200, "application/json", json_status("success", "Device reboot issued")};
}

// --- Request router ---
HttpResponse route_request(const HttpRequest& req) {
    if (req.method == "POST" && req.path == "/auth/login") return handle_login(req);
    if (req.method == "POST" && req.path == "/auth/logout") return handle_logout(req);
    if (req.method == "GET"  && req.path == "/status") return handle_status(req);
    if (req.method == "POST" && req.path == "/control/decoder") return handle_decoder_control(req);
    if (req.method == "PUT"  && req.path == "/config/display") return handle_display_config(req);
    if (req.method == "POST" && req.path == "/control/playback") return handle_playback_control(req);
    if (req.method == "POST" && req.path == "/sys/reboot") return handle_reboot(req);

    return {404, "application/json", json_error("Not found")};
}

// --- HTTP server main loop ---
void client_thread(int client_sock) {
    HttpRequest req;
    if (!read_http_request(client_sock, req)) {
        closesocket(client_sock);
        return;
    }
    HttpResponse resp = route_request(req);
    send_http_response(client_sock, resp);
    closesocket(client_sock);
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2),&wsa);
#endif
    // Get config
    int http_port = getenv_int("HTTP_PORT", 8080);
    std::string bind_addr = getenv_str("HTTP_HOST", "0.0.0.0");
    std::string device_ip = getenv_str("DEVICE_IP", "");
    int device_port = getenv_int("DEVICE_PORT", 8000);

    sdk_init();

    // Listen socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(http_port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        return 2;
    }
    listen(server_fd, 5);
    std::cout << "HTTP server listening on " << bind_addr << ":" << http_port << std::endl;

    while (true) {
        sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int client_sock = accept(server_fd, (sockaddr*)&client_addr, &clen);
        if (client_sock < 0) continue;
        std::thread(client_thread, client_sock).detach();
    }
    sdk_cleanup();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}