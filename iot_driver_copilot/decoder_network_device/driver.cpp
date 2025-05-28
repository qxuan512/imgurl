// hikvision_decoder_http_driver.cpp

#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <map>
#include <thread>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <cstring>

// Minimal HTTP server (C++11): No external dependencies
// Only for demonstration and driver logic

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib,"ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR   -1
typedef int SOCKET;
#endif

// --- Device Protocol Simulation Layer ---
// (In practice, use HCNetSDK or network calls. Here, simulate responses.)

struct ChannelConfig {
    int id;
    bool enabled;
    std::string name;
    std::string stream;
};

struct DisplayConfig {
    std::string layout;
    std::vector<std::string> scenes;
};

struct DeviceStatus {
    std::string decoder_state;
    std::string alarm;
    std::string remote_play;
    int upgrade_progress;
    std::string polling_info;
};

// Simulated state storage
std::vector<ChannelConfig> channels = {
    {1, true,  "Main Entrance", "rtsp://192.168.1.100/stream1"},
    {2, false, "Back Door",     "rtsp://192.168.1.100/stream2"},
    {3, true,  "Lobby",         "rtsp://192.168.1.100/stream3"}
};

DisplayConfig display_config = {"2x2", {"SceneA", "SceneB"}};

DeviceStatus device_status = {
    "Running", "Normal", "Stopped", 37, "Loop1"
};

std::mutex state_mutex;

// --- Utility ---
std::string getenv_default(const char* name, const std::string& def) {
    const char* val = std::getenv(name);
    return val ? val : def;
}

std::string int_to_str(int x) {
    std::ostringstream oss;
    oss << x;
    return oss.str();
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// --- Minimal HTTP Server Implementation ---
class HttpServer {
public:
    HttpServer(const std::string& host, int port)
        : host_(host), port_(port), running_(false) {}
    ~HttpServer() { stop(); }

    void route(const std::string& method, const std::string& path, std::function<void(const std::string&, const std::map<std::string,std::string>&, const std::string&, std::ostream&)> handler) {
        std::string key = method + ":" + path;
        handlers_[key] = handler;
    }

    void start() {
        running_ = true;
        server_thread_ = std::thread([this] { this->run(); });
    }

    void stop() {
        running_ = false;
#ifdef _WIN32
        closesocket(listen_sock_);
        WSACleanup();
#else
        close(listen_sock_);
#endif
        if (server_thread_.joinable()) server_thread_.join();
    }

private:
    std::string host_;
    int port_;
    bool running_;
    std::thread server_thread_;
    SOCKET listen_sock_;
    std::map<std::string, std::function<void(const std::string&, const std::map<std::string,std::string>&, const std::string&, std::ostream&)>> handlers_;

    void run() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2),&wsa);
#endif
        listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == INVALID_SOCKET) return;

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(host_.c_str());
        addr.sin_port = htons(port_);

        int optval = 1;
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));

        if (bind(listen_sock_, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return;
        if (listen(listen_sock_, 8) == SOCKET_ERROR) return;

        while (running_) {
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            SOCKET client = accept(listen_sock_, (struct sockaddr*)&client_addr, &client_len);
            if (client == INVALID_SOCKET) continue;
            std::thread(&HttpServer::handle_client, this, client).detach();
        }
    }

    void handle_client(SOCKET client) {
        char buf[8192];
        int received = recv(client, buf, sizeof(buf)-1, 0);
        if (received <= 0) {
#ifdef _WIN32
            closesocket(client);
#else
            close(client);
#endif
            return;
        }
        buf[received] = 0;
        std::istringstream request_stream(buf);
        std::string request_line;
        std::getline(request_stream, request_line);
        request_line.erase(request_line.find_last_not_of("\r\n")+1);

        std::string method, path, version;
        std::istringstream rl(request_line);
        rl >> method >> path >> version;

        std::map<std::string,std::string> headers;
        std::string header_line;
        while (std::getline(request_stream, header_line)) {
            if (header_line == "\r" || header_line.empty()) break;
            auto pos = header_line.find(':');
            if (pos != std::string::npos) {
                std::string key = header_line.substr(0, pos);
                std::string val = header_line.substr(pos+1);
                val.erase(0, val.find_first_not_of(" \t\r\n"));
                val.erase(val.find_last_not_of(" \t\r\n")+1);
                headers[key] = val;
            }
        }
        std::string body;
        if (headers.count("Content-Length")) {
            int len = std::stoi(headers["Content-Length"]);
            body.resize(len);
            request_stream.read(&body[0], len);
        }

        // Parse query string
        std::string path_only = path;
        std::map<std::string, std::string> query_params;
        auto qpos = path.find('?');
        if (qpos != std::string::npos) {
            path_only = path.substr(0, qpos);
            std::string query = path.substr(qpos+1);
            std::istringstream qss(query);
            std::string kv;
            while (std::getline(qss, kv, '&')) {
                auto eq = kv.find('=');
                if (eq != std::string::npos) {
                    query_params[kv.substr(0,eq)] = kv.substr(eq+1);
                }
            }
        }

        std::ostringstream response;
        bool handled = false;
        for (const auto& h : handlers_) {
            std::string route_method = h.first.substr(0, h.first.find(":"));
            std::string route_path   = h.first.substr(h.first.find(":")+1);

            // Support path with {id} variables
            if (method == route_method && match_path(route_path, path_only, query_params)) {
                h.second(path_only, query_params, body, response);
                handled = true;
                break;
            }
        }
        if (!handled) {
            response << "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n\r\n{\"error\":\"Not Found\"}";
        }
        std::string resp_str = response.str();
        send(client, resp_str.c_str(), (int)resp_str.length(), 0);
#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
    }

    // Path matching with {id} support
    bool match_path(const std::string& route, const std::string& actual, std::map<std::string,std::string>& query_params) {
        std::vector<std::string> r_parts, a_parts;
        split(route, '/', r_parts);
        split(actual, '/', a_parts);
        if (r_parts.size() != a_parts.size()) return false;
        for (size_t i=0; i<r_parts.size(); ++i) {
            if (!r_parts[i].empty() && r_parts[i][0] == '{' && r_parts[i].back() == '}') {
                std::string key = r_parts[i].substr(1, r_parts[i].size()-2);
                query_params[key] = a_parts[i];
            } else if (r_parts[i] != a_parts[i]) {
                return false;
            }
        }
        return true;
    }
    void split(const std::string& s, char delim, std::vector<std::string>& out) {
        std::istringstream iss(s);
        std::string item;
        while (std::getline(iss, item, delim)) {
            if (!item.empty()) out.push_back(item);
        }
    }
};

// --- JSON Generators ---
std::string channels_to_json(const std::vector<ChannelConfig>& chs, int page=0, int page_size=0) {
    std::ostringstream oss;
    oss << "[";
    int count = 0, skipped = 0;
    for (const auto& ch : chs) {
        if (page_size > 0 && skipped < page*page_size) { skipped++; continue; }
        if (page_size > 0 && count >= page_size) break;
        if (count > 0) oss << ",";
        oss << "{"
            << "\"id\":" << ch.id << ","
            << "\"enabled\":" << (ch.enabled ? "true":"false") << ","
            << "\"name\":\"" << json_escape(ch.name) << "\","
            << "\"stream\":\"" << json_escape(ch.stream) << "\""
            << "}";
        count++;
    }
    oss << "]";
    return oss.str();
}

std::string display_to_json(const DisplayConfig& d) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"layout\":\"" << json_escape(d.layout) << "\",";
    oss << "\"scenes\":[";
    for (size_t i=0; i<d.scenes.size(); ++i) {
        if (i>0) oss << ",";
        oss << "\"" << json_escape(d.scenes[i]) << "\"";
    }
    oss << "]";
    oss << "}";
    return oss.str();
}

std::string status_to_json(const DeviceStatus& s) {
    std::ostringstream oss;
    oss << "{"
        << "\"decoder_state\":\"" << json_escape(s.decoder_state) << "\","
        << "\"alarm\":\"" << json_escape(s.alarm) << "\","
        << "\"remote_play\":\"" << json_escape(s.remote_play) << "\","
        << "\"upgrade_progress\":" << s.upgrade_progress << ","
        << "\"polling_info\":\"" << json_escape(s.polling_info) << "\""
        << "}";
    return oss.str();
}

// --- API Handlers ---
void handle_get_channels(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    std::lock_guard<std::mutex> lock(state_mutex);
    std::vector<ChannelConfig> filtered = channels;
    // Filtering
    if (params.count("id")) {
        int id = std::stoi(params.at("id"));
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(), [id](const ChannelConfig& c){ return c.id != id; }), filtered.end());
    }
    // Pagination
    int page = 0, page_size = 0;
    if (params.count("page")) page = std::stoi(params.at("page"));
    if (params.count("page_size")) page_size = std::stoi(params.at("page_size"));
    std::string resp = channels_to_json(filtered, page, page_size);
    out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" << resp;
}

void handle_put_channels(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    // Update decoder channel configuration (enable/disable)
    std::lock_guard<std::mutex> lock(state_mutex);
    // Expect: [{"id":1,"enabled":true}, ...]
    size_t pos = 0;
    std::vector<std::pair<int,bool>> updates;
    while ((pos = body.find("\"id\":",pos)) != std::string::npos) {
        size_t id_start = body.find(":", pos)+1;
        size_t id_end = body.find(",", id_start);
        int id = std::stoi(body.substr(id_start, id_end-id_start));
        size_t en_pos = body.find("\"enabled\":", id_end);
        bool enabled = false;
        if (en_pos != std::string::npos) {
            enabled = (body.find("true", en_pos) < body.find("}", en_pos));
        }
        updates.push_back({id, enabled});
        pos = id_end;
    }
    for (auto& up : updates) {
        for (auto& ch : channels) {
            if (ch.id == up.first) ch.enabled = up.second;
        }
    }
    out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"result\":\"success\"}";
}

void handle_put_channel_id(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    // Update a specific decoder channel configuration
    if (!params.count("id")) {
        out << "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"error\":\"Missing channel id\"}";
        return;
    }
    int id = std::stoi(params.at("id"));
    bool found = false;
    bool enabled = false;
    size_t en_pos = body.find("\"enabled\":");
    if (en_pos != std::string::npos) {
        enabled = (body.find("true", en_pos) < body.find("}", en_pos));
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    for (auto& ch : channels) {
        if (ch.id == id) {
            ch.enabled = enabled;
            found = true;
        }
    }
    if (!found) {
        out << "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n\r\n{\"error\":\"Channel not found\"}";
    } else {
        out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"result\":\"success\"}";
    }
}

void handle_post_control(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    // Issue control commands
    // Expect JSON: {"command":"reset","params":{...}}
    if (body.find("\"command\"") == std::string::npos) {
        out << "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"error\":\"Missing command\"}";
        return;
    }
    std::string command;
    size_t cmd_pos = body.find("\"command\"");
    size_t val_start = body.find("\"", cmd_pos+9);
    size_t val_end = body.find("\"", val_start+1);
    if (val_start != std::string::npos && val_end != std::string::npos)
        command = body.substr(val_start+1, val_end-val_start-1);

    std::lock_guard<std::mutex> lock(state_mutex);
    std::string result = "unknown";
    if (command == "reset") {
        device_status.decoder_state = "Resetting";
        result = "resetting";
    } else if (command == "reboot") {
        device_status.decoder_state = "Rebooting";
        result = "rebooting";
    } else if (command == "start_decode") {
        device_status.decoder_state = "Decoding";
        result = "started";
    } else if (command == "stop_decode") {
        device_status.decoder_state = "Idle";
        result = "stopped";
    } else {
        result = "unknown command";
    }
    out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"result\":\"" << result << "\"}";
}

void handle_post_commands_decode(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    // Control dynamic decode operations
    // {"action":"start"|"stop"}
    bool start = body.find("\"action\":\"start\"") != std::string::npos;
    std::lock_guard<std::mutex> lock(state_mutex);
    if (start) device_status.decoder_state = "Decoding";
    else device_status.decoder_state = "Idle";
    out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"result\":\"" << (start ? "started":"stopped") << "\"}";
}

void handle_post_commands_reboot(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    // Reboot the device
    std::lock_guard<std::mutex> lock(state_mutex);
    device_status.decoder_state = "Rebooting";
    out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"result\":\"rebooting\"}";
}

void handle_put_display(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    // Modify the display output configuration
    // {"layout":"3x3", "scenes":["Main","Sub"]}
    std::lock_guard<std::mutex> lock(state_mutex);
    size_t l_pos = body.find("\"layout\"");
    if (l_pos != std::string::npos) {
        size_t v1 = body.find("\"", l_pos+8);
        size_t v2 = body.find("\"", v1+1);
        if (v1 != std::string::npos && v2 != std::string::npos)
            display_config.layout = body.substr(v1+1, v2-v1-1);
    }
    size_t s_pos = body.find("\"scenes\"");
    if (s_pos != std::string::npos) {
        display_config.scenes.clear();
        size_t arr_start = body.find("[", s_pos);
        size_t arr_end = body.find("]", arr_start);
        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            std::string arr = body.substr(arr_start+1, arr_end-arr_start-1);
            std::istringstream iss(arr);
            std::string scene;
            while (std::getline(iss, scene, ',')) {
                scene.erase(std::remove(scene.begin(), scene.end(), '\"'), scene.end());
                scene.erase(std::remove_if(scene.begin(), scene.end(), ::isspace), scene.end());
                if (!scene.empty()) display_config.scenes.push_back(scene);
            }
        }
    }
    out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" << display_to_json(display_config);
}

void handle_get_display(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    std::lock_guard<std::mutex> lock(state_mutex);
    out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" << display_to_json(display_config);
}

void handle_get_status(const std::string& path, const std::map<std::string,std::string>& params, const std::string& body, std::ostream& out) {
    std::lock_guard<std::mutex> lock(state_mutex);
    out << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" << status_to_json(device_status);
}

// --- MAIN ---
int main() {
    std::string host = getenv_default("SERVER_HOST", "0.0.0.0");
    int port = std::stoi(getenv_default("SERVER_PORT", "8080"));

    HttpServer server(host, port);

    // API Routing
    server.route("GET",  "/channels",         handle_get_channels);
    server.route("PUT",  "/channels",         handle_put_channels);
    server.route("PUT",  "/channels/{id}",    handle_put_channel_id);
    server.route("POST", "/control",          handle_post_control);
    server.route("POST", "/commands/decode",  handle_post_commands_decode);
    server.route("POST", "/commands/reboot",  handle_post_commands_reboot);
    server.route("PUT",  "/display",          handle_put_display);
    server.route("GET",  "/display",          handle_get_display);
    server.route("GET",  "/status",           handle_get_status);

    std::cout << "Hikvision Decoder HTTP Driver running on " << host << ":" << port << std::endl;
    server.start();

    // Wait forever
    std::mutex mtx;
    std::unique_lock<std::mutex> lck(mtx);
    std::condition_variable cv;
    cv.wait(lck);

    return 0;
}