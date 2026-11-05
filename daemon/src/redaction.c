#define _POSIX_C_SOURCE 200809L
#include "redaction.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
struct gstd_redactor {
    char *secrets[64];
    size_t count, used;
    int omitted, pem, failed;
    gstd_redacted_sink sink;
    void *context;
    char line[GSTD_LOG_LINE_MAX + 1];
};
static void wipe(void *memory, size_t length) {
    volatile unsigned char *bytes = memory;
    while (length--)
        *bytes++ = 0;
}
static int contains(const char *line, const char *needle) {
    for (; *line; line++) {
        size_t i = 0;
        for (; needle[i] && line[i]; i++) {
            unsigned char c = (unsigned char)line[i];
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if (c != (unsigned char)needle[i])
                break;
        }
        if (!needle[i])
            return 1;
    }
    return 0;
}
static int emit(gstd_redactor *r, const char *text, size_t size) {
    if (r->sink(r->context, text, size)) {
        r->failed = 1;
        return -1;
    }
    return 0;
}
static int line(gstd_redactor *r) {
    static const char *fields[] = {"authorization", "password",   "passwd",      "secret",
                                   "token",         "privatekey", "private_key", "private key",
                                   "api-key",       "api_key",    "apikey",      "signingkey",
                                   "signing_key",   "cookie"};
    r->line[r->used] = 0;
    const char *fixed = NULL;
    int suppress = 0;
    if (r->pem) {
        if (contains(r->line, "-----end "))
            r->pem = 0;
        suppress = 1;
    } else if (contains(r->line, "-----begin ")) {
        r->pem = !contains(r->line, "-----end ");
        fixed = "[REDACTED: PEM block]";
    } else if (r->omitted)
        fixed = "[OMITTED: oversized or binary log line]";
    else {
        for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
            if (contains(r->line, fields[i])) {
                fixed = "[REDACTED: sensitive log line]";
                break;
            }
        if (!fixed && strstr(r->line, "eyJ") && strchr(r->line, '.'))
            fixed = "[REDACTED: possible JWT]";
    }
    char output[GSTD_LOG_LINE_MAX + 1];
    size_t used = 0;
    int result = 0;
    if (!suppress && !fixed) {
        unsigned char mask[GSTD_LOG_LINE_MAX] = {0};
        json_t *valid = json_stringn(r->line, r->used);
        if (!valid)
            fixed = "[OMITTED: invalid UTF-8 log line]";
        json_decref(valid);
        for (size_t i = 0; !fixed && i < r->count; i++) {
            size_t length = strlen(r->secrets[i]);
            const char *position = r->line;
            while ((position = strstr(position, r->secrets[i]))) {
                memset(mask + (size_t)(position - r->line), 1, length);
                position += length;
            }
        }
        for (size_t i = 0; !fixed && i < r->used;) {
            const char *part = mask[i] ? "[REDACTED]" : r->line + i;
            size_t length = mask[i] ? 10 : 1;
            if (used + length > GSTD_LOG_LINE_MAX) {
                fixed = "[OMITTED: redacted log line exceeds bound]";
                break;
            }
            memcpy(output + used, part, length);
            used += length;
            if (mask[i])
                while (i < r->used && mask[i])
                    i++;
            else
                i++;
        }
    }
    if (!suppress)
        result = fixed ? emit(r, fixed, strlen(fixed)) : emit(r, output, used);
    wipe(output, sizeof(output));
    wipe(r->line, r->used);
    r->used = 0;
    r->omitted = 0;
    return result;
}
gstd_redactor *gstd_redactor_open(const char *const *secrets, size_t count, gstd_redacted_sink sink,
                                  void *context) {
    if (count > 64 || (count && !secrets) || !sink)
        return NULL;
    gstd_redactor *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->sink = sink;
    r->context = context;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        size_t size = secrets[i] ? strnlen(secrets[i], 4097) : 0;
        if (!size || size > 4096 || size > 65536 - total || strchr(secrets[i], '\n') ||
            strchr(secrets[i], '\r')) {
            gstd_redactor_close(r);
            return NULL;
        }
        json_t *valid = json_stringn(secrets[i], size);
        if (!valid) {
            gstd_redactor_close(r);
            return NULL;
        }
        json_decref(valid);
        r->secrets[i] = strdup(secrets[i]);
        if (!r->secrets[i]) {
            gstd_redactor_close(r);
            return NULL;
        }
        r->count++;
        total += size;
    }
    return r;
}
int gstd_redactor_feed(gstd_redactor *r, const void *input, size_t size) {
    if (!r || r->failed || (size && !input) || size > 65536)
        return -1;
    const unsigned char *bytes = input;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = bytes[i];
        if (c == '\n') {
            if (line(r))
                return -1;
        } else if (!c || r->used == GSTD_LOG_LINE_MAX)
            r->omitted = 1;
        else if (!r->omitted)
            r->line[r->used++] = (char)c;
    }
    return 0;
}
int gstd_redactor_finish(gstd_redactor *r) {
    if (!r || r->failed)
        return -1;
    return r->used || r->omitted ? line(r) : 0;
}
void gstd_redactor_close(gstd_redactor *r) {
    if (!r)
        return;
    for (size_t i = 0; i < r->count; i++) {
        wipe(r->secrets[i], strlen(r->secrets[i]));
        free(r->secrets[i]);
    }
    wipe(r, sizeof(*r));
    free(r);
}
