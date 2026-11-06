#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/server.h"
#include "gitstored/core.h"
#include "gitstored/inspect.h"
#include "gitstored/paths.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "gitstored/validate.h"
#include "gitstored/wait.h"
#include "runtime_manager.h"
#include "runtime_settings.h"
#include "stream_service.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd, negotiated;
    unsigned slot;
    int subscribing;
    char subscribe_request[65];
    gstd_frame frame;
    unsigned char *output;
    size_t output_size, output_sent;
    double traffic_at, frame_at;
    int waiting;
    double wait_deadline;
    char wait_operation[65], wait_request[65], wait_until[16];
} connection;
static volatile sig_atomic_t stopping;
static sqlite3 *database;
static gstd_runtime_settings runtime_settings = GSTD_RUNTIME_SETTINGS_INITIAL;
static gstd_runtime_manager *runtime_manager;
static gstd_stream_service *stream_service;
static void interrupt_server(int signal_number) {
    (void)signal_number;
    stopping = 1;
}
static double monotonic(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return 0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}
static int peer_allowed(int fd) {
#ifdef __linux__
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
           credentials.uid == geteuid();
#elif defined(__APPLE__) || defined(__FreeBSD__)
    uid_t uid;
    gid_t gid;
    return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
#else
    (void)fd;
    return 0;
#endif
}
#ifdef GITSTORED_TESTING
/* Isolated credential-boundary tests only; no wire command or bypass. */
int gstd_test_peer_allowed(int fd) { return peer_allowed(fd); }
#endif

static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
                   fcntl(fd, F_SETFD, FD_CLOEXEC) < 0
               ? -1
               : 0;
}
static void close_connection(connection *c) {
    unsigned slot = c->slot;
    if (c->fd >= 0)
        gstd_stream_service_disconnect(stream_service, slot);
    if (c->fd >= 0)
        close(c->fd);
    gstd_frame_reset(&c->frame);
    free(c->output);
    memset(c, 0, sizeof(*c));
    c->slot = slot;
    c->fd = -1;
}
static json_t *error_value(const char *code, const char *stage, const char *message) {
    return json_pack("{s:s,s:s,s:b,s:s}", "code", code, "stage", stage, "retryable", 0, "message",
                     message);
}
static json_t *capabilities(void) {
    static const struct {
        const char *name;
        json_int_t value;
    } limits[] = {{"frameBytes", 1048576},
                  {"nesting", 32},
                  {"stringBytes", 262144},
                  {"collectionItems", 4096},
                  {"connections", 32},
                  {"outstandingPerConnection", 16},
                  {"subscriptionsPerConnection", 8},
                  {"queuedEventsPerSubscription", 256},
                  {"queuedBytesPerSubscription", 4194304},
                  {"globalQueuedBytes", 67108864},
                  {"logsPerDeploymentBytes", 67108864},
                  {"logMaxAgeDays", 7},
                  {"globalLogBytes", 1073741824},
                  {"workers", 8},
                  {"mutationPerDeployment", 1},
                  {"pendingJobs", 64},
                  {"partialFrameMs", 10000},
                  {"idleMs", 300000},
                  {"stopMs", 30000},
                  {"shutdownMs", 30000},
                  {"operationMetadataBytes", 268435456},
                  {"operationRecords", 100000},
                  {"terminalRetentionDays", 7}};
    json_t *result = json_pack("{s:{s:i,s:i},s:[s,s],s:{}}", "selectedVersion", "major", 1, "minor",
                               0, "capabilities", "system.read", "deployment.validate", "limits");
    if (!result)
        return NULL;
    if (json_object_set_new(result, "implementationVersion", json_string(gstd_version()))) {
        json_decref(result);
        return NULL;
    }
    if (database) {
        if (json_array_append_new(json_object_get(result, "capabilities"),
                                  json_string("deployment.read")) ||
            json_array_append_new(json_object_get(result, "capabilities"),
                                  json_string("operation.read"))) {
            json_decref(result);
            return NULL;
        }
    }
    if (gstd_runtime_manager_capable(runtime_manager)) {
        json_t *names = json_object_get(result, "capabilities");
        if (json_array_append_new(names, json_string("deployment.oidc")) ||
            json_array_append_new(names, json_string("deployment.start")) ||
            json_array_append_new(names, json_string("deployment.stop")) ||
            json_array_append_new(names, json_string("operation.cancel")) ||
            json_array_append_new(names, json_string("migration.start"))) {
            json_decref(result);
            return NULL;
        }
    }
    if (stream_service && json_array_append_new(json_object_get(result, "capabilities"),
                                                json_string("stream.read"))) {
        json_decref(result);
        return NULL;
    }
    json_t *object = json_object_get(result, "limits");
    for (size_t i = 0; i < sizeof(limits) / sizeof(*limits); ++i) {
        if (json_object_set_new(object, limits[i].name, json_integer(limits[i].value))) {
            json_decref(result);
            return NULL;
        }
    }
    return result;
}
static int set_frame(connection *c, const json_t *response) {
    size_t length = json_dumpb(response, NULL, 0, JSON_COMPACT);
    if (!length || length > GSTD_FRAME_MAX || c->output)
        return -1;
    c->output = malloc(length + 4);
    if (!c->output)
        return -1;
    c->output[0] = (unsigned char)(length >> 24);
    c->output[1] = (unsigned char)(length >> 16);
    c->output[2] = (unsigned char)(length >> 8);
    c->output[3] = (unsigned char)length;
    size_t written = json_dumpb(response, (char *)c->output + 4, length, JSON_COMPACT);
    if (written != length) {
        free(c->output);
        c->output = NULL;
        return -1;
    }
    c->output_size = length + 4;
    c->output_sent = 0;
    return 0;
}
static int set_response(connection *c, json_t *id, json_t *value, int error) {
    json_t *response = json_object();
    if (!response || !value) {
        json_decref(response);
        json_decref(value);
        return -1;
    }
    json_object_set_new(response, "version", json_pack("{s:i,s:i}", "major", 1, "minor", 0));
    json_object_set(response, "requestId", id ? id : json_null());
    json_object_set_new(response, error ? "error" : "result", value);
    int status = set_frame(c, response);
    json_decref(response);
    return status;
}
static json_t *query_error(gstd_query_status status) {
    const char *code = status == GSTD_QUERY_INVALID          ? "INVALID_ARGUMENT"
                       : status == GSTD_QUERY_NOT_FOUND      ? "NOT_FOUND"
                       : status == GSTD_QUERY_CURSOR_EXPIRED ? "CURSOR_EXPIRED"
                       : status == GSTD_QUERY_LIMIT          ? "RESOURCE_LIMIT"
                                                             : "INTERNAL";
    return error_value(code, "observe",
                       status == GSTD_QUERY_NOT_FOUND ? "Record not found or retention expired"
                       : status == GSTD_QUERY_CURSOR_EXPIRED
                           ? "Cursor is foreign, stale or expired; restart the listing"
                           : "Local observation could not complete");
}
static gstd_query_status wait_poll(const char *operation, const char *until, json_t **result) {
    *result = NULL;
    if (!strcmp(until, "ready") &&
        gstd_runtime_manager_observation_blocked(runtime_manager, NULL, operation))
        return GSTD_QUERY_PENDING;
    return gstd_wait_poll(database, operation, until, result);
}
static int handle(connection *c) {
    json_t *message = gstd_json_parse(c->frame.payload, c->frame.used);
    if (!message)
        return -1;
    gstd_request request;
    int valid = gstd_request_parse(message, &request);
    json_t *id = json_object_get(message, "requestId");
    if (!json_is_string(id) || !gstd_identifier(json_string_value(id), json_string_length(id)))
        id = NULL;
    json_t *result = NULL;
    int error = 1;
    if (valid)
        result = error_value(valid == -2 ? "UNSUPPORTED_CAPABILITY" : "INVALID_ARGUMENT",
                             "validate", "Unsupported method or invalid parameters");
    else if (request.version.major != 1 || request.version.minor != 0)
        result = error_value("INCOMPATIBLE_VERSION", "negotiate", "Use local protocol 1.0");
    else if (!c->negotiated && request.method != GSTD_NEGOTIATE)
        result =
            error_value("NEGOTIATION_REQUIRED", "negotiate", "Negotiate before other requests");
    else if (request.method == GSTD_NEGOTIATE) {
        int compatible = 0, missing = 0;
        json_t *supported = json_object_get(request.params, "supported"),
               *required = json_object_get(request.params, "requiredCapabilities");
        for (size_t i = 0; i < json_array_size(supported); ++i)
            if (json_integer_value(json_object_get(json_array_get(supported, i), "major")) == 1)
                compatible = 1;
        json_t *available = capabilities();
        if (!available) {
            json_decref(message);
            return -1;
        }
        json_t *names = json_object_get(available, "capabilities");
        for (size_t i = 0; i < json_array_size(required); i++) {
            int found = 0;
            for (size_t j = 0; j < json_array_size(names); j++)
                if (json_equal(json_array_get(required, i), json_array_get(names, j)))
                    found = 1;
            if (!found)
                missing = 1;
        }
        json_decref(available);
        if (c->negotiated)
            result = error_value("INVALID_ARGUMENT", "negotiate",
                                 "Use a new connection to negotiate again");
        else if (!compatible)
            result = error_value("INCOMPATIBLE_VERSION", "negotiate", "No common protocol major");
        else if (missing)
            result = error_value("UNSUPPORTED_CAPABILITY", "negotiate",
                                 "Required capability is unavailable");
        else {
            c->negotiated = 1;
            result = capabilities();
            error = 0;
        }
    } else if (request.method == GSTD_CAPABILITIES) {
        result = capabilities();
        error = 0;
    } else if (stream_service && request.method == GSTD_SUBSCRIBE) {
        int pending =
            gstd_stream_service_subscribe(stream_service, c->slot, request.params, &result, &error);
        if (pending < 0) {
            json_decref(message);
            return -1;
        }
        if (pending) {
            c->subscribing = 1;
            snprintf(c->subscribe_request, sizeof(c->subscribe_request), "%s", request.request_id);
            json_decref(message);
            gstd_frame_reset(&c->frame);
            c->frame_at = 0;
            return 0;
        }
    } else if (stream_service && request.method == GSTD_UNSUBSCRIBE) {
        result = gstd_stream_service_unsubscribe(
            stream_service, c->slot,
            json_string_value(json_object_get(request.params, "subscriptionId")), &error);
    } else if (request.method == GSTD_START || request.method == GSTD_STOP ||
               request.method == GSTD_CANCEL || request.method == GSTD_MIGRATE) {
        result = gstd_runtime_manager_submit(runtime_manager, &request, &error);
    } else if (request.method == GSTD_VALIDATE) {
        result = gstd_validate(request.params);
        error = 0;
        if (runtime_manager && result && gstd_runtime_manager_assess(runtime_manager, result)) {
            json_decref(result);
            result = error_value("INTERNAL", "validate", "Local assessment unavailable");
            error = 1;
        }
    } else if (database && (request.method == GSTD_LIST || request.method == GSTD_INSPECT ||
                            request.method == GSTD_OPERATION_INSPECT)) {
        gstd_query_status status = gstd_inspect(database, request.method, request.params, &result);
        error = status != GSTD_QUERY_OK;
        if (error)
            result = query_error(status);
        else if (request.method != GSTD_OPERATION_INSPECT &&
                 gstd_runtime_manager_observation_blocked(
                     runtime_manager,
                     json_string_value(json_object_get(request.params, "deploymentId")), NULL)) {
            if (request.method == GSTD_LIST) {
                json_decref(result);
                result = error_value(
                    "OWNERSHIP_UNCERTAIN", "observe",
                    "Current readiness could not be durably recorded; inspect the deployment");
                error = 1;
            } else {
                /* Expose best known current uncertainty without blocking
                 * inspection of immutable operation history or claiming the
                 * cached durable ready row is still live evidence. */
                if (json_object_set_new(result, "readiness", json_string("failed")) ||
                    json_object_set_new(result, "recovery", json_string("uncertain"))) {
                    json_decref(result);
                    result = query_error(GSTD_QUERY_DATABASE);
                    error = 1;
                } else
                    json_object_del(result, "endpoints");
            }
        }
    } else if (database && request.method == GSTD_WAIT) {
        const char *operation = json_string_value(json_object_get(request.params, "operationId"));
        const char *until = json_string_value(json_object_get(request.params, "until"));
        gstd_query_status status = wait_poll(operation, until, &result);
        if (status == GSTD_QUERY_PENDING) {
            c->waiting = 1;
            c->wait_deadline = monotonic() + (double)request.timeout_ms / 1000;
            memcpy(c->wait_operation, operation, strlen(operation) + 1);
            memcpy(c->wait_until, until, strlen(until) + 1);
            memcpy(c->wait_request, request.request_id, strlen(request.request_id) + 1);
            json_decref(message);
            gstd_frame_reset(&c->frame);
            c->frame_at = 0;
            return 0;
        }
        error = status != GSTD_QUERY_OK;
        if (error)
            result = query_error(status);
    } else
        result = error_value("UNSUPPORTED_CAPABILITY", "validate",
                             "Capability is not implemented in this build");
    int status = set_response(c, id, result, error);
    json_decref(message);
    gstd_frame_reset(&c->frame);
    c->frame_at = 0;
    return status;
}
static void read_connection(connection *c) {
    unsigned char bytes[8192];
    /* Read only the remaining current frame; never discard a coalesced next frame. */
    size_t need =
        c->frame.prefix_used < 4 ? 4 - c->frame.prefix_used : c->frame.expected - c->frame.used;
    if (need > sizeof(bytes))
        need = sizeof(bytes);
    ssize_t count = recv(c->fd, bytes, need, 0);
    if (count <= 0) {
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            close_connection(c);
        return;
    }
    c->traffic_at = monotonic();
    if (!c->frame_at)
        c->frame_at = c->traffic_at;
    size_t consumed;
    int status = gstd_frame_feed(&c->frame, bytes, (size_t)count, &consumed);
    if (status < 0 || (status == 1 && handle(c)))
        close_connection(c);
}
static int serve(const char *directory, int readiness_fd) {
    int root = gstd_owned_directory(directory);
    if (root < 0) {
        fputs("gitstored: runtime directory must be owned, mode 0700 and free of symlinks\n",
              stderr);
        return 3;
    }
    int lock = openat(root, "daemon.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 || flock(lock, LOCK_EX | LOCK_NB)) {
        fputs("gitstored: unsafe state lock or another manager owns this directory\n", stderr);
        if (lock >= 0)
            close(lock);
        close(root);
        return 5;
    }
    if (fchdir(root)) {
        close(lock);
        close(root);
        return 1;
    }
    if (fstatat(root, "gitstored.sock", &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() ||
            unlinkat(root, "gitstored.sock", 0)) {
            close(lock);
            close(root);
            return 3;
        }
    } else if (errno != ENOENT) {
        close(lock);
        close(root);
        return 3;
    }
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, "gitstored.sock", 15);
    mode_t oldmask = umask(0177);
    int bound = listener >= 0 ? bind(listener, (struct sockaddr *)&address, sizeof(address)) : -1;
    umask(oldmask);
    if (bound || nonblocking(listener) || listen(listener, 32)) {
        perror("gitstored: socket initialization");
        if (listener >= 0)
            close(listener);
        close(lock);
        close(root);
        return 1;
    }
    struct stat socket_identity;
    if (fstatat(root, "gitstored.sock", &socket_identity, AT_SYMLINK_NOFOLLOW)) {
        close(listener);
        close(lock);
        close(root);
        return 1;
    }
    signal(SIGTERM, interrupt_server);
    signal(SIGINT, interrupt_server);
    signal(SIGPIPE, SIG_IGN);
    stopping = 0;
    connection connections[GSTD_CONNECTIONS_MAX];
    memset(connections, 0, sizeof(connections));
    for (size_t i = 0; i < GSTD_CONNECTIONS_MAX; ++i) {
        connections[i].fd = -1;
        connections[i].slot = (unsigned)i;
    }
    if (readiness_fd >= 0) {
        char ready[256];
        int length = snprintf(ready, sizeof(ready),
                              "{\"ready\":true,\"implementationVersion\":\"%s\",\"protocol\":{"
                              "\"major\":1,\"minor\":0}}\n",
                              gstd_version());
        int delivered = length > 0 && (size_t)length < sizeof(ready) &&
                        send(readiness_fd, ready, (size_t)length, 0) == length;
        close(readiness_fd);
        if (!delivered)
            stopping = 1;
    }
    while (!stopping) {
        gstd_runtime_manager_poll(runtime_manager);
        gstd_stream_service_poll(stream_service);
        struct pollfd polls[GSTD_CONNECTIONS_MAX + 1];
        polls[0] = (struct pollfd){listener, POLLIN, 0};
        for (size_t i = 0; i < GSTD_CONNECTIONS_MAX; ++i)
            polls[i + 1] =
                (struct pollfd){connections[i].fd,
                                connections[i].output                                    ? POLLOUT
                                : (connections[i].waiting || connections[i].subscribing) ? 0
                                                                                         : POLLIN,
                                0};
        int poll_ms = stream_service ? 10 : 100;
        for (size_t i = 0; i < GSTD_CONNECTIONS_MAX; i++) {
            connection *c = &connections[i];
            if (c->fd >= 0 && !c->output && !c->waiting && !c->subscribing &&
                !c->frame.prefix_used && gstd_stream_service_ready(stream_service, c->slot))
                poll_ms = 0;
        }
        double before_poll = monotonic();
        for (size_t i = 0; i < GSTD_CONNECTIONS_MAX; i++)
            if (connections[i].fd >= 0 && connections[i].waiting) {
                double remaining = connections[i].wait_deadline - before_poll;
                int ms = remaining <= 0 ? 0 : (int)(remaining * 1000) + 1;
                if (ms < poll_ms)
                    poll_ms = ms;
            }
        int ready = poll(polls, GSTD_CONNECTIONS_MAX + 1, poll_ms);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (polls[0].revents & POLLIN) {
            int peer = accept(listener, NULL, NULL);
            if (peer >= 0) {
                size_t i = 0;
                while (i < GSTD_CONNECTIONS_MAX && connections[i].fd >= 0)
                    ++i;
                if (i == GSTD_CONNECTIONS_MAX || !peer_allowed(peer) || nonblocking(peer))
                    close(peer);
                else {
                    connections[i].fd = peer;
                    connections[i].traffic_at = monotonic();
                }
            }
        }
        double now = monotonic();
        for (size_t i = 0; i < GSTD_CONNECTIONS_MAX; ++i) {
            connection *c = &connections[i];
            if (c->fd < 0)
                continue;
            short events = polls[i + 1].revents;
            if ((events & (POLLERR | POLLNVAL)) ||
                (!c->waiting && !c->subscribing && now - c->traffic_at >= 300) ||
                (c->frame_at && now - c->frame_at >= 10)) {
                close_connection(c);
                continue;
            }
            if (c->subscribing) {
                json_t *value = NULL;
                int error = 1;
                int finished = gstd_stream_service_reply(stream_service, c->slot, &value, &error);
                if (finished < 0) {
                    close_connection(c);
                    continue;
                }
                if (finished) {
                    json_t *id = json_string(c->subscribe_request);
                    c->subscribing = 0;
                    if (!id || set_response(c, id, value, error))
                        close_connection(c);
                    json_decref(id);
                }
            }
            if (c->waiting) {
                json_t *value = NULL;
                gstd_query_status status = wait_poll(c->wait_operation, c->wait_until, &value);
                if (status != GSTD_QUERY_PENDING || now >= c->wait_deadline) {
                    int error = status != GSTD_QUERY_OK;
                    if (status == GSTD_QUERY_PENDING) {
                        value = error_value("DEADLINE_EXCEEDED", "observe",
                                            "Observation timed out; operation remains inspectable");
                        error = 1;
                    } else if (error)
                        value = query_error(status);
                    if (error && value)
                        json_object_set_new(value, "operationId", json_string(c->wait_operation));
                    json_t *id = json_string(c->wait_request);
                    c->waiting = 0;
                    if (!id || set_response(c, id, value, error))
                        close_connection(c);
                    json_decref(id);
                }
            }
            if (c->fd < 0)
                continue;
            int finished_output = 0;
            if ((events & POLLIN) && !c->waiting && !c->subscribing)
                read_connection(c);
            else if (events & POLLOUT) {
                ssize_t sent =
                    send(c->fd, c->output + c->output_sent, c->output_size - c->output_sent, 0);
                if (sent > 0) {
                    c->output_sent += (size_t)sent;
                    c->traffic_at = now;
                    if (c->output_sent == c->output_size) {
                        free(c->output);
                        c->output = NULL;
                        c->output_size = c->output_sent = 0;
                        finished_output = 1;
                    }
                } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    close_connection(c);
            } else if (events & POLLHUP)
                close_connection(c);
            /* After sending a frame, give input a poll turn before the next
             * unsolicited event, so unsubscribe cannot starve behind replay. */
            if (stream_service && c->fd >= 0 && !finished_output && !c->output && !c->waiting &&
                !c->subscribing && !c->frame.prefix_used) {
                json_t *event = NULL;
                int got = gstd_stream_service_next(stream_service, c->slot, &event);
                if (got < 0 || (got && set_frame(c, event)))
                    close_connection(c);
                json_decref(event);
            }
        }
    }
    for (size_t i = 0; i < GSTD_CONNECTIONS_MAX; ++i)
        close_connection(&connections[i]);
    close(listener);
    if (!fstatat(root, "gitstored.sock", &st, AT_SYMLINK_NOFOLLOW) &&
        st.st_ino == socket_identity.st_ino && st.st_dev == socket_identity.st_dev)
        unlinkat(root, "gitstored.sock", 0);
    close(lock);
    close(root);
    return 0;
}

int gstd_serve(const char *directory, const char *state_directory, const char *entrypoint,
               int readiness_fd) {
    if (readiness_fd >= 0) {
        int type = 0;
        socklen_t size = sizeof(type);
        struct sockaddr_un address;
        socklen_t address_size = sizeof(address);
        if (readiness_fd != 3 || getsockopt(readiness_fd, SOL_SOCKET, SO_TYPE, &type, &size) ||
            type != SOCK_STREAM ||
            getsockname(readiness_fd, (struct sockaddr *)&address, &address_size) ||
            address.sun_family != AF_UNIX || !peer_allowed(readiness_fd) ||
            nonblocking(readiness_fd))
            return 2;
    }
    if (!state_directory)
        return serve(directory, readiness_fd);
    int root = gstd_owned_directory(state_directory);
    if (root < 0)
        return 3;
    int lock = openat(root, "state.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0)
            close(lock);
        close(root);
        return 5;
    }
    if (gstd_runtime_settings_load(root, &runtime_settings)) {
        fputs("gitstored: invalid private runtime owner settings\n", stderr);
        close(lock);
        close(root);
        return 2;
    }
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/state.db", state_directory);
    if (n < 0 || (size_t)n >= sizeof(path) || gstd_state_open(path, &database)) {
        gstd_runtime_settings_clear(&runtime_settings);
        close(lock);
        close(root);
        return 1;
    }
    runtime_manager = gstd_runtime_manager_open(database, root, &runtime_settings, entrypoint);
    if (!runtime_manager) {
        sqlite3_close(database);
        database = NULL;
        gstd_runtime_settings_clear(&runtime_settings);
        close(lock);
        close(root);
        return 1;
    }
    stream_service = gstd_stream_service_open(database, root);
    int result = serve(directory, readiness_fd);
    gstd_stream_service_close(stream_service);
    stream_service = NULL;
    gstd_runtime_manager_close(runtime_manager);
    runtime_manager = NULL;
    sqlite3_close(database);
    database = NULL;
    gstd_runtime_settings_clear(&runtime_settings);
    close(lock);
    close(root);
    return result;
}
