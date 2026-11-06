#define _POSIX_C_SOURCE 200809L
#include "stream_service.h"
#include "log_read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef struct {
    unsigned connection;
    int active, acknowledged, reading;
    int64_t after;
    char id[65], deployment[65], component[16], cursor[65];
    gstd_stream_kind kind;
} observer;
struct gstd_stream_service {
    sqlite3 *db;
    int state;
    char store[33];
    gstd_streams *streams;
    observer observers[256];
    unsigned next;
    struct {
        gstd_log_read job;
        char id[65];
    } readers[2];
    json_t *replies[32];
    int errors[32];
};
static int64_t now_ms(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts) ? 0
                                               : (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static json_t *error_value(const char *code) {
    return json_pack(
        "{s:s,s:s,s:b,s:s}", "code", code, "stage", "stream", "retryable", 0, "message",
        !strcmp(code, "CURSOR_EXPIRED")   ? "Requested log history is no longer retained"
        : !strcmp(code, "RESOURCE_LIMIT") ? "Log observation capacity is occupied; retry later"
        : !strcmp(code, "NOT_FOUND")      ? "Deployment, component or subscription not found"
                                          : "Local log observation could not complete");
}
static observer *find(gstd_stream_service *s, const char *id) {
    for (unsigned i = 0; i < 256; i++)
        if (s->observers[i].active && !strcmp(s->observers[i].id, id))
            return &s->observers[i];
    return NULL;
}
static int launch(gstd_stream_service *s, observer *o) {
    for (unsigned i = 0; i < 2; i++)
        if (!s->readers[i].id[0]) {
            gstd_log_read_input input = {.state_fd = s->state,
                                         .store_id = s->store,
                                         .subscription_id = o->id,
                                         .deployment_id = o->deployment,
                                         .component = o->component[0] ? o->component : NULL,
                                         .cursor = o->cursor[0] ? o->cursor : NULL,
                                         .kind = o->kind,
                                         .limit = 32,
                                         .now = (int64_t)time(NULL)};
            int started = gstd_log_read_launch(s->db, &input, &s->readers[i].job);
            if (started == 1) {
                snprintf(s->readers[i].id, sizeof(s->readers[i].id), "%s", o->id);
                o->reading = 1;
            }
            return started;
        }
    return 0;
}
gstd_stream_service *gstd_stream_service_open(sqlite3 *db, int state) {
#ifndef __linux__
    (void)db;
    (void)state;
    return NULL;
#else
    if (!db || state < 3)
        return NULL;
    gstd_stream_service *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    sqlite3_stmt *q = NULL;
    int ok = sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &q, NULL) == SQLITE_OK &&
             sqlite3_step(q) == SQLITE_ROW && sqlite3_column_bytes(q, 0) == 32;
    if (ok) {
        const char *store = (const char *)sqlite3_column_text(q, 0);
        memcpy(s->store, store, 32);
        for (unsigned i = 0; i < 32; i++)
            if (!((store[i] >= '0' && store[i] <= '9') || (store[i] >= 'a' && store[i] <= 'f')))
                ok = 0;
    }
    sqlite3_finalize(q);
    s->db = db;
    s->state = state;
    for (unsigned i = 0; i < 2; i++)
        s->readers[i].job = (gstd_log_read)GSTD_LOG_READ_INITIAL;
    if (!ok || !(s->streams = gstd_streams_open())) {
        free(s);
        return NULL;
    }
    return s;
#endif
}
int gstd_stream_service_subscribe(gstd_stream_service *s, unsigned connection, const json_t *params,
                                  json_t **result, int *error) {
    if (!s || connection >= 32 || !params || !result || !error)
        return -1;
    *result = NULL;
    *error = 1;
    const char *deployment = json_string_value(json_object_get(params, "deploymentId"));
    const char *component = json_string_value(json_object_get(params, "component"));
    const char *cursor = json_string_value(json_object_get(params, "cursor"));
    const char *kind = json_string_value(json_object_get(params, "kind"));
    if (!deployment || !kind || !gstd_identifier(deployment, strnlen(deployment, 65)) ||
        (component && strnlen(component, 16) >= 16) || (cursor && strnlen(cursor, 65) > 64) ||
        (strcmp(kind, "logs") && strcmp(kind, "events"))) {
        *result = error_value("INVALID_ARGUMENT");
        return *result ? 0 : -1;
    }
    sqlite3_stmt *q = NULL;
    int ok =
        sqlite3_prepare_v2(
            s->db,
            "SELECT 1 FROM deployments d WHERE deployment_id=? AND (? IS NULL OR EXISTS(SELECT 1 "
            "FROM components c WHERE c.deployment_id=d.deployment_id AND (c.component=?3 OR "
            "(?3='oidc' AND c.component IN ('oidc-smtp','oidc-hydra','oidc-kratos',"
            "'oidc-ui','oidc-bridge')))))",
            -1, &q, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(q, 1, deployment, -1, SQLITE_TRANSIENT);
        if (component) {
            sqlite3_bind_text(q, 2, component, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 3, component, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(q, 2);
            sqlite3_bind_null(q, 3);
        }
    }
    int row = ok ? sqlite3_step(q) : SQLITE_ERROR;
    sqlite3_finalize(q);
    if (row != SQLITE_ROW) {
        *result = error_value(row == SQLITE_DONE ? "NOT_FOUND" : "INTERNAL");
        return *result ? 0 : -1;
    }
    observer *o = NULL;
    for (unsigned i = 0; i < 256; i++)
        if (!s->observers[i].active) {
            o = &s->observers[i];
            break;
        }
    int unresolved = s->replies[connection] != NULL;
    for (unsigned i = 0; i < 256; i++)
        if (s->observers[i].active && s->observers[i].connection == connection &&
            !s->observers[i].acknowledged)
            unresolved = 1;
    if (!o || unresolved) {
        *result = error_value("RESOURCE_LIMIT");
        return *result ? 0 : -1;
    }
    memset(o, 0, sizeof(*o));
    o->kind = !strcmp(kind, "logs") ? GSTD_STREAM_LOGS : GSTD_STREAM_EVENTS_KIND;
    int subscribed =
        gstd_streams_subscribe(s->streams, connection, deployment, o->kind, component, o->id);
    if (subscribed) {
        *result = error_value(subscribed == -2 ? "RESOURCE_LIMIT" : "INVALID_ARGUMENT");
        return *result ? 0 : -1;
    }
    o->active = 1;
    o->connection = connection;
    snprintf(o->deployment, sizeof(o->deployment), "%s", deployment);
    if (component)
        snprintf(o->component, sizeof(o->component), "%s", component);
    if (cursor)
        snprintf(o->cursor, sizeof(o->cursor), "%s", cursor);
    int started = launch(s, o);
    if (started == 1)
        return 1;
    (void)gstd_streams_unsubscribe(s->streams, connection, o->id);
    o->active = 0;
    *result = error_value(started == 0 ? "RESOURCE_LIMIT" : "INTERNAL");
    return *result ? 0 : -1;
}
json_t *gstd_stream_service_unsubscribe(gstd_stream_service *s, unsigned connection, const char *id,
                                        int *error) {
    if (!s || connection >= 32 || !error)
        return NULL;
    int removed = gstd_streams_unsubscribe(s->streams, connection, id);
    *error = removed < 0;
    if (removed < 0)
        return error_value("NOT_FOUND");
    observer *o = find(s, id);
    if (o && o->connection == connection) {
        o->active = 0;
        for (unsigned i = 0; i < 2; i++)
            if (!strcmp(s->readers[i].id, id))
                gstd_log_read_abandon(&s->readers[i].job);
    }
    return json_pack("{s:b}", "closed", removed == 1);
}
void gstd_stream_service_disconnect(gstd_stream_service *s, unsigned connection) {
    if (!s || connection >= 32)
        return;
    for (unsigned i = 0; i < 256; i++)
        if (s->observers[i].active && s->observers[i].connection == connection) {
            observer *o = &s->observers[i];
            for (unsigned j = 0; j < 2; j++)
                if (!strcmp(s->readers[j].id, o->id))
                    gstd_log_read_abandon(&s->readers[j].job);
            o->active = 0;
        }
    gstd_streams_disconnect(s->streams, connection);
    json_decref(s->replies[connection]);
    s->replies[connection] = NULL;
    s->errors[connection] = 0;
}
void gstd_stream_service_poll(gstd_stream_service *s) {
    if (!s)
        return;
    int64_t now = now_ms();
    for (unsigned i = 0; i < 2; i++)
        if (s->readers[i].id[0]) {
            gstd_log_read *job = &s->readers[i].job;
            if (gstd_log_read_poll(s->db, job) != 1)
                continue;
            observer *o = find(s, s->readers[i].id);
            if (o) {
                o->reading = 0;
                o->after = now + 500;
                if (!o->acknowledged) {
                    s->errors[o->connection] = job->status != GSTD_LOG_OK;
                    s->replies[o->connection] =
                        job->status == GSTD_LOG_OK
                            ? json_pack("{s:s,s:b}", "subscriptionId", o->id, "replayAvailable", 1)
                            : error_value(job->status == GSTD_LOG_CURSOR_EXPIRED ? "CURSOR_EXPIRED"
                                                                                 : "INTERNAL");
                    if (!s->replies[o->connection])
                        s->errors[o->connection] = -1;
                    o->acknowledged = 1;
                    if (job->status != GSTD_LOG_OK) {
                        (void)gstd_streams_unsubscribe(s->streams, o->connection, o->id);
                        o->active = 0;
                    }
                } else if (job->status != GSTD_LOG_OK) {
                    (void)gstd_streams_terminal(
                        s->streams, o->connection, o->id,
                        job->status == GSTD_LOG_CURSOR_EXPIRED ? "CURSOR_EXPIRED" : "INTERNAL");
                }
                if (o->active && job->status == GSTD_LOG_OK) {
                    size_t index;
                    json_t *record;
                    json_array_foreach(job->records, index, record) {
                        const char *component =
                            json_string_value(json_object_get(record, "component"));
                        const char *cursor = json_string_value(json_object_get(record, "cursor"));
                        if (!cursor || strnlen(cursor, 65) > 64 ||
                            gstd_streams_replay(s->streams, o->connection, o->id, o->deployment,
                                                o->kind, component, record)) {
                            (void)gstd_streams_terminal(s->streams, o->connection, o->id,
                                                        "INTERNAL");
                            break;
                        }
                        snprintf(o->cursor, sizeof(o->cursor), "%s", cursor);
                    }
                    if (json_array_size(job->records) == 32)
                        o->after = now;
                }
            }
            gstd_log_read_close(job);
            s->readers[i].id[0] = 0;
        }
    for (unsigned n = 0; n < 256; n++) {
        unsigned index = s->next;
        s->next = (s->next + 1) % 256;
        observer *o = &s->observers[index];
        if (!o->active || o->reading || !o->acknowledged || now < o->after)
            continue;
        int pending = gstd_streams_pending(s->streams, o->connection, o->id);
        if (pending == -2)
            continue; /* terminal gap already queued */
        if (pending < 0) {
            o->active = 0;
            continue;
        }
        int launched = launch(s, o);
        if (!launched)
            break;
        if (launched < 0) {
            (void)gstd_streams_terminal(s->streams, o->connection, o->id, "INTERNAL");
            o->after = now + 500;
        }
    }
}
int gstd_stream_service_reply(gstd_stream_service *s, unsigned connection, json_t **result,
                              int *error) {
    if (!s || connection >= 32 || !result || !error)
        return -1;
    *result = s->replies[connection];
    *error = s->errors[connection];
    s->replies[connection] = NULL;
    return *error < 0 ? -1 : *result ? 1 : 0;
}
int gstd_stream_service_ready(const gstd_stream_service *s, unsigned connection) {
    if (!s || connection >= 32)
        return 0;
    for (unsigned i = 0; i < 256; i++) {
        const observer *o = &s->observers[i];
        if (o->active && o->connection == connection && o->acknowledged) {
            int pending = gstd_streams_pending(s->streams, connection, o->id);
            if (pending > 0 || pending == -2)
                return 1;
        }
    }
    return 0;
}
int gstd_stream_service_next(gstd_stream_service *s, unsigned connection, json_t **event) {
    return s ? gstd_streams_next(s->streams, connection, event) : -1;
}
void gstd_stream_service_close(gstd_stream_service *s) {
    if (!s)
        return;
    for (unsigned i = 0; i < 32; i++)
        gstd_stream_service_disconnect(s, i);
    int64_t deadline = now_ms() + 5000;
    for (unsigned i = 0; i < 2; i++)
        if (s->readers[i].id[0]) {
            gstd_log_read *job = &s->readers[i].job;
            while (!job->reaped && now_ms() < deadline) {
                (void)gstd_log_read_poll(s->db, job);
                struct timespec pause = {0, 1000000};
                nanosleep(&pause, NULL);
            }
            /* Shutdown never releases an unreaped child's lease. Parent death
             * kills its private reader; next manager owns stale-lease recovery. */
            if (job->reaped)
                (void)gstd_log_read_poll(s->db, job);
            if (job->channel >= 0)
                close(job->channel);
            free(job->bytes);
            json_decref(job->records);
        }
    gstd_streams_close(s->streams);
    free(s);
}
