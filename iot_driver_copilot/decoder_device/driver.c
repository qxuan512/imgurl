```c
// hikvision_decoder_http_driver.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <microhttpd.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <jansson.h>

// ----------------- Device SDK Mockup/Placeholder Functions ----------------- //
// In real implementation, link against Hikvision SDK and use actual API calls.

typedef struct {
    bool logged_in;
    char session_token[64];
} DeviceSession;

static DeviceSession device_session = { false, "" };

static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;

static int device_login(const char* username, const char* password, char* session_token, size_t token_len) {
    pthread_mutex_lock(&device_mutex);
    if (device_session.logged_in) {
        pthread_mutex_unlock(&device_mutex);
        return -2; // Already logged in
    }
    // Mock: accept "admin"/"12345"
    if (strcmp(username, "admin") == 0 && strcmp(password, "12345") == 0) {
        snprintf(session_token, token_len, "SESSION%ld", time(NULL));
        strcpy(device_session.session_token, session_token);
        device_session.logged_in = true;
        pthread_mutex_unlock(&device_mutex);
        return 0; // Success
    }
    pthread_mutex_unlock(&device_mutex);
    return -1; // Fail
}

static int device_logout(const char* session_token) {
    pthread_mutex_lock(&device_mutex);
    if (device_session.logged_in && strcmp(session_token, device_session.session_token) == 0) {
        device_session.logged_in = false;
        device_session.session_token[0] = 0;
        pthread_mutex_unlock(&device_mutex);
        return 0;
    }
    pthread_mutex_unlock(&device_mutex);
    return -1;
}

static bool device_is_logged_in(const char* token) {
    pthread_mutex_lock(&device_mutex);
    bool r = (device_session.logged_in && strcmp(token, device_session.session_token) == 0);
    pthread_mutex_unlock(&device_mutex);
    return r;
}

static json_t* device_get_status(void) {
    json_t* status = json_object();
    json_object_set_new(status, "device", json_string("Decoder Device"));
    json_object_set_new(status, "state", json_string("running"));
    json_object_set_new(status, "channels_active", json_integer(8));
    json_object_set_new(status, "alarm", json_string("none"));
    json_object_set_new(status, "error_code", json_integer(0));
    return status;
}

static json_t* device_get_config(const char* filter) {
    json_t* config = json_object();
    if (!filter || strstr(filter, "display")) {
        json_object_set_new(config, "display_channels", json_integer(4));
    }
    if (!filter || strstr(filter, "decode")) {
        json_object_set_new(config, "decode_channels", json_integer(8));
    }
    if (!filter || strstr(filter, "scene")) {
        json_object_set_new(config, "scene_mode", json_string("default"));
    }
    return config;
}

static int device_update_config(json_t* payload) {
    // Accept any JSON payload as OK (mock)
    return 0;
}

static int device_alarm_control(const char* action, json_t* params) {
    // Accept "start"/"stop"
    if (strcmp(action, "start") == 0 || strcmp(action, "stop") == 0)
        return 0;
    return -1;
}

static int device_decode_control(const char* action, json_t* params) {
    // Accept "start"/"stop"
    if (strcmp(action, "start") == 0 || strcmp(action, "stop") == 0)
        return 0;
    return -1;
}

static int device_reboot(void) {
    return 0; // success
}

static int device_shutdown(void) {
    return 0; // success
}

static json_t* device_get_state(void) {
    json_t* state = json_object();
    json_object_set_new(state, "operational_status", json_string("OK"));
    json_object_set_new(state, "error_codes", json_integer(0));
    json_object_set_new(state, "channel_conditions", json_string("normal"));
    return state;
}

// ---------------------- HTTP Server Configuration -------------------------- //

#define POSTBUFFERSIZE 8192
#define MAX_JSON_SIZE 65536

typedef struct {
    char* data;
    size_t size;
} post_data_t;

typedef struct {
    char session_token[64];
} connection_info_t;

typedef struct {
    char* post_data;
    size_t post_data_size;
} request_context_t;

static const char* get_env(const char* name, const char* defval) {
    const char* v = getenv(name);
    return v ? v : defval;
}

static const char* SERVER_HOST;
static int SERVER_PORT;
static const char* DEVICE_IP;
static int DEVICE_PORT;
static const char* DEVICE_USER;
static const char* DEVICE_PASS;

// ---------------------- Helper Functions ----------------------------------- //

static int send_json_response(struct MHD_Connection* connection, int status, json_t* json) {
    char* response_str = json_dumps(json, JSON_COMPACT);
    struct MHD_Response* response = MHD_create_response_from_buffer(strlen(response_str), (void*)response_str, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(response, "Content-Type", "application/json");
    int ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

static int send_simple_response(struct MHD_Connection* connection, int status, const char* msg) {
    struct MHD_Response* response = MHD_create_response_from_buffer(strlen(msg), (void*)msg, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(response, "Content-Type", "text/plain");
    int ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

static const char* get_session_token(struct MHD_Connection* connection) {
    const char* token = MHD_lookup_connection_value(connection, MHD_COOKIE_KIND, "session_token");
    if (!token) {
        token = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-Session-Token");
    }
    return token;
}

// ---------------------- API Handlers --------------------------------------- //

static int handle_login(struct MHD_Connection* connection, const char* upload_data, size_t* upload_data_size, request_context_t* context) {
    if (*upload_data_size != 0) {
        // Accumulate POST data
        size_t oldsize = context->post_data_size;
        context->post_data = realloc(context->post_data, oldsize + *upload_data_size + 1);
        memcpy(context->post_data + oldsize, upload_data, *upload_data_size);
        context->post_data_size = oldsize + *upload_data_size;
        context->post_data[context->post_data_size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }
    // Parse JSON
    json_error_t err;
    json_t* root = json_loads(context->post_data, 0, &err);
    if (!root) {
        return send_simple_response(connection, MHD_HTTP_BAD_REQUEST, "Invalid JSON");
    }
    const char* username = json_string_value(json_object_get(root, "username"));
    const char* password = json_string_value(json_object_get(root, "password"));
    char session_token[64] = "";
    int rv = device_login(username, password, session_token, sizeof(session_token));
    json_decref(root);
    if (rv == 0) {
        json_t* resp = json_object();
        json_object_set_new(resp, "session_token", json_string(session_token));
        struct MHD_Response* response = MHD_create_response_from_buffer(strlen(json_dumps(resp, JSON_COMPACT)), (void*)json_dumps(resp, JSON_COMPACT), MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(response, "Set-Cookie", session_token);
        MHD_add_response_header(response, "Content-Type", "application/json");
        MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        json_decref(resp);
        return MHD_YES;
    } else if (rv == -2) {
        return send_simple_response(connection, MHD_HTTP_CONFLICT, "Already logged in");
    } else {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Login failed");
    }
}

static int handle_logout(struct MHD_Connection* connection) {
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    if (device_logout(token) == 0) {
        json_t* resp = json_object();
        json_object_set_new(resp, "message", json_string("Logged out"));
        int ret = send_json_response(connection, MHD_HTTP_OK, resp);
        json_decref(resp);
        return ret;
    }
    return send_simple_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Logout failed");
}

static int handle_get_status(struct MHD_Connection* connection) {
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    json_t* status = device_get_status();
    int ret = send_json_response(connection, MHD_HTTP_OK, status);
    json_decref(status);
    return ret;
}

static int handle_get_state(struct MHD_Connection* connection) {
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    json_t* state = device_get_state();
    int ret = send_json_response(connection, MHD_HTTP_OK, state);
    json_decref(state);
    return ret;
}

static int handle_get_config(struct MHD_Connection* connection, const char* filter) {
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    json_t* config = device_get_config(filter);
    int ret = send_json_response(connection, MHD_HTTP_OK, config);
    json_decref(config);
    return ret;
}

static int handle_update_config(struct MHD_Connection* connection, const char* upload_data, size_t* upload_data_size, request_context_t* context) {
    if (*upload_data_size != 0) {
        // Accumulate POST data
        size_t oldsize = context->post_data_size;
        context->post_data = realloc(context->post_data, oldsize + *upload_data_size + 1);
        memcpy(context->post_data + oldsize, upload_data, *upload_data_size);
        context->post_data_size = oldsize + *upload_data_size;
        context->post_data[context->post_data_size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    json_error_t err;
    json_t* root = json_loads(context->post_data, 0, &err);
    if (!root) {
        return send_simple_response(connection, MHD_HTTP_BAD_REQUEST, "Invalid JSON");
    }
    int rv = device_update_config(root);
    json_decref(root);
    if (rv == 0) {
        return send_simple_response(connection, MHD_HTTP_OK, "Config updated");
    }
    return send_simple_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Config update failed");
}

static int handle_alarm(struct MHD_Connection* connection, const char* upload_data, size_t* upload_data_size, request_context_t* context) {
    if (*upload_data_size != 0) {
        size_t oldsize = context->post_data_size;
        context->post_data = realloc(context->post_data, oldsize + *upload_data_size + 1);
        memcpy(context->post_data + oldsize, upload_data, *upload_data_size);
        context->post_data_size = oldsize + *upload_data_size;
        context->post_data[context->post_data_size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    json_error_t err;
    json_t* root = json_loads(context->post_data, 0, &err);
    if (!root) {
        return send_simple_response(connection, MHD_HTTP_BAD_REQUEST, "Invalid JSON");
    }
    const char* action = json_string_value(json_object_get(root, "action"));
    int rv = device_alarm_control(action, root);
    json_decref(root);
    if (rv == 0) {
        return send_simple_response(connection, MHD_HTTP_OK, "Alarm action performed");
    }
    return send_simple_response(connection, MHD_HTTP_BAD_REQUEST, "Invalid alarm action");
}

static int handle_decode(struct MHD_Connection* connection, const char* upload_data, size_t* upload_data_size, request_context_t* context) {
    if (*upload_data_size != 0) {
        size_t oldsize = context->post_data_size;
        context->post_data = realloc(context->post_data, oldsize + *upload_data_size + 1);
        memcpy(context->post_data + oldsize, upload_data, *upload_data_size);
        context->post_data_size = oldsize + *upload_data_size;
        context->post_data[context->post_data_size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    json_error_t err;
    json_t* root = json_loads(context->post_data, 0, &err);
    if (!root) {
        return send_simple_response(connection, MHD_HTTP_BAD_REQUEST, "Invalid JSON");
    }
    const char* action = json_string_value(json_object_get(root, "action"));
    int rv = device_decode_control(action, root);
    json_decref(root);
    if (rv == 0) {
        return send_simple_response(connection, MHD_HTTP_OK, "Decode action performed");
    }
    return send_simple_response(connection, MHD_HTTP_BAD_REQUEST, "Invalid decode action");
}

static int handle_reboot(struct MHD_Connection* connection) {
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    if (device_reboot() == 0) {
        return send_simple_response(connection, MHD_HTTP_OK, "Reboot initiated");
    }
    return send_simple_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Reboot failed");
}

static int handle_shutdown(struct MHD_Connection* connection) {
    const char* token = get_session_token(connection);
    if (!token || !device_is_logged_in(token)) {
        return send_simple_response(connection, MHD_HTTP_UNAUTHORIZED, "Not logged in");
    }
    if (device_shutdown() == 0) {
        return send_simple_response(connection, MHD_HTTP_OK, "Shutdown initiated");
    }
    return send_simple_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Shutdown failed");
}

// ---------------------- HTTP Routing --------------------------------------- //

struct handler_map_entry {
    const char* method;
    const char* path;
    int (*handler)(struct MHD_Connection*, const char*, size_t*, request_context_t*);
    int (*simple_handler)(struct MHD_Connection*);
};

static struct handler_map_entry handler_map[] = {
    {"POST", "/login", handle_login, NULL},
    {"POST", "/logout", NULL, handle_logout},
    {"GET",  "/status", NULL, handle_get_status},
    {"GET",  "/state", NULL, handle_get_state},
    {"GET",  "/config", NULL, NULL}, // filter handled in dispatcher
    {"PUT",  "/config", handle_update_config, NULL},
    {"POST", "/cmd/alarm", handle_alarm, NULL},
    {"POST", "/cmd/decode", handle_decode, NULL},
    {"POST", "/cmd/reboot", NULL, handle_reboot},
    {"POST", "/reboot", NULL, handle_reboot},
    {"POST", "/shutdown", NULL, handle_shutdown},
    {NULL, NULL, NULL, NULL}
};

static int dispatcher(void* cls, struct MHD_Connection* connection,
                      const char* url, const char* method,
                      const char* version, const char* upload_data,
                      size_t* upload_data_size, void** con_cls) {
    request_context_t* ctx = *con_cls;
    if (!ctx) {
        ctx = calloc(1, sizeof(request_context_t));
        *con_cls = ctx;
        return MHD_YES;
    }
    // Find handler
    for (int i = 0; handler_map[i].method != NULL; ++i) {
        if (strcmp(handler_map[i].method, method) == 0 &&
            strcmp(handler_map[i].path, url) == 0) {
            if (handler_map[i].handler)
                return handler_map[i].handler(connection, upload_data, upload_data_size, ctx);
            if (handler_map[i].simple_handler)
                return handler_map[i].simple_handler(connection);
        }
    }
    // /config GET special: allow filter by query string
    if (strcmp(method, "GET") == 0 && strcmp(url, "/config") == 0) {
        const char* filter = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "filter");
        int r = handle_get_config(connection, filter);
        free(ctx->post_data);
        free(ctx);
        *con_cls = NULL;
        return r;
    }
    // Method not allowed
    if (*upload_data_size != 0) { *upload_data_size = 0; }
    free(ctx->post_data);
    free(ctx);
    *con_cls = NULL;
    return send_simple_response(connection, MHD_HTTP_NOT_FOUND, "Not found");
}

static void request_completed_callback(void* cls, struct MHD_Connection* connection,
                                      void** con_cls, enum MHD_RequestTerminationCode toe) {
    request_context_t* ctx = *con_cls;
    if (ctx) {
        free(ctx->post_data);
        free(ctx);
        *con_cls = NULL;
    }
}

// ---------------------- MAIN ----------------------------------------------- //

int main(int argc, char* argv[]) {
    SERVER_HOST = get_env("SERVER_HOST", "0.0.0.0");
    SERVER_PORT = atoi(get_env("SERVER_PORT", "8080"));
    DEVICE_IP   = get_env("DEVICE_IP", "192.168.1.100");
    DEVICE_PORT = atoi(get_env("DEVICE_PORT", "8000"));
    DEVICE_USER = get_env("DEVICE_USER", "admin");
    DEVICE_PASS = get_env("DEVICE_PASS", "12345");

    struct MHD_Daemon* daemon;
    daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, SERVER_PORT, NULL, NULL,
                              &dispatcher, NULL, 
                              MHD_OPTION_NOTIFY_COMPLETED, request_completed_callback, NULL,
                              MHD_OPTION_END);
    if (daemon == NULL) {
        fprintf(stderr, "Failed to start HTTP server on %s:%d\n", SERVER_HOST, SERVER_PORT);
        return 1;
    }
    printf("Hikvision Decoder HTTP Driver running at http://%s:%d\n", SERVER_HOST, SERVER_PORT);
    fflush(stdout);
    while(1) {
        sleep(1);
    }
    MHD_stop_daemon(daemon);
    return 0;
}
```
