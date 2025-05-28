#include <iostream>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <csignal>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

// --------- Utility Functions ---------

std::string get_env(const std::string& name, const std::string& def = "") {
    const char* v = getenv(name.c_str());
    return v ? std::string(v) : def;
}

void url_decode_inplace(std::string& str) {
    char* out = &str[0];
    char* in = &str[0];
    size_t len = str.length();
    for (size_t i = 0; i < len; ++i) {
        if (in[i] == '%' && i + 2 < len && std::isxdigit(in[i+1]) && std::isxdigit(in[i+2])) {
            unsigned char val = (std::isdigit(in[i+1]) ? in[i+1] - '0' : std::tolower(in[i+1]) - 'a' + 10) * 16;
            val += (std::isdigit(in[i+2]) ? in[i+2] - '0' : std::tolower(in[i+2]) - 'a' + 10);
            *out++ = val;
            i += 2;
        } else if (in[i] == '+') {
            *out++ = ' ';
        } else {
            *out++ = in[i];
        }
    }
    *out = '\0';
    str.resize(out-&str[0]);
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> params;
    size_t start = 0;
    while (start < query.size()) {
        size_t eq = query.find('=', start);
        size_t amp = query.find('&', start);
        if (eq == std::string::npos) break;
        std::string k = query.substr(start, eq - start);
        std::string v = (amp == std::string::npos)
            ? query.substr(eq + 1)
            : query.substr(eq + 1, amp - eq - 1);
        url_decode_inplace(k); url_decode_inplace(v);
        params[k] = v;
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return params;
}

std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (auto c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f') {
                    o << "\\u"
                      << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                } else {
                    o << c;
                }
        }
    }
    return o.str();
}

// ---------- Device Data Structures & Mock SDK ----------

struct ChannelStatus {
    int id;
    bool enabled;
    std::string type; // e.g. "loop", "patrol", "display"
    std::string status; // "active"/"inactive"
};

struct DeviceStatus {
    std::string sdk_state;
    std::string alarm_info;
    std::string upgrade_progress;
    std::string error_codes;
};

struct PlaybackCommand {
    std::string action; // start, stop, control
    std::string file;
    int channel_id;
    std::string params;
};

class HikvisionDevice {
    std::mutex mutex_;
    std::vector<ChannelStatus> channels_;
    DeviceStatus status_;
    std::atomic<bool> upgrading_;

public:
    HikvisionDevice() : upgrading_(false) {
        // Initialize with some channels for demonstration
        for (int i = 0; i < 8; ++i) {
            ChannelStatus ch;
            ch.id = i;
            ch.enabled = true;
            ch.type = (i%2==0) ? "display" : "loop";
            ch.status = (i%3==0) ? "active" : "inactive";
            channels_.push_back(ch);
        }
        status_.sdk_state = "normal";
        status_.alarm_info = "none";
        status_.upgrade_progress = "0%";
        status_.error_codes = "";
    }

    // --- Device Management Commands ---
    bool device_command(const std::string& cmd, std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cmd == "reboot") {
            msg = "Device rebooting";
            return true;
        } else if (cmd == "shutdown") {
            msg = "Device shutting down";
            return true;
        } else if (cmd == "restore_defaults") {
            msg = "Device settings restored to defaults";
            return true;
        } else if (cmd == "upgrade") {
            if (upgrading_) { msg = "Already upgrading"; return false; }
            upgrading_ = true;
            std::thread([this]() {
                for (int i=1; i<=10; ++i) {
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        status_.upgrade_progress = std::to_string(i*10) + "%";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                }
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    status_.upgrade_progress = "100%";
                    upgrading_ = false;
                }
            }).detach();
            msg = "Upgrade started";
            return true;
        } else if (cmd == "import_config") {
            msg = "Configuration imported";
            return true;
        } else if (cmd == "export_config") {
            msg = "Configuration exported";
            return true;
        } else {
            msg = "Unknown command";
            return false;
        }
    }

    // --- Playback ---
    bool playback(const PlaybackCommand& pb, std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pb.action == "start") {
            msg = "Playback started for file " + pb.file + " on channel " + std::to_string(pb.channel_id);
            return true;
        } else if (pb.action == "stop") {
            msg = "Playback stopped on channel " + std::to_string(pb.channel_id);
            return true;
        } else if (pb.action == "control") {
            msg = "Playback control: " + pb.params;
            return true;
        } else {
            msg = "Unknown playback command";
            return false;
        }
    }

    // --- Status ---
    DeviceStatus get_status() {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    // --- Channels ---
    std::vector<ChannelStatus> get_channels(const std::map<std::string,std::string>& filters) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ChannelStatus> result;
        for (const auto& ch : channels_) {
            bool match = true;
            auto it = filters.find("status");
            if (it != filters.end() && ch.status != it->second) match = false;
            it = filters.find("type");
            if (it != filters.end() && ch.type != it->second) match = false;
            if (match) result.push_back(ch);
        }
        return result;
    }

    // --- Channel Update ---
    bool update_channel(int id, bool* enabled, std::string* type, std::string* status, std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& ch : channels_) {
            if (ch.id == id) {
                if (enabled) ch.enabled = *enabled;
                if (type) ch.type = *type;
                if (status) ch.status = *status;
                msg = "Channel updated";
                return true;
            }
        }
        msg = "Channel not found";
        return false;
    }
};

// ----------- HTTP Server Implementation ----------------

class HttpServer {
    int port_;
    std::string host_;
    int listen_fd_;
    std::atomic<bool> running_;
    std::vector<std::thread> workers_;
    HikvisionDevice& device_;

    static constexpr int MAX_HEADER = 8192;
    static constexpr int MAX_BODY = 65536;

    static std::string http_status_text(int code) {
        switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Error";
        }
    }

    void send_response(int client, int code, const std::string& content_type, const std::string& body) {
        std::ostringstream out;
        out << "HTTP/1.1 " << code << " " << http_status_text(code) << "\r\n";
        out << "Content-Type: " << content_type << "\r\n";
        out << "Access-Control-Allow-Origin: *\r\n";
        out << "Content-Length: " << body.size() << "\r\n";
        out << "\r\n";
        out << body;
        std::string resp = out.str();
        ::send(client, resp.data(), resp.size(), 0);
    }

    void send_json(int client, int code, const std::string& json) {
        send_response(client, code, "application/json", json);
    }

    void send_204(int client) {
        send_response(client, 204, "application/json", "");
    }

    void handle_client(int client) {
        try {
            char buf[MAX_HEADER+1];
            int recvd = recv(client, buf, MAX_HEADER, 0);
            if (recvd <= 0) { ::close(client); return; }
            buf[recvd] = 0;
            std::string header(buf);
            size_t req_end = header.find("\r\n\r\n");
            if (req_end == std::string::npos) { ::close(client); return; }
            std::string reqline = header.substr(0, header.find("\r\n"));
            std::istringstream rl(reqline);
            std::string method, url, ver;
            rl >> method >> url >> ver;
            std::string path = url;
            std::string query;
            size_t qmark = url.find('?');
            if (qmark != std::string::npos) {
                path = url.substr(0, qmark);
                query = url.substr(qmark+1);
            }
            std::map<std::string,std::string> headers;
            size_t cur = header.find("\r\n") + 2;
            while (cur < req_end) {
                size_t nxt = header.find("\r\n", cur);
                if (nxt == std::string::npos) break;
                size_t colon = header.find(':', cur);
                if (colon != std::string::npos && colon < nxt) {
                    std::string h = header.substr(cur, colon-cur);
                    std::string v = header.substr(colon+1, nxt-colon-1);
                    while (!v.empty() && std::isspace(v[0])) v.erase(0,1);
                    headers[h] = v;
                }
                cur = nxt + 2;
            }
            std::string body;
            auto it = headers.find("Content-Length");
            if (it != headers.end()) {
                int clen = atoi(it->second.c_str());
                if (clen > 0 && clen < MAX_BODY) {
                    body.resize(clen);
                    int got = 0;
                    while (got < clen) {
                        int n = recv(client, &body[got], clen-got, 0);
                        if (n <= 0) break;
                        got += n;
                    }
                }
            }

            // Route
            if (method == "POST" && path == "/commands/device") {
                handle_post_commands_device(client, body);
            } else if (method == "POST" && path == "/playback") {
                handle_post_playback(client, body);
            } else if (method == "GET" && path == "/status") {
                handle_get_status(client);
            } else if (method == "GET" && path == "/channels") {
                auto params = parse_query(query);
                handle_get_channels(client, params);
            } else if (method == "PUT" && path.substr(0,10) == "/channels/") {
                std::string idstr = path.substr(10);
                handle_put_channels_id(client, idstr, body);
            } else {
                send_json(client, 404, "{\"error\":\"Not Found\"}");
            }
        } catch (...) {}
        ::close(client);
    }

    // ------ JSON Parsing (minimal, simplistic) -------
    // Only for small JSON maps with string, int, bool values.
    std::map<std::string,std::string> parse_json(const std::string& json) {
        std::map<std::string,std::string> result;
        size_t i = 0;
        while (i < json.size()) {
            while (i < json.size() && std::isspace(json[i])) ++i;
            if (i < json.size() && json[i] == '"') {
                size_t j = json.find('"', i+1);
                if (j == std::string::npos) break;
                std::string key = json.substr(i+1, j-i-1);
                i = j+1;
                while (i < json.size() && (json[i]==':' || std::isspace(json[i]))) ++i;
                if (i < json.size() && (json[i]=='"' || std::isdigit(json[i]) || json[i]=='t' || json[i]=='f')) {
                    std::string val;
                    if (json[i] == '"') {
                        size_t vj = json.find('"', i+1);
                        if (vj == std::string::npos) break;
                        val = json.substr(i+1, vj-i-1);
                        i = vj+1;
                    } else {
                        size_t vj = i;
                        while (vj<json.size() && (std::isalnum(json[vj]) || json[vj]=='.' || json[vj]=='_')) ++vj;
                        val = json.substr(i, vj-i);
                        i = vj;
                    }
                    result[key] = val;
                }
            }
            while (i < json.size() && json[i] != ',') ++i;
            if (i < json.size() && json[i] == ',') ++i;
        }
        return result;
    }

    bool parse_bool(const std::string& s) {
        return (s=="true" || s=="1" || s=="on");
    }

    // ----------- Endpoint Handlers ------------------

    void handle_post_commands_device(int client, const std::string& body) {
        auto req = parse_json(body);
        auto it = req.find("command");
        if (it == req.end()) {
            send_json(client, 400, "{\"error\":\"Missing 'command' field\"}");
            return;
        }
        std::string msg;
        bool ok = device_.device_command(it->second, msg);
        if (ok) {
            send_json(client, 200, "{\"result\":\""+json_escape(msg)+"\"}");
        } else {
            send_json(client, 400, "{\"error\":\""+json_escape(msg)+"\"}");
        }
    }

    void handle_post_playback(int client, const std::string& body) {
        auto req = parse_json(body);
        auto it = req.find("action");
        if (it == req.end()) {
            send_json(client, 400, "{\"error\":\"Missing 'action' field\"}");
            return;
        }
        PlaybackCommand pb;
        pb.action = it->second;
        pb.file = req.count("file") ? req["file"] : "";
        pb.channel_id = req.count("channel_id") ? atoi(req["channel_id"].c_str()) : 0;
        pb.params = req.count("params") ? req["params"] : "";
        std::string msg;
        bool ok = device_.playback(pb, msg);
        if (ok) {
            send_json(client, 200, "{\"result\":\""+json_escape(msg)+"\"}");
        } else {
            send_json(client, 400, "{\"error\":\""+json_escape(msg)+"\"}");
        }
    }

    void handle_get_status(int client) {
        DeviceStatus status = device_.get_status();
        std::ostringstream o;
        o << "{";
        o << "\"sdk_state\":\"" << json_escape(status.sdk_state) << "\",";
        o << "\"alarm_info\":\"" << json_escape(status.alarm_info) << "\",";
        o << "\"upgrade_progress\":\"" << json_escape(status.upgrade_progress) << "\",";
        o << "\"error_codes\":\"" << json_escape(status.error_codes) << "\"";
        o << "}";
        send_json(client, 200, o.str());
    }

    void handle_get_channels(int client, const std::map<std::string,std::string>& filters) {
        auto channels = device_.get_channels(filters);
        std::ostringstream o;
        o << "[";
        for (size_t i = 0; i < channels.size(); ++i) {
            const auto& ch = channels[i];
            if (i) o << ",";
            o << "{";
            o << "\"id\":" << ch.id << ",";
            o << "\"enabled\":" << (ch.enabled ? "true" : "false") << ",";
            o << "\"type\":\"" << json_escape(ch.type) << "\",";
            o << "\"status\":\"" << json_escape(ch.status) << "\"";
            o << "}";
        }
        o << "]";
        send_json(client, 200, o.str());
    }

    void handle_put_channels_id(int client, const std::string& idstr, const std::string& body) {
        int id = atoi(idstr.c_str());
        auto req = parse_json(body);
        bool have_any = false;
        bool enabled;
        std::string type, status;
        if (req.count("enabled")) { have_any = true; enabled = parse_bool(req["enabled"]); }
        if (req.count("type")) { have_any = true; type = req["type"]; }
        if (req.count("status")) { have_any = true; status = req["status"]; }
        if (!have_any) {
            send_json(client, 400, "{\"error\":\"No fields to update\"}");
            return;
        }
        std::string msg;
        bool ok = device_.update_channel(id, req.count("enabled")?&enabled:nullptr, req.count("type")?&type:nullptr, req.count("status")?&status:nullptr, msg);
        if (ok) {
            send_json(client, 200, "{\"result\":\""+json_escape(msg)+"\"}");
        } else {
            send_json(client, 404, "{\"error\":\""+json_escape(msg)+"\"}");
        }
    }

public:
    HttpServer(HikvisionDevice& dev, const std::string& host, int port)
        : port_(port), host_(host), listen_fd_(-1), running_(false), device_(dev) {}

    void run() {
        struct sockaddr_in addr;
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(host_.c_str());
        addr.sin_port = htons(port_);
        bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr));
        listen(listen_fd_, 16);
        running_ = true;
        while (running_) {
            int client = accept(listen_fd_, NULL, NULL);
            if (client < 0) continue;
            workers_.emplace_back([this, client]() { handle_client(client); });
        }
    }

    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) close(listen_fd_);
        for (auto& th : workers_) {
            if (th.joinable()) th.join();
        }
    }
};

// ----------- Main Entrypoint ---------------------

std::atomic<bool> sigint_flag(false);

void sigint_handler(int) {
    sigint_flag = true;
}

int main() {
    std::string device_ip = get_env("DEVICE_IP", "127.0.0.1");
    std::string server_host = get_env("SERVER_HOST", "0.0.0.0");
    int server_port = atoi(get_env("SERVER_PORT", "8080").c_str());

    HikvisionDevice device;

    signal(SIGINT, sigint_handler);

    HttpServer server(device, server_host, server_port);

    std::thread server_thread([&]() { server.run(); });

    std::cout << "Hikvision Decoder HTTP driver running on " << server_host << ":" << server_port << std::endl;

    while (!sigint_flag) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::cout << "Shutting down..." << std::endl;
    server.stop();
    server_thread.join();
    return 0;
}