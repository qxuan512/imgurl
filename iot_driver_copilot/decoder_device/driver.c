#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>

/* ==== Configuration from Environment Variables ==== */
#define ENV_SERVER_PORT "DRV_HTTP_PORT"
#define ENV_SERVER_HOST "DRV_HTTP_HOST"
#define ENV_DEVICE_IP   "DRV_DEVICE_IP"
#define ENV_DEVICE_PORT "DRV_DEVICE_PORT"
#define ENV_DEVICE_USER "DRV_DEVICE_USER"
#define ENV_DEVICE_PASS "DRV_DEVICE_PASS"

#define DEFAULT_SERVER_PORT 8080
#define MAX_POST_DATA_SIZE 8192
#define MAX_SESSION_TOKEN   128
#define SESSION_TIMEOUT_SEC 3600

/* ==== Device Session and State ==== */
typedef struct {
    char token[MAX_SESSION_TOKEN];
    time_t last_active;
    bool logged_in;
} session_t;

static session_t g_session = {0};

/* ==== Device Connection Info ==== */
typedef struct {
    char ip[64];
    int  port;
    char username[64];
    char password[64];
} device_info_t;

static device_info_t g_device = {0};

/* ==== Utility Functions ==== */
static int json_response(struct MHD_Connection *conn, int status, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(buf), (void*)buf, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json");
    int ret = MHD_queue_response(conn, status, response);
    MHD_destroy_response(response);
    return ret;
}

static int plain_response(struct MHD_Connection *conn, int status, const char *msg) {
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(msg), (void*)msg, MHD_RESPMEM_MUST_COPY);
    int ret = MHD_queue_response(conn, status, response);
    MHD_destroy_response(response);
    return ret;
}

static void trim(char *s) {
    char *p = s;
    while (*p && isspace(*p)) p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    size_t l = strlen(s);
    while (l && isspace(s[l-1])) s[--l] = '\0';
}

/* ==== Session Token ==== */
static void generate_token(char *out, size_t maxlen) {
    const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t clen = strlen(chars);
    srand(time(NULL) ^ (intptr_t)&out);
    for (size_t i = 0; i < maxlen-1; ++i)
        out[i] = chars[rand()%clen];
    out[maxlen-1] = 0;
}

/* ==== Device "Native" Mockup API (Would map to Hikvision SDK in real) ==== */

#define MOCK_CONFIG "{\"display_channels\": 4, \"decode_channels\": 4, \"scene\": \"MainWall\", \"param\": {\"brightness\": 80}}"
#define MOCK_STATUS "{\"state\": \"online\", \"channels\": 4, \"alarms\": 0, \"error_codes\": [], \"system_info\": {\"model\": \"DS-6300D-JX\", \"version\": \"V8.1.2_build2024\"}}"
#define MOCK_STATE "{\"operational\": \"OK\", \"error_codes\": [], \"channel_conditions\": [\"active\", \"idle\", \"idle\", \"active\"]}"

static int device_login(const char *user, const char *pass) {
    // In real: connect to device and authenticate via SDK
    if (strcmp(user, g_device.username) == 0 && strcmp(pass, g_device.password) == 0) {
        g_session.logged_in = true;
        g_session.last_active = time(NULL);
        return 1;
    }
    return 0;
}

static void device_logout(void) {
    g_session.logged_in = false;
}

static int device_is_logged_in(void) {
    if (!g_session.logged_in) return 0;
    if (time(NULL) - g_session.last_active > SESSION_TIMEOUT_SEC) {
        g_session.logged_in = false;
        return 0;
    }
    return 1;
}

static const char* device_get_config(void) {
    // In real: retrieve config structure from device and convert to JSON
    return MOCK_CONFIG;
}

static int device_set_config(const char *json) {
    // In real: send new config to device, parse JSON, map to C struct, and call SDK
    (void)json;
    return 0;
}

static const char* device_get_status(void) {
    // In real: retrieve status from device, convert to JSON
    return MOCK_STATUS;
}

static const char* device_get_state(void) {
    return MOCK_STATE;
}

static int device_alarm(const char *action) {
    // In real: start or stop alarm monitoring via SDK
    (void)action;
    return 0;
}

static int device_decode(const char *action) {
    // In real: start or stop decoding via SDK
    (void)action;
    return 0;
}

static int device_shutdown(void) {
    // In real: send shutdown command via SDK
    return 0;
}

static int device_reboot(void) {
    // In real: send reboot command via SDK
    return 0;
}

/* ==== HTTP POST Data Handling ==== */
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} post_data_t;

static int post_data_append(post_data_t *pd, const char *data, size_t size) {
    if (pd->size + size > pd->capacity) return 0;
    memcpy(pd->data + pd->size, data, size);
    pd->size += size;
    pd->data[pd->size] = 0;
    return 1;
}

/* ==== API Routing ==== */
typedef enum {
    API_LOGIN,
    API_LOGOUT,
    API_CONFIG_GET,
    API_CONFIG_PUT,
    API_STATUS,
    API_STATE,
    API_CMD_ALARM,
    API_CMD_DECODE,
    API_CMD_REBOOT,
    API_CMD_SHUTDOWN,
    API_REBOOT,
    API_SHUTDOWN,
    API_UNKNOWN
} api_endpoint_t;

static api_endpoint_t match_api(const char *url, const char *method) {
    if (strcmp(url, "/login")==0 && strcmp(method, "POST")==0) return API_LOGIN;
    if (strcmp(url, "/logout")==0 && strcmp(method, "POST")==0) return API_LOGOUT;
    if (strcmp(url, "/config")==0 && strcmp(method, "GET")==0) return API_CONFIG_GET;
    if (strcmp(url, "/config")==0 && strcmp(method, "PUT")==0) return API_CONFIG_PUT;
    if (strcmp(url, "/status")==0 && strcmp(method, "GET")==0) return API_STATUS;
    if (strcmp(url, "/state")==0 && strcmp(method, "GET")==0) return API_STATE;
    if (strcmp(url, "/cmd/alarm")==0 && strcmp(method, "POST")==0) return API_CMD_ALARM;
    if (strcmp(url, "/cmd/decode")==0 && strcmp(method, "POST")==0) return API_CMD_DECODE;
    if (strcmp(url, "/cmd/reboot")==0 && strcmp(method, "POST")==0) return API_CMD_REBOOT;
    if (strcmp(url, "/reboot")==0 && strcmp(method, "POST")==0) return API_REBOOT;
    if (strcmp(url, "/shutdown")==0 && strcmp(method, "POST")==0) return API_SHUTDOWN;
    return API_UNKNOWN;
}

/* ==== API Endpoint Handlers ==== */

static int handle_login(struct MHD_Connection *conn, const char *payload) {
    char user[64] = {0}, pass[64] = {0};
    const char *pu = strstr(payload, "\"username\"");
    const char *pp = strstr(payload, "\"password\"");
    if (!pu || !pp) return json_response(conn, 400, "{\"error\":\"Missing credentials\"}");
    sscanf(pu, "\"username\"%*s:%*s\"%63[^\"]", user);
    sscanf(pp, "\"password\"%*s:%*s\"%63[^\"]", pass);
    trim(user); trim(pass);
    if (device_login(user, pass)) {
        generate_token(g_session.token, sizeof(g_session.token));
        g_session.last_active = time(NULL);
        return json_response(conn, 200, "{\"token\":\"%s\"}", g_session.token);
    }
    return json_response(conn, 401, "{\"error\":\"Invalid credentials\"}");
}

static int handle_logout(struct MHD_Connection *conn) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    device_logout();
    return json_response(conn, 200, "{\"message\":\"Logged out\"}");
}

static int handle_get_config(struct MHD_Connection *conn) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    const char *cfg = device_get_config();
    return json_response(conn, 200, "%s", cfg);
}

static int handle_put_config(struct MHD_Connection *conn, const char *payload) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    if (!payload) return json_response(conn, 400, "{\"error\":\"Missing config payload\"}");
    if (device_set_config(payload) == 0)
        return json_response(conn, 200, "{\"message\":\"Config updated\"}");
    else
        return json_response(conn, 500, "{\"error\":\"Config update failed\"}");
}

static int handle_status(struct MHD_Connection *conn) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    const char *st = device_get_status();
    return json_response(conn, 200, "%s", st);
}

static int handle_state(struct MHD_Connection *conn) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    const char *state = device_get_state();
    return json_response(conn, 200, "%s", state);
}

static int handle_cmd_alarm(struct MHD_Connection *conn, const char *payload) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    const char *pa = strstr(payload, "\"action\"");
    if (!pa) return json_response(conn, 400, "{\"error\":\"Missing action\"}");
    char action[16] = {0};
    sscanf(pa, "\"action\"%*s:%*s\"%15[^\"]", action);
    trim(action);
    if (device_alarm(action) == 0)
        return json_response(conn, 200, "{\"message\":\"Alarm %s\"}", action);
    else
        return json_response(conn, 500, "{\"error\":\"Alarm command failed\"}");
}

static int handle_cmd_decode(struct MHD_Connection *conn, const char *payload) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    const char *pa = strstr(payload, "\"action\"");
    if (!pa) return json_response(conn, 400, "{\"error\":\"Missing action\"}");
    char action[16] = {0};
    sscanf(pa, "\"action\"%*s:%*s\"%15[^\"]", action);
    trim(action);
    if (device_decode(action) == 0)
        return json_response(conn, 200, "{\"message\":\"Decode %s\"}", action);
    else
        return json_response(conn, 500, "{\"error\":\"Decode command failed\"}");
}

static int handle_cmd_reboot(struct MHD_Connection *conn) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    if (device_reboot() == 0)
        return json_response(conn, 200, "{\"message\":\"Rebooting\"}");
    else
        return json_response(conn, 500, "{\"error\":\"Reboot failed\"}");
}

static int handle_reboot(struct MHD_Connection *conn) {
    return handle_cmd_reboot(conn);
}

static int handle_shutdown(struct MHD_Connection *conn) {
    if (!device_is_logged_in()) return json_response(conn, 401, "{\"error\":\"Not authenticated\"}");
    if (device_shutdown() == 0)
        return json_response(conn, 200, "{\"message\":\"Shutdown initiated\"}");
    else
        return json_response(conn, 500, "{\"error\":\"Shutdown failed\"}");
}

/* ==== HTTP Request Handler ==== */
struct connection_info_struct {
    post_data_t post_data;
    api_endpoint_t endpoint;
};

static int iterate_post(void *coninfo_cls, enum MHD_ValueKind kind, const char *key, const char *filename, const char *content_type, const char *transfer_encoding, const char *data, uint64_t off, size_t size) {
    struct connection_info_struct *coninfo = (struct connection_info_struct*)coninfo_cls;
    if (off + size > MAX_POST_DATA_SIZE) return MHD_NO;
    if (!post_data_append(&coninfo->post_data, data, size)) return MHD_NO;
    return MHD_YES;
}

static int answer_to_connection(void *cls, struct MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls) {
    static int dummy;
    if (*con_cls == NULL) {
        struct connection_info_struct *coninfo = calloc(1, sizeof(*coninfo));
        coninfo->post_data.data = calloc(1, MAX_POST_DATA_SIZE+1);
        coninfo->post_data.capacity = MAX_POST_DATA_SIZE;
        coninfo->endpoint = match_api(url, method);
        *con_cls = coninfo;
        return MHD_YES;
    }
    struct connection_info_struct *coninfo = *con_cls;

    /* POST/PUT: accumulate data */
    if ((strcmp(method, "POST")==0 || strcmp(method, "PUT")==0) && *upload_data_size) {
        post_data_append(&coninfo->post_data, upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* Check Auth for all except /login */
    if (coninfo->endpoint != API_LOGIN) {
        const char *token = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Session-Token");
        if (!token || strcmp(token, g_session.token)!=0 || !device_is_logged_in())
            return json_response(connection, 401, "{\"error\":\"Not authenticated\"}");
        g_session.last_active = time(NULL);
    }

    int ret = MHD_NO;
    switch (coninfo->endpoint) {
        case API_LOGIN:
            ret = handle_login(connection, coninfo->post_data.data);
            break;
        case API_LOGOUT:
            ret = handle_logout(connection);
            break;
        case API_CONFIG_GET:
            ret = handle_get_config(connection);
            break;
        case API_CONFIG_PUT:
            ret = handle_put_config(connection, coninfo->post_data.data);
            break;
        case API_STATUS:
            ret = handle_status(connection);
            break;
        case API_STATE:
            ret = handle_state(connection);
            break;
        case API_CMD_ALARM:
            ret = handle_cmd_alarm(connection, coninfo->post_data.data);
            break;
        case API_CMD_DECODE:
            ret = handle_cmd_decode(connection, coninfo->post_data.data);
            break;
        case API_CMD_REBOOT:
            ret = handle_cmd_reboot(connection);
            break;
        case API_REBOOT:
            ret = handle_reboot(connection);
            break;
        case API_SHUTDOWN:
            ret = handle_shutdown(connection);
            break;
        default:
            ret = json_response(connection, 404, "{\"error\":\"Endpoint not found\"}");
            break;
    }
    return ret;
}

static void request_completed_callback(void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe) {
    struct connection_info_struct *coninfo = *con_cls;
    if (coninfo) {
        if (coninfo->post_data.data)
            free(coninfo->post_data.data);
        free(coninfo);
    }
    *con_cls = NULL;
}

/* ==== Environment Variable Helpers ==== */
static void get_envstr(const char *env, char *out, size_t maxlen, const char *def) {
    const char *v = getenv(env);
    if (v && *v) strncpy(out, v, maxlen-1);
    else if (def) strncpy(out, def, maxlen-1);
    else out[0] = 0;
}

static int get_envint(const char *env, int def) {
    const char *v = getenv(env);
    if (v && *v) return atoi(v);
    return def;
}

/* ==== Main ==== */
int main(int argc, char *argv[]) {
    struct MHD_Daemon *daemon;
    int port = get_envint(ENV_SERVER_PORT, DEFAULT_SERVER_PORT);
    get_envstr(ENV_DEVICE_IP, g_device.ip, sizeof(g_device.ip), "192.168.1.100");
    g_device.port = get_envint(ENV_DEVICE_PORT, 8000);
    get_envstr(ENV_DEVICE_USER, g_device.username, sizeof(g_device.username), "admin");
    get_envstr(ENV_DEVICE_PASS, g_device.password, sizeof(g_device.password), "12345");

    daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY, port, NULL, NULL,
        &answer_to_connection, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed_callback, NULL,
        MHD_OPTION_END);

    if (NULL == daemon) {
        fprintf(stderr, "Failed to start HTTP server on port %d\n", port);
        return 1;
    }
    printf("HTTP driver listening on port %d\n", port);
    fflush(stdout);

    // Run forever
    while (1) sleep(60);

    MHD_stop_daemon(daemon);
    return 0;
}