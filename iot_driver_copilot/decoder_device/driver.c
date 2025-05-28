#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <microhttpd.h>
#include <pthread.h>
#include <jansson.h>
#include <errno.h>
#include <time.h>

// ----------------- Environment Variables -----------------
#define ENV_DEVICE_IP       "DEVICE_IP"
#define ENV_DEVICE_PORT     "DEVICE_PORT"
#define ENV_DEVICE_USER     "DEVICE_USER"
#define ENV_DEVICE_PASS     "DEVICE_PASS"
#define ENV_HTTP_HOST       "HTTP_HOST"
#define ENV_HTTP_PORT       "HTTP_PORT"

// ----------------- Config Defaults -----------------
#define DEFAULT_HTTP_HOST   "0.0.0.0"
#define DEFAULT_HTTP_PORT   8080
#define MAX_SESSIONS        32
#define SESSION_TOKEN_LEN   64

// ----------------- Hikvision SDK Stubs -----------------
// Real implementations should use Hikvision's SDK/API. Stubs below simulate device behavior.

typedef struct {
    int logged_in;
    char session_token[SESSION_TOKEN_LEN];
} DeviceSession;

typedef struct {
    DeviceSession sessions[MAX_SESSIONS];
    pthread_mutex_t lock;
} SessionTable;

SessionTable session_table = {.lock = PTHREAD_MUTEX_INITIALIZER};

// Simulate session management
int device_login(const char *username, const char *password, char *session_token) {
    pthread_mutex_lock(&session_table.lock);
    int i;
    for (i = 0; i < MAX_SESSIONS; ++i) {
        if (!session_table.sessions[i].logged_in) {
            snprintf(session_table.sessions[i].session_token, SESSION_TOKEN_LEN, "sess-%d-%ld", i, time(NULL));
            session_table.sessions[i].logged_in = 1;
            strncpy(session_token, session_table.sessions[i].session_token, SESSION_TOKEN_LEN);
            pthread_mutex_unlock(&session_table.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&session_table.lock);
    return -1;
}

int device_logout(const char *session_token) {
    pthread_mutex_lock(&session_table.lock);
    int i;
    for (i = 0; i < MAX_SESSIONS; ++i) {
        if (session_table.sessions[i].logged_in &&
            strncmp(session_table.sessions[i].session_token, session_token, SESSION_TOKEN_LEN) == 0) {
            session_table.sessions[i].logged_in = 0;
            session_table.sessions[i].session_token[0] = '\0';
            pthread_mutex_unlock(&session_table.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&session_table.lock);
    return -1;
}

int session_is_valid(const char *session_token) {
    int found = 0;
    pthread_mutex_lock(&session_table.lock);
    int i;
    for (i = 0; i < MAX_SESSIONS; ++i) {
        if (session_table.sessions[i].logged_in &&
            strncmp(session_table.sessions[i].session_token, session_token, SESSION_TOKEN_LEN) == 0) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&session_table.lock);
    return found;
}

// Simulated device state/config
static const char *device_state_json = "{ \"state\": \"running\", \"errors\": [], \"channels\": [{\"id\":1,\"status\":\"ok\"}] }";
static const char *device_config_json = "{ \"display\": {\"resolution\": \"1920x1080\"}, \"decode_channels\": [1,2,3], \"scene\": {\"mode\": \"normal\"} }";
static const char *device_ability_xml = "<AbilitySet><Channel>32</Channel></AbilitySet>";

// Simulated command implementations
int device_alarm_control(const char *action, json_t *params) { return 0; }
int device_decode_control(const char *action, json_t *params) { return 0; }
int device_reboot() { return 0; }
int device_get_config(json_t **result) {
    json_error_t jerr;
    *result = json_loads(device_config_json, 0, &jerr);
    return *result ? 0 : -1;
}
int device_update_config(json_t *settings) {
    // Simulate update
    return 0;
}
int device_get_state(json_t **result) {
    json_error_t jerr;
    *result = json_loads(device_state_json, 0, &jerr);
    return *result ? 0 : -1;
}

// ----------------- Utility Functions -----------------
const char* getenv_default(const char* var, const char* def) {
    const char *val = getenv(var);
    return val ? val : def;
}

int getenv_int_default(const char* var, int def) {
    const char *val = getenv(var);
    return val ? atoi(val) : def;
}

void send_json_response(struct MHD_Connection *connection, int status, json_t *json_obj) {
    char *json_str = json_dumps(json_obj, JSON_COMPACT);
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(json_str), (void*)json_str, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
}

void send_error(struct MHD_Connection *connection, int code, const char *message) {
    json_t *err = json_object();
    json_object_set_new(err, "error", json_string(message));
    send_json_response(connection, code, err);
    json_decref(err);
}

char *get_post_data(struct MHD_Connection *connection, const char *upload_data, size_t *upload_data_size) {
    char *data = NULL;
    if (*upload_data_size > 0) {
        data = malloc(*upload_data_size + 1);
        memcpy(data, upload_data, *upload_data_size);
        data[*upload_data_size] = '\0';
        *upload_data_size = 0;
    }
    return data;
}

char *get_token_from_header(struct MHD_Connection *connection) {
    const char *header = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
    if (!header) return NULL;
    if (strncmp(header, "Bearer ", 7) == 0)
        return strdup(header + 7);
    return NULL;
}

// ----------------- HTTP Handlers -----------------
typedef struct {
    char *post_data;
    size_t post_data_size;
} ClientContext;

static int handle_login(struct MHD_Connection *connection, const char *upload_data, size_t *upload_data_size, ClientContext *ctx) {
    if (*upload_data_size != 0) {
        ctx->post_data = get_post_data(connection, upload_data, upload_data_size);
        return MHD_YES;
    }
    // After data received
    json_error_t jerr;
    json_t *json = json_loads(ctx->post_data, 0, &jerr);
    free(ctx->post_data);
    if (!json) {
        send_error(connection, MHD_HTTP_BAD_REQUEST, "Invalid JSON");
        return MHD_YES;
    }
    const char *username = json_string_value(json_object_get(json, "username"));
    const char *password = json_string_value(json_object_get(json, "password"));
    char session_token[SESSION_TOKEN_LEN] = {0};
    if (username && password && device_login(username, password, session_token) == 0) {
        json_t *resp = json_object();
        json_object_set_new(resp, "token", json_string(session_token));
        send_json_response(connection, MHD_HTTP_OK, resp);
        json_decref(resp);
    } else {
        send_error(connection, MHD_HTTP_UNAUTHORIZED, "Login failed");
    }
    json_decref(json);
    return MHD_YES;
}

static int handle_logout(struct MHD_Connection *connection, const char *upload_data, size_t *upload_data_size, ClientContext *ctx) {
    char *token = get_token_from_header(connection);
    if (!token) {
        send_error(connection, MHD_HTTP_UNAUTHORIZED, "Missing token");
        return MHD_YES;
    }
    if (device_logout(token) == 0) {
        json_t *resp = json_object();
        json_object_set_new(resp, "result", json_string("logged out"));
        send_json_response(connection, MHD_HTTP_OK, resp);
        json_decref(resp);
    } else {
        send_error(connection, MHD_HTTP_UNAUTHORIZED, "Invalid session");
    }
    free(token);
    return MHD_YES;
}

static int handle_get_config(struct MHD_Connection *connection) {
    json_t *result = NULL;
    if (device_get_config(&result) == 0) {
        send_json_response(connection, MHD_HTTP_OK, result);
        json_decref(result);
    } else {
        send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to get config");
    }
    return MHD_YES;
}

static int handle_put_config(struct MHD_Connection *connection, const char *upload_data, size_t *upload_data_size, ClientContext *ctx) {
    if (*upload_data_size != 0) {
        ctx->post_data = get_post_data(connection, upload_data, upload_data_size);
        return MHD_YES;
    }
    json_error_t jerr;
    json_t *json = json_loads(ctx->post_data, 0, &jerr);
    free(ctx->post_data);
    if (!json) {
        send_error(connection, MHD_HTTP_BAD_REQUEST, "Invalid JSON");
        return MHD_YES;
    }
    if (device_update_config(json) == 0) {
        json_t *resp = json_object();
        json_object_set_new(resp, "result", json_string("config updated"));
        send_json_response(connection, MHD_HTTP_OK, resp);
        json_decref(resp);
    } else {
        send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Update failed");
    }
    json_decref(json);
    return MHD_YES;
}

static int handle_alarm_cmd(struct MHD_Connection *connection, const char *upload_data, size_t *upload_data_size, ClientContext *ctx) {
    if (*upload_data_size != 0) {
        ctx->post_data = get_post_data(connection, upload_data, upload_data_size);
        return MHD_YES;
    }
    json_error_t jerr;
    json_t *json = json_loads(ctx->post_data, 0, &jerr);
    free(ctx->post_data);
    if (!json) {
        send_error(connection, MHD_HTTP_BAD_REQUEST, "Invalid JSON");
        return MHD_YES;
    }
    const char *action = json_string_value(json_object_get(json, "action"));
    if (!action) {
        send_error(connection, MHD_HTTP_BAD_REQUEST, "Missing action");
        json_decref(json);
        return MHD_YES;
    }
    if (device_alarm_control(action, json) == 0) {
        json_t *resp = json_object();
        json_object_set_new(resp, "result", json_string("alarm command executed"));
        send_json_response(connection, MHD_HTTP_OK, resp);
        json_decref(resp);
    } else {
        send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Command failed");
    }
    json_decref(json);
    return MHD_YES;
}

static int handle_decode_cmd(struct MHD_Connection *connection, const char *upload_data, size_t *upload_data_size, ClientContext *ctx) {
    if (*upload_data_size != 0) {
        ctx->post_data = get_post_data(connection, upload_data, upload_data_size);
        return MHD_YES;
    }
    json_error_t jerr;
    json_t *json = json_loads(ctx->post_data, 0, &jerr);
    free(ctx->post_data);
    if (!json) {
        send_error(connection, MHD_HTTP_BAD_REQUEST, "Invalid JSON");
        return MHD_YES;
    }
    const char *action = json_string_value(json_object_get(json, "action"));
    if (!action) {
        send_error(connection, MHD_HTTP_BAD_REQUEST, "Missing action");
        json_decref(json);
        return MHD_YES;
    }
    if (device_decode_control(action, json) == 0) {
        json_t *resp = json_object();
        json_object_set_new(resp, "result", json_string("decode command executed"));
        send_json_response(connection, MHD_HTTP_OK, resp);
        json_decref(resp);
    } else {
        send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Command failed");
    }
    json_decref(json);
    return MHD_YES;
}

static int handle_reboot_cmd(struct MHD_Connection *connection) {
    if (device_reboot() == 0) {
        json_t *resp = json_object();
        json_object_set_new(resp, "result", json_string("device rebooted"));
        send_json_response(connection, MHD_HTTP_OK, resp);
        json_decref(resp);
    } else {
        send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Reboot failed");
    }
    return MHD_YES;
}

static int handle_get_state(struct MHD_Connection *connection) {
    json_t *result = NULL;
    if (device_get_state(&result) == 0) {
        send_json_response(connection, MHD_HTTP_OK, result);
        json_decref(result);
    } else {
        send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to get state");
    }
    return MHD_YES;
}

// ----------------- Dispatcher -----------------
static int request_handler(void *cls, struct MHD_Connection *connection,
                          const char *url, const char *method,
                          const char *version, const char *upload_data,
                          size_t *upload_data_size, void **con_cls) {
    static const struct {
        const char *path;
        const char *method;
        int (*handler)(struct MHD_Connection*, const char*, size_t*, ClientContext*);
        int requires_auth;
    } routes[] = {
        {"/login", "POST", handle_login, 0},
        {"/logout", "POST", handle_logout, 1},
        {"/config", "GET", NULL, 1},
        {"/config", "PUT", handle_put_config, 1},
        {"/cmd/alarm", "POST", handle_alarm_cmd, 1},
        {"/cmd/decode", "POST", handle_decode_cmd, 1},
        {"/cmd/reboot", "POST", NULL, 1},
        {"/state", "GET", NULL, 1},
        {NULL, NULL, NULL, 0}
    };
    int i;
    ClientContext *ctx = *con_cls;
    if (!ctx) {
        ctx = calloc(1, sizeof(ClientContext));
        *con_cls = ctx;
        return MHD_YES;
    }
    // Find route
    for (i = 0; routes[i].path; ++i) {
        if (strcmp(url, routes[i].path) == 0 && strcmp(method, routes[i].method) == 0) {
            if (routes[i].requires_auth) {
                char *token = get_token_from_header(connection);
                if (!token || !session_is_valid(token)) {
                    send_error(connection, MHD_HTTP_UNAUTHORIZED, "Invalid or missing token");
                    free(token);
                    return MHD_YES;
                }
                free(token);
            }
            if (strcmp(url, "/config") == 0 && strcmp(method, "GET") == 0)
                return handle_get_config(connection);
            if (strcmp(url, "/cmd/reboot") == 0 && strcmp(method, "POST") == 0)
                return handle_reboot_cmd(connection);
            if (strcmp(url, "/state") == 0 && strcmp(method, "GET") == 0)
                return handle_get_state(connection);
            return routes[i].handler(connection, upload_data, upload_data_size, ctx);
        }
    }
    send_error(connection, MHD_HTTP_NOT_FOUND, "Not found");
    return MHD_YES;
}

static void request_completed(void *cls, struct MHD_Connection *connection,
                             void **con_cls, enum MHD_RequestTerminationCode toe) {
    ClientContext *ctx = *con_cls;
    if (ctx) {
        if (ctx->post_data) free(ctx->post_data);
        free(ctx);
    }
}

// ----------------- Main -----------------
int main(int argc, char *argv[]) {
    const char *http_host = getenv_default(ENV_HTTP_HOST, DEFAULT_HTTP_HOST);
    int http_port = getenv_int_default(ENV_HTTP_PORT, DEFAULT_HTTP_PORT);

    printf("Starting HTTP server on %s:%d\n", http_host, http_port);

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY, http_port, NULL, NULL,
        &request_handler, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
        MHD_OPTION_END
    );

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTP server\n");
        return 1;
    }

    getchar(); // Press Enter to stop
    MHD_stop_daemon(daemon);
    return 0;
}