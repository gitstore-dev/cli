#include "gitstored/protocol.h"
#include "redaction.h"
#include "streams.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "streams line %d: %s\n", __LINE__, #x);                                \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int equal(const json_t *v, const char *key, const char *expected) {
    const char *value = json_string_value(json_object_get(v, key));
    return value && !strcmp(value, expected);
}
typedef struct {
    char bytes[65536];
    size_t used;
    int fail;
} captured;
static int capture(void *context, const char *bytes, size_t length) {
    captured *output = context;
    if (output->fail || length + 1 > sizeof(output->bytes) - output->used)
        return -1;
    memcpy(output->bytes + output->used, bytes, length);
    output->used += length;
    output->bytes[output->used++] = '\n';
    return 0;
}
static int redaction(void) {
    const char *secrets[] = {"fixture-key-123456789", "longer-fixture-key-123456789-tail"};
    const char *input =
        "before longer-fixture-key-123456789-tail after\nAuthorization: Bearer "
        "unknown-value\n-----BEGIN PRIVATE KEY-----\nprivate-body-without-field-label\n-----END "
        "PRIVATE KEY-----\nafter PEM\n";
    for (size_t split = 0; split <= strlen(input); split++) {
        captured output = {0};
        gstd_redactor *r = gstd_redactor_open(secrets, 2, capture, &output);
        if (!r || gstd_redactor_feed(r, input, split) ||
            gstd_redactor_feed(r, input + split, strlen(input) - split) ||
            gstd_redactor_finish(r)) {
            gstd_redactor_close(r);
            return -1;
        }
        gstd_redactor_close(r);
        if (strstr(output.bytes, "fixture-key") || strstr(output.bytes, "unknown-value") ||
            strstr(output.bytes, "private-body") ||
            !strstr(output.bytes, "before [REDACTED] after") || !strstr(output.bytes, "after PEM"))
            return -1;
    }
    captured output = {0};
    gstd_redactor *r = gstd_redactor_open(secrets, 2, capture, &output);
    if (!r)
        return -1;
    for (unsigned i = 0; i < GSTD_LOG_LINE_MAX + 1; i++)
        if (gstd_redactor_feed(r, "x", 1)) {
            gstd_redactor_close(r);
            return -1;
        }
    const char binary[] = {'a', 0, 'b', '\n', (char)0xff, '\n'};
    int failed = gstd_redactor_feed(r, "\n", 1) || gstd_redactor_feed(r, binary, sizeof(binary)) ||
                 gstd_redactor_finish(r) || !strstr(output.bytes, "[OMITTED:") ||
                 strchr(output.bytes, 'x');
    output.fail = 1;
    failed |= gstd_redactor_feed(r, "test\n", 5) != -1 || gstd_redactor_feed(r, "more", 4) != -1;
    gstd_redactor_close(r);
    return failed ? -1 : 0;
}
int main(void) {
    gstd_json_initialize();
    int result = 1;
    gstd_streams *streams = gstd_streams_open();
    json_t *payload = json_pack("{s:s,s:s}", "cursor", "opaque-1", "text", "redacted fixture"),
           *event = NULL;
    char slow[65], fast[65], ids[8][65], foreign[65], replacement[65];
    CHECK(streams && payload && !redaction());
    CHECK(!gstd_streams_subscribe(streams, 0, "one", GSTD_STREAM_LOGS, "api", slow));
    CHECK(!gstd_streams_subscribe(streams, 1, "one", GSTD_STREAM_LOGS, "api", fast));
    CHECK(gstd_streams_unsubscribe(streams, 1, slow) == -1);
    CHECK(!gstd_streams_replay(streams, 1, fast, "one", GSTD_STREAM_LOGS, "api", payload));
    CHECK(gstd_streams_pending(streams, 0, slow) == 0);
    CHECK(gstd_streams_pending(streams, 1, fast) == 1);
    CHECK(gstd_streams_pending(streams, 1, slow) == -1);
    CHECK(gstd_streams_replay(streams, 0, fast, "one", GSTD_STREAM_LOGS, "api", payload) == -1);
    CHECK(gstd_streams_next(streams, 1, &event) == 1 && equal(event, "subscriptionId", fast));
    json_decref(event);
    event = NULL;
    for (unsigned i = 0; i < 257; i++) {
        CHECK(!gstd_streams_publish(streams, "one", GSTD_STREAM_LOGS, "api", payload));
        CHECK(gstd_streams_next(streams, 1, &event) == 1);
        CHECK(equal(event, "subscriptionId", fast) && equal(event, "type", "log"));
        CHECK(!json_object_get(event, "requestId") &&
              json_integer_value(json_object_get(event, "sequence")) == i + 2);
        json_decref(event);
        event = NULL;
    }
    CHECK(gstd_streams_next(streams, 0, &event) == 1);
    CHECK(equal(event, "subscriptionId", slow) && equal(event, "type", "gap"));
    CHECK(equal(json_object_get(event, "payload"), "code", "STREAM_OVERFLOW"));
    json_decref(event);
    event = NULL;
    CHECK(!gstd_streams_next(streams, 0, &event));
    CHECK(gstd_streams_unsubscribe(streams, 0, slow) == 0);
    CHECK(!gstd_streams_terminal(streams, 1, fast, "CURSOR_EXPIRED"));
    CHECK(gstd_streams_next(streams, 1, &event) == 1 && equal(event, "type", "gap"));
    CHECK(equal(json_object_get(event, "payload"), "code", "CURSOR_EXPIRED"));
    json_decref(event);
    event = NULL;
    CHECK(gstd_streams_unsubscribe(streams, 1, fast) == 0);
    CHECK(gstd_streams_unsubscribe(streams, 1, fast) == 0);
    CHECK(!gstd_streams_queued_bytes(streams));
    for (unsigned i = 0; i < 8; i++)
        CHECK(!gstd_streams_subscribe(streams, 2, "other", GSTD_STREAM_EVENTS_KIND, NULL, ids[i]));
    CHECK(gstd_streams_subscribe(streams, 2, "other", GSTD_STREAM_LOGS, NULL, foreign) == -2);
    CHECK(!gstd_streams_publish(streams, "one", GSTD_STREAM_LOGS, "git", payload));
    CHECK(!gstd_streams_next(streams, 2, &event));
    CHECK(!gstd_streams_publish(streams, "other", GSTD_STREAM_EVENTS_KIND, "git", payload));
    CHECK(gstd_streams_next(streams, 2, &event) == 1 && equal(event, "type", "event"));
    json_decref(event);
    event = NULL;
    gstd_streams_disconnect(streams, 2);
    CHECK(!gstd_streams_queued_bytes(streams));
    CHECK(!gstd_streams_subscribe(streams, 2, "other", GSTD_STREAM_EVENTS_KIND, NULL, replacement));
    CHECK(strcmp(replacement, ids[0]) && gstd_streams_unsubscribe(streams, 2, ids[0]) == -1);
    gstd_streams_disconnect(streams, 2);
    CHECK(gstd_streams_subscribe(streams, 32, "one", GSTD_STREAM_LOGS, NULL, foreign) == -1);
    /* Byte pressure must shed a queue before its 256-event count is exhausted. */
    char *text = malloc(GSTD_STRING_MAX + 1);
    CHECK(text);
    memset(text, 'x', GSTD_STRING_MAX);
    text[GSTD_STRING_MAX] = 0;
    json_t *large = json_pack("{s:s}", "text", text);
    free(text);
    CHECK(large);
    json_decref(payload);
    payload = large;
    CHECK(!gstd_streams_subscribe(streams, 3, "large", GSTD_STREAM_LOGS, NULL, slow));
    for (unsigned i = 0; i < 17; i++) {
        CHECK(!gstd_streams_publish(streams, "large", GSTD_STREAM_LOGS, "api", payload));
        CHECK(gstd_streams_queued_bytes(streams) <= GSTD_STREAM_BYTES);
    }
    CHECK(gstd_streams_next(streams, 3, &event) == 1 && equal(event, "type", "gap"));
    json_decref(event);
    event = NULL;
    /* Full fan-out still obeys the single global accounting bound. */
    for (unsigned connection = 0; connection < 32; connection++) {
        gstd_streams_disconnect(streams, connection);
        for (unsigned i = 0; i < 8; i++)
            CHECK(!gstd_streams_subscribe(streams, connection, "global", GSTD_STREAM_LOGS, NULL,
                                          foreign));
    }
    CHECK(!gstd_streams_publish(streams, "global", GSTD_STREAM_LOGS, "api", payload));
    CHECK(gstd_streams_queued_bytes(streams) <= GSTD_STREAM_GLOBAL_BYTES);
    unsigned gaps = 0;
    for (unsigned connection = 0; connection < 32; connection++) {
        while (gstd_streams_next(streams, connection, &event) == 1) {
            gaps += equal(event, "type", "gap");
            json_decref(event);
            event = NULL;
        }
    }
    CHECK(gaps && !gstd_streams_queued_bytes(streams));
    gstd_streams_close(streams);
    streams = gstd_streams_open();
    CHECK(streams);
    CHECK(!gstd_streams_subscribe(streams, 0, "providers", GSTD_STREAM_LOGS, "oidc", slow));
    CHECK(!gstd_streams_subscribe(streams, 1, "providers", GSTD_STREAM_LOGS, "oidc-hydra", fast));
    CHECK(!gstd_streams_publish(streams, "providers", GSTD_STREAM_LOGS, "oidc-kratos", payload));
    CHECK(gstd_streams_next(streams, 0, &event) == 1);
    json_decref(event); event = NULL;
    CHECK(!gstd_streams_next(streams, 1, &event));
    CHECK(!gstd_streams_publish(streams, "providers", GSTD_STREAM_LOGS, "oidc-hydra", payload));
    CHECK(gstd_streams_next(streams, 1, &event) == 1);
    json_decref(event); event = NULL;
    CHECK(!gstd_streams_publish(streams, "providers", GSTD_STREAM_LOGS, "api", payload));
    CHECK(gstd_streams_next(streams, 0, &event) == 1);
    json_decref(event); event = NULL;
    CHECK(!gstd_streams_next(streams, 0, &event));
    result = 0;
done:
    json_decref(event);
    json_decref(payload);
    gstd_streams_close(streams);
    return result;
}
