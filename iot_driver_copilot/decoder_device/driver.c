/*
 * Hikvision Decoder Device HTTP Driver
 * Implements HTTP API endpoints for device control and monitoring.
 * Uses environment variables for configuration.
 * Only standard C library and POSIX sockets used.
 * No external process execution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

/* Configuration via environment variables */
#define ENV_SERVER_HOST      "HTTP_SERVER_HOST"
#define ENV_SERVER_PORT      "HTTP_SERVER_PORT"
#define ENV_DEVICE_IP        "DEVICE_IP"
#define ENV_DEVICE_PORT      "DEVICE_PORT"
#define ENV_DEVICE_USER      "DEVICE_USER"
#define ENV_DEVICE_PASSWORD  "DEVICE_PASSWORD"

#define MAX_CONN             16
#define BUF_SZ               8192
#define SMALL_BUF            256
#define SESSION_TOKEN_LEN    64

/* Simulated Session Structure */
typedef struct {
    char token[SESSION_TOKEN_LEN];
    int logged_in;
    time_t last_access;
} Session;

/* Only one session for simplicity */
static Session g_session = {0};

/* Helper function: Get env var or default */
static const char* get_env(const char* env, const char* def) {
    const char* v = getenv(env);
    return (v && *v) ? v : def;
}

/* Helper: send all data */
static int sendall(int sock, const char *data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Helper: HTTP response with JSON */
static void resp_json(int sock, int code, const char* msg, const char* json) {
    char header[SMALL_BUF];
    int json_len = json ? strlen(json) : 0;
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n",
        code, msg, json_len);
    sendall(sock, header, strlen(header));
    if (json) sendall(sock, json, json_len);
}

/* Helper: HTTP 400 */
static void resp_400(int sock, const char* msg) {
    char json[SMALL_BUF];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", msg ? msg : "bad request");
    resp_json(sock, 400, "Bad Request", json);
}

/* Helper: HTTP 401 */
static void resp_401(int sock, const char* msg) {
    char json[SMALL_BUF];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", msg ? msg : "unauthorized");
    resp_json(sock, 401, "Unauthorized", json);
}

/* Helper: HTTP 404 */
static void resp_404(int sock) {
    resp_json(sock, 404, "Not Found", "{\"error\":\"not found\"}");
}

/* Helper: HTTP 500 */
static void resp_500(int sock, const char* msg) {
    char json[SMALL_BUF];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", msg ? msg : "internal error");
    resp_json(sock, 500, "Internal Server Error", json);
}

/* Helper: HTTP 200 OK */
static void resp_200(int sock, const char* json) {
    resp_json(sock, 200, "OK", json);
}

/* Helper: HTTP 204 No Content */
static void resp_204(int sock) {
    resp_json(sock, 204, "No Content", "");
}

/* Helper: generate a pseudo session token */
static void generate_token(char* buf, size_t len) {
    srand((unsigned)time(NULL) ^ getpid());
    for (size_t i = 0; i < len-1; ++i)
        buf[i] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[rand()%62];
    buf[len-1] = 0;
}

/* Helper: simple JSON field extraction (not robust, demonstration only) */
static int json_get_field(const char* json, const char* field, char* val, size_t vlen) {
    char pat[64], *p, *q; int flen;
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    ++p;
    while (*p == ' ' || *p == '\"') ++p;
    q = val;
    flen = 0;
    while (*p && *p != '\"' && *p != ',' && *p != '}') {
        if (flen < (int)vlen-1) *q++ = *p, ++flen;
        ++p;
    }
    *q = 0;
    return 1;
}

/* Helper: authenticate token */
static int check_auth(const char* header) {
    if (!g_session.logged_in) return 0;
    if (!header) return 0;
    const char *p = strstr(header, "Authorization: Bearer ");
    if (!p) return 0;
    p += strlen("Authorization: Bearer ");
    if (strncmp(p, g_session.token, strlen(g_session.token)) == 0) return 1;
    return 0;
}

/* Simulated device SDK calls - for demonstration purposes only */
static int sdk_login(const char* user, const char* pass) {
    /* In real driver, connect to device via TCP, send login */
    if (strcmp(user, get_env(ENV_DEVICE_USER, "admin")) == 0 &&
        strcmp(pass, get_env(ENV_DEVICE_PASSWORD, "12345")) == 0)
        return 1;
    return 0;
}
static int sdk_logout() { return 1; }
static int sdk_get_config(char* out, size_t max) {
    snprintf(out, max,
        "{\"display_channels\":4,\"decode_channels\":8,\"scene_parameters\":{\"wall\":\"A1\",\"window\":\"W1\"}}");
    return 0;
}
static int sdk_update_config(const char* json) {
    /* Accept all configs */
    return 0;
}
static int sdk_get_state(char* out, size_t max) {
    snprintf(out, max,
        "{\"state\":\"OK\",\"error_code\":0,\"channels\":[{\"id\":1,\"status\":\"active\"}]}");
    return 0;
}
static int sdk_alarm_cmd(const char* action) {
    /* Accept start/stop */
    if (strcmp(action, "start") == 0 || strcmp(action, "stop") == 0)
        return 0;
    return -1;
}
static int sdk_decode_cmd(const char* action) {
    /* Accept start/stop */
    if (strcmp(action, "start") == 0 || strcmp(action, "stop") == 0)
        return 0;
    return -1;
}
static int sdk_reboot() { return 0; }

/* HTTP request parsing */
typedef struct {
    char method[8];
    char path[128];
    char headers[2048];
    char body[BUF_SZ];
} HttpReq;

/* Parse HTTP request (method, path, headers, body) */
static int parse_http_req(const char* data, HttpReq* req) {
    memset(req, 0, sizeof(*req));
    const char *p = data, *q;
    /* Parse method */
    q = strchr(p, ' ');
    if (!q || (q-p) > 7) return -1;
    strncpy(req->method, p, q-p);
    req->method[q-p] = 0;
    p = q+1;
    /* Parse path */
    q = strchr(p, ' ');
    if (!q || (q-p) > 127) return -1;
    strncpy(req->path, p, q-p);
    req->path[q-p] = 0;
    p = strstr(q, "\r\n");
    if (!p) return -1;
    p += 2;
    /* Parse headers */
    q = strstr(p, "\r\n\r\n");
    if (!q) return -1;
    size_t hlen = q-p;
    if (hlen > sizeof(req->headers)-1) hlen = sizeof(req->headers)-1;
    memcpy(req->headers, p, hlen);
    req->headers[hlen] = 0;
    p = q+4;
    /* Body */
    strncpy(req->body, p, sizeof(req->body)-1);
    return 0;
}

/* Handler functions for endpoints */
static void handle_login(int sock, HttpReq* req) {
    char user[64], pass[64];
    if (!json_get_field(req->body, "username", user, sizeof(user)) ||
        !json_get_field(req->body, "password", pass, sizeof(pass))) {
        resp_400(sock, "Missing username/password");
        return;
    }
    if (sdk_login(user, pass)) {
        g_session.logged_in = 1;
        generate_token(g_session.token, SESSION_TOKEN_LEN);
        g_session.last_access = time(NULL);
        char json[SMALL_BUF];
        snprintf(json, sizeof(json), "{\"token\":\"%s\"}", g_session.token);
        resp_200(sock, json);
    } else {
        resp_401(sock, "Invalid credentials");
    }
}

static void handle_logout(int sock, HttpReq* req) {
    if (!check_auth(req->headers)) {
        resp_401(sock, NULL);
        return;
    }
    sdk_logout();
    g_session.logged_in = 0;
    g_session.token[0] = 0;
    resp_204(sock);
}

static void handle_get_config(int sock, HttpReq* req) {
    if (!check_auth(req->headers)) {
        resp_401(sock, NULL);
        return;
    }
    char out[BUF_SZ];
    if (sdk_get_config(out, sizeof(out)) == 0)
        resp_200(sock, out);
    else
        resp_500(sock, "Device config unavailable");
}

static void handle_put_config(int sock, HttpReq* req) {
    if (!check_auth(req->headers)) {
        resp_401(sock, NULL);
        return;
    }
    if (sdk_update_config(req->body) == 0)
        resp_204(sock);
    else
        resp_400(sock, "Invalid config");
}

static void handle_get_state(int sock, HttpReq* req) {
    if (!check_auth(req->headers)) {
        resp_401(sock, NULL);
        return;
    }
    char out[BUF_SZ];
    if (sdk_get_state(out, sizeof(out)) == 0)
        resp_200(sock, out);
    else
        resp_500(sock, "Device state unavailable");
}

static void handle_alarm(int sock, HttpReq* req) {
    if (!check_auth(req->headers)) {
        resp_401(sock, NULL);
        return;
    }
    char action[16];
    if (!json_get_field(req->body, "action", action, sizeof(action))) {
        resp_400(sock, "Missing action");
        return;
    }
    if (sdk_alarm_cmd(action) == 0)
        resp_204(sock);
    else
        resp_400(sock, "Invalid alarm action");
}

static void handle_decode(int sock, HttpReq* req) {
    if (!check_auth(req->headers)) {
        resp_401(sock, NULL);
        return;
    }
    char action[16];
    if (!json_get_field(req->body, "action", action, sizeof(action))) {
        resp_400(sock, "Missing action");
        return;
    }
    if (sdk_decode_cmd(action) == 0)
        resp_204(sock);
    else
        resp_400(sock, "Invalid decode action");
}

static void handle_reboot(int sock, HttpReq* req) {
    if (!check_auth(req->headers)) {
        resp_401(sock, NULL);
        return;
    }
    if (sdk_reboot() == 0)
        resp_204(sock);
    else
        resp_500(sock, "Reboot failed");
}

/* Dispatch HTTP request to handler */
static void dispatch(int sock, HttpReq* req) {
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/login") == 0)
        handle_login(sock, req);
    else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/logout") == 0)
        handle_logout(sock, req);
    else if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/config") == 0)
        handle_get_config(sock, req);
    else if (strcmp(req->method, "PUT") == 0 && strcmp(req->path, "/config") == 0)
        handle_put_config(sock, req);
    else if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/state") == 0)
        handle_get_state(sock, req);
    else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/cmd/alarm") == 0)
        handle_alarm(sock, req);
    else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/cmd/decode") == 0)
        handle_decode(sock, req);
    else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/cmd/reboot") == 0)
        handle_reboot(sock, req);
    else
        resp_404(sock);
}

/* Main HTTP server loop */
static void run_server(const char *host, int port) {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); exit(1); }
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = host ? inet_addr(host) : INADDR_ANY;
    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenfd);
        exit(1);
    }
    if (listen(listenfd, MAX_CONN) < 0) {
        perror("listen");
        close(listenfd);
        exit(1);
    }
    printf("HTTP server listening on %s:%d\n", host ? host : "0.0.0.0", port);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int clientfd = accept(listenfd, (struct sockaddr*)&cli_addr, &cli_len);
        if (clientfd < 0) continue;
        char buf[BUF_SZ+1];
        int r = recv(clientfd, buf, BUF_SZ, 0);
        if (r <= 0) { close(clientfd); continue; }
        buf[r] = 0;
        HttpReq req;
        if (parse_http_req(buf, &req) == 0)
            dispatch(clientfd, &req);
        else
            resp_400(clientfd, "Malformed request");
        close(clientfd);
    }
}

int main() {
    const char* host = get_env(ENV_SERVER_HOST, "0.0.0.0");
    int port = atoi(get_env(ENV_SERVER_PORT, "8080"));
    /* These are for simulated SDK login */
    get_env(ENV_DEVICE_USER, "admin");
    get_env(ENV_DEVICE_PASSWORD, "12345");
    run_server(host, port);
    return 0;
}