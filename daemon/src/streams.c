#define _POSIX_C_SOURCE 200809L
#include "streams.h"
#include "gitstored/protocol.h"
#include "oidc_profile.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct entry {
    struct entry *next;
    size_t charge, length;
    char bytes[];
} entry;
typedef struct {
    char id[65], deployment[65], component[16];
    int active, overflow;
    char gap_code[32];
    gstd_stream_kind kind;
    uint64_t sequence, dropped;
    unsigned count;
    size_t bytes;
    entry *head, *tail;
} subscription;
struct gstd_streams {
    char nonce[33];
    uint32_t epoch, connection_epoch[32], serial[32];
    unsigned next[32];
    size_t bytes;
    subscription subscriptions[32][8];
};
static int component_valid(const char *name) {
    if (gstd_oidc_component(name))
        return 1;
    static const char *names[] = {"api",   "git",        "controller", "oidc",
                                  "admin", "storefront", "scylladb"};
    if (!name)
        return 1;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}
static int identifier(const char *value) {
    return value && gstd_identifier(value, strnlen(value, 65));
}
static void clear_queue(gstd_streams *streams, subscription *s) {
    while (s->head) {
        entry *next = s->head->next;
        streams->bytes -= s->head->charge;
        free(s->head);
        s->head = next;
    }
    s->tail = NULL;
    s->bytes = s->count = 0;
}
static void close_subscription(gstd_streams *streams, subscription *s) {
    clear_queue(streams, s);
    memset(s, 0, sizeof(*s));
}
gstd_streams *gstd_streams_open(void) {
    unsigned char nonce[16];
    size_t have = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    while (have < sizeof(nonce)) {
        ssize_t n = read(fd, nonce + have, sizeof(nonce) - have);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        have += (size_t)n;
    }
    close(fd);
    if (have != sizeof(nonce))
        return NULL;
    gstd_streams *streams = calloc(1, sizeof(*streams));
    if (streams)
        for (unsigned i = 0; i < sizeof(nonce); i++)
            snprintf(streams->nonce + 2 * i, 3, "%02x", nonce[i]);
    return streams;
}
void gstd_streams_disconnect(gstd_streams *streams, unsigned connection) {
    if (!streams || connection >= 32)
        return;
    for (unsigned i = 0; i < 8; i++)
        close_subscription(streams, &streams->subscriptions[connection][i]);
    streams->connection_epoch[connection] = streams->serial[connection] =
        streams->next[connection] = 0;
}
void gstd_streams_close(gstd_streams *streams) {
    if (!streams)
        return;
    for (unsigned i = 0; i < 32; i++)
        gstd_streams_disconnect(streams, i);
    free(streams);
}
int gstd_streams_subscribe(gstd_streams *streams, unsigned connection, const char *deployment,
                           gstd_stream_kind kind, const char *component, char id[65]) {
    if (!streams || connection >= 32 || !identifier(deployment) || !component_valid(component) ||
        (kind != GSTD_STREAM_LOGS && kind != GSTD_STREAM_EVENTS_KIND) || !id)
        return -1;
    unsigned slot = 0;
    while (slot < 8 && streams->subscriptions[connection][slot].active)
        slot++;
    if (slot == 8 || streams->serial[connection] == UINT32_MAX ||
        (!streams->connection_epoch[connection] && streams->epoch == UINT32_MAX))
        return -2;
    if (!streams->connection_epoch[connection])
        streams->connection_epoch[connection] = ++streams->epoch;
    subscription *s = &streams->subscriptions[connection][slot];
    char nonce[33];
    memcpy(nonce, streams->nonce, sizeof(nonce));
    snprintf(s->id, sizeof(s->id), "sub-%s-%x-%x", nonce, streams->connection_epoch[connection],
             ++streams->serial[connection]);
    memcpy(id, s->id, strlen(s->id) + 1);
    memcpy(s->deployment, deployment, strlen(deployment) + 1);
    if (component)
        memcpy(s->component, component, strlen(component) + 1);
    s->kind = kind;
    s->active = 1;
    return 0;
}
int gstd_streams_unsubscribe(gstd_streams *streams, unsigned connection, const char *id) {
    if (!streams || connection >= 32 || !identifier(id) || !streams->connection_epoch[connection])
        return -1;
    for (unsigned i = 0; i < 8; i++) {
        subscription *s = &streams->subscriptions[connection][i];
        if (s->active && !strcmp(s->id, id)) {
            close_subscription(streams, s);
            return 1;
        }
    }
    char prefix[64];
    int length = snprintf(prefix, sizeof(prefix), "sub-%s-%x-", streams->nonce,
                          streams->connection_epoch[connection]);
    if (length < 1 || (size_t)length >= sizeof(prefix) || strncmp(prefix, id, (size_t)length))
        return -1;
    const char *suffix = id + length;
    size_t size = strlen(suffix);
    if (!size || size > 8 || suffix[0] == '0')
        return -1;
    for (size_t i = 0; i < size; i++)
        if (!((suffix[i] >= '0' && suffix[i] <= '9') || (suffix[i] >= 'a' && suffix[i] <= 'f')))
            return -1;
    unsigned long serial = strtoul(suffix, NULL, 16);
    return serial && serial <= streams->serial[connection] ? 0 : -1;
}
static void overflow(gstd_streams *streams, subscription *s) {
    s->dropped = (uint64_t)s->count + 1;
    clear_queue(streams, s);
    s->overflow = 1;
    snprintf(s->gap_code, sizeof(s->gap_code), "%s", "STREAM_OVERFLOW");
}
static int deliver(gstd_streams *streams, const char *deployment, gstd_stream_kind kind,
                   const char *component, const json_t *payload, int target, const char *id) {
    if (!streams || !identifier(deployment) || !component_valid(component) ||
        (kind != GSTD_STREAM_LOGS && kind != GSTD_STREAM_EVENTS_KIND) || !json_is_object(payload))
        return -1;
    char *bytes = gstd_json_dump(payload);
    if (!bytes)
        return -1;
    size_t length = strlen(bytes), charge = sizeof(entry) + length + 1 + 512;
    json_t *checked = gstd_json_parse((const unsigned char *)bytes, length);
    int bounded = json_is_object(checked);
    json_decref(checked);
    if (!bounded) {
        free(bytes);
        return -1;
    }
    if (length > GSTD_FRAME_MAX - 512) {
        free(bytes);
        return -1;
    }
    int found = 0;
    for (unsigned connection = 0; connection < 32; connection++)
        for (unsigned i = 0; i < 8; i++) {
            subscription *s = &streams->subscriptions[connection][i];
            if (target >= 0 && ((unsigned)target != connection || strcmp(s->id, id)))
                continue;
            if (!s->active || s->kind != kind || strcmp(s->deployment, deployment) ||
                (*s->component && (!component || (strcmp(s->component, component) &&
                 (strcmp(s->component, "oidc") || !gstd_oidc_component(component))))))
                continue;
            found = 1;
            if (s->overflow) {
                if (s->dropped < GSTD_SAFE_INTEGER)
                    s->dropped++;
                continue;
            }
            if (s->count >= GSTD_STREAM_EVENTS || charge > GSTD_STREAM_BYTES - s->bytes ||
                charge > GSTD_STREAM_GLOBAL_BYTES - streams->bytes) {
                overflow(streams, s);
                continue;
            }
            entry *node = malloc(sizeof(*node) + length + 1);
            if (!node) {
                overflow(streams, s);
                continue;
            }
            *node = (entry){.charge = charge, .length = length};
            memcpy(node->bytes, bytes, length + 1);
            if (s->tail)
                s->tail->next = node;
            else
                s->head = node;
            s->tail = node;
            s->count++;
            s->bytes += charge;
            streams->bytes += charge;
        }
    free(bytes);
    return target >= 0 && !found ? -1 : 0;
}
int gstd_streams_publish(gstd_streams *streams, const char *deployment, gstd_stream_kind kind,
                         const char *component, const json_t *payload) {
    return deliver(streams, deployment, kind, component, payload, -1, NULL);
}
int gstd_streams_replay(gstd_streams *streams, unsigned connection, const char *id,
                        const char *deployment, gstd_stream_kind kind, const char *component,
                        const json_t *payload) {
    if (connection >= 32 || !identifier(id))
        return -1;
    return deliver(streams, deployment, kind, component, payload, (int)connection, id);
}
int gstd_streams_pending(const gstd_streams *streams, unsigned connection, const char *id) {
    if (!streams || connection >= 32 || !identifier(id))
        return -1;
    for (unsigned i = 0; i < 8; i++) {
        const subscription *s = &streams->subscriptions[connection][i];
        if (s->active && !strcmp(s->id, id))
            return s->overflow ? -2 : (int)s->count;
    }
    return -1;
}
int gstd_streams_terminal(gstd_streams *streams, unsigned connection, const char *id,
                          const char *code) {
    if (!streams || connection >= 32 || !identifier(id) || !code ||
        (strcmp(code, "CURSOR_EXPIRED") && strcmp(code, "INTERNAL")))
        return -1;
    for (unsigned i = 0; i < 8; i++) {
        subscription *s = &streams->subscriptions[connection][i];
        if (s->active && !strcmp(s->id, id)) {
            if (s->overflow)
                return 0;
            overflow(streams, s);
            snprintf(s->gap_code, sizeof(s->gap_code), "%s", code);
            return 0;
        }
    }
    return -1;
}
int gstd_streams_next(gstd_streams *streams, unsigned connection, json_t **event) {
    if (!streams || connection >= 32 || !event)
        return -1;
    *event = NULL;
    for (unsigned n = 0; n < 8; n++) {
        unsigned slot = (streams->next[connection] + n) % 8;
        subscription *s = &streams->subscriptions[connection][slot];
        if (!s->active || (!s->head && !s->overflow))
            continue;
        if (s->sequence >= GSTD_SAFE_INTEGER) {
            close_subscription(streams, s);
            return -1;
        }
        json_t *payload =
            s->overflow
                ? json_pack("{s:s,s:I}", "code", s->gap_code, "dropped", (json_int_t)s->dropped)
                : gstd_json_parse((const unsigned char *)s->head->bytes, s->head->length);
        json_t *value =
            payload ? json_pack("{s:{s:i,s:i},s:s,s:s,s:I,s:s,s:O}", "version", "major", 1, "minor",
                                0, "subscriptionId", s->id, "deploymentId", s->deployment,
                                "sequence", (json_int_t)(s->sequence + 1), "type",
                                s->overflow                   ? "gap"
                                : s->kind == GSTD_STREAM_LOGS ? "log"
                                                              : "event",
                                "payload", payload)
                    : NULL;
        json_decref(payload);
        if (!value)
            return -1;
        s->sequence++;
        if (s->overflow)
            close_subscription(streams, s);
        else {
            entry *node = s->head;
            s->head = node->next;
            if (!s->head)
                s->tail = NULL;
            s->count--;
            s->bytes -= node->charge;
            streams->bytes -= node->charge;
            free(node);
        }
        streams->next[connection] = (slot + 1) % 8;
        *event = value;
        return 1;
    }
    return 0;
}
size_t gstd_streams_queued_bytes(const gstd_streams *streams) {
    return streams ? streams->bytes : 0;
}
