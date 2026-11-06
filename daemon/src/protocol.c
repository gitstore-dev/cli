#include "gitstored/protocol.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(JSON_PARSER_MAX_DEPTH == 33, "The private Jansson decoder depth must stay bounded");

#define JSON_LIVE_BUDGET (64u * 1024u * 1024u)
#define JSON_PARSE_BUDGET (16u * 1024u * 1024u)
typedef union {
    max_align_t alignment;
    size_t bytes;
} allocation_header;
static size_t live_bytes, parse_bytes;
static int parsing, initialized;
#ifdef GITSTORED_TESTING
static size_t allocation_remaining = SIZE_MAX;
void gstd_test_json_fail_after(size_t count) { allocation_remaining = count; }
#endif
static void *bounded_allocate(size_t bytes) {
#ifdef GITSTORED_TESTING
    if (parsing && allocation_remaining != SIZE_MAX) {
        if (!allocation_remaining)
            return NULL;
        allocation_remaining--;
    }
#endif
    if (bytes > JSON_LIVE_BUDGET - live_bytes ||
        (parsing && bytes > JSON_PARSE_BUDGET - parse_bytes))
        return NULL;
    allocation_header *header = malloc(sizeof(*header) + bytes);
    if (header == NULL)
        return NULL;
    header->bytes = bytes;
    live_bytes += bytes;
    if (parsing)
        parse_bytes += bytes;
    return header + 1;
}
static void bounded_free(void *pointer) {
    if (pointer == NULL)
        return;
    allocation_header *header = (allocation_header *)pointer - 1;
    live_bytes -= header->bytes;
    free(header);
}
/* Single control owner; install before any Jansson allocation. */
void gstd_json_initialize(void) {
    if (!initialized) {
        json_set_alloc_funcs(bounded_allocate, bounded_free);
        initialized = 1;
    }
}
char *gstd_json_dump(const json_t *value) {
    if (!value)
        return NULL;
    size_t length = json_dumpb(value, NULL, 0, JSON_COMPACT | JSON_SORT_KEYS);
    if (!length || length > GSTD_FRAME_MAX)
        return NULL;
    char *bytes = malloc(length + 1);
    if (!bytes)
        return NULL;
    if (json_dumpb(value, bytes, length, JSON_COMPACT | JSON_SORT_KEYS) != length) {
        free(bytes);
        return NULL;
    }
    bytes[length] = 0;
    return bytes;
}
static int bounded_structure(const unsigned char *s, size_t length) {
    unsigned depth = 0;
    int string = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = s[i];
        if (string) {
            if (c == '\\') {
                if (++i == length)
                    return 0;
            } else if (c == '"')
                string = 0;
        } else if (c == '"')
            string = 1;
        else if (c == '{' || c == '[') {
            if (++depth > GSTD_DEPTH_MAX)
                return 0;
        } else if (c == '}' || c == ']') {
            if (depth == 0)
                return 0;
            --depth;
        }
    }
    return depth == 0 && !string;
}
static int bounded_value(const json_t *value) {
    if (json_is_real(value))
        return 0;
    if (json_is_integer(value)) {
        json_int_t n = json_integer_value(value);
        return n >= -GSTD_SAFE_INTEGER && n <= GSTD_SAFE_INTEGER;
    }
    if (json_is_string(value))
        return json_string_length(value) <= GSTD_STRING_MAX;
    if (json_is_array(value)) {
        if (json_array_size(value) > GSTD_ITEMS_MAX)
            return 0;
        for (size_t i = 0; i < json_array_size(value); ++i)
            if (!bounded_value(json_array_get(value, i)))
                return 0;
    } else if (json_is_object(value)) {
        if (json_object_size(value) > GSTD_ITEMS_MAX)
            return 0;
        for (void *i = json_object_iter((json_t *)value); i;
             i = json_object_iter_next((json_t *)value, i)) {
            if (json_object_iter_key_len(i) > GSTD_STRING_MAX ||
                !bounded_value(json_object_iter_value(i)))
                return 0;
        }
    }
    return 1;
}
json_t *gstd_json_parse(const unsigned char *bytes, size_t length) {
    if (bytes == NULL || length == 0 || length > GSTD_FRAME_MAX ||
        !bounded_structure(bytes, length))
        return NULL;
    gstd_json_initialize();
    parse_bytes = 0;
    parsing = 1;
    json_error_t error;
    json_t *value =
        json_loadb((const char *)bytes, length, JSON_REJECT_DUPLICATES | JSON_ALLOW_NUL, &error);
    parsing = 0;
    if (value != NULL && (!json_is_object(value) || !bounded_value(value))) {
        json_decref(value);
        return NULL;
    }
    return value;
}
int gstd_identifier(const char *bytes, size_t length) {
    if (bytes == NULL || length == 0 || length > 64)
        return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)bytes[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == ':' || c == '-'))
            return 0;
    }
    return 1;
}
void gstd_frame_reset(gstd_frame *frame) {
    free(frame->payload);
    memset(frame, 0, sizeof(*frame));
}
int gstd_frame_feed(gstd_frame *f, const unsigned char *bytes, size_t length, size_t *consumed) {
    *consumed = 0;
    if (f->failed)
        return -1;
    while (f->prefix_used < 4 && *consumed < length)
        f->prefix[f->prefix_used++] = bytes[(*consumed)++];
    if (f->prefix_used < 4)
        return 0;
    if (f->payload == NULL) {
        uint32_t n = ((uint32_t)f->prefix[0] << 24) | ((uint32_t)f->prefix[1] << 16) |
                     ((uint32_t)f->prefix[2] << 8) | f->prefix[3];
        if (n == 0 || n > GSTD_FRAME_MAX) {
            f->failed = 1;
            return -1;
        }
        f->expected = n;
        f->payload = malloc(n);
        if (f->payload == NULL) {
            f->failed = 1;
            return -1;
        }
    }
    size_t available = length - *consumed;
    size_t need = f->expected - f->used;
    size_t count = available < need ? available : need;
    if (count)
        memcpy(f->payload + f->used, bytes + *consumed, count);
    f->used += count;
    *consumed += count;
    return f->used == f->expected ? 1 : 0;
}
int gstd_frame_eof(const gstd_frame *f) {
    return !f->failed &&
           (f->prefix_used == 0 || (f->prefix_used == 4 && f->expected && f->used == f->expected));
}

typedef struct {
    const char *name, *required, *optional;
} method_schema;
static const method_schema methods[] = {
    {"system.negotiate", "supported requiredCapabilities", ""},
    {"system.capabilities", "", ""},
    {"deployment.validate", "profileId profileVersion",
     "components datastore deploymentId allowProvision"},
    {"deployment.start", "deploymentId profileId profileVersion",
     "components datastore allowProvision assetManifestDigest"},
    {"deployment.inspect", "deploymentId", ""},
    {"deployment.list", "", "limit cursor"},
    {"deployment.stop", "deploymentId", "expectedOperationId"},
    {"operation.inspect", "operationId", ""},
    {"operation.wait", "operationId until", ""},
    {"operation.cancel", "operationId", ""},
    {"migration.start", "deploymentId targetVersion backupAcknowledged", ""},
    {"stream.subscribe", "deploymentId kind", "component cursor"},
    {"stream.unsubscribe", "subscriptionId", ""}};
static int string_equal(json_t *value, const char *expected) {
    return json_is_string(value) && json_string_length(value) == strlen(expected) &&
           memcmp(json_string_value(value), expected, strlen(expected)) == 0;
}
static int one_of(json_t *value, const char *choices) {
    if (!json_is_string(value))
        return 0;
    size_t length = json_string_length(value);
    for (const char *p = choices; *p;) {
        const char *end = strchr(p, ' ');
        size_t count = end ? (size_t)(end - p) : strlen(p);
        if (count == length && memcmp(json_string_value(value), p, count) == 0)
            return 1;
        p = end ? end + 1 : p + count;
    }
    return 0;
}
static int field_allowed(const char *name, size_t len, const char *fields) {
    for (const char *p = fields; *p;) {
        const char *end = strchr(p, ' ');
        size_t count = end ? (size_t)(end - p) : strlen(p);
        if (len == count && memcmp(name, p, count) == 0)
            return 1;
        p = end ? end + 1 : p + count;
    }
    return 0;
}
static int is_identifier(json_t *v) {
    return json_is_string(v) && gstd_identifier(json_string_value(v), json_string_length(v));
}
static int version_parse(json_t *v, gstd_protocol_version *out) {
    if (!json_is_object(v) || json_object_size(v) != 2)
        return 0;
    json_t *major = json_object_get(v, "major"), *minor = json_object_get(v, "minor");
    if (!json_is_integer(major) || !json_is_integer(minor))
        return 0;
    json_int_t a = json_integer_value(major), b = json_integer_value(minor);
    if (a < 0 || b < 0 || a > UINT_MAX || b > UINT_MAX)
        return 0;
    out->major = (unsigned)a;
    out->minor = (unsigned)b;
    return 1;
}
static int valid_param(const char *key, size_t len, json_t *v) {
    if (field_allowed(key, len,
                      "profileId profileVersion deploymentId operationId subscriptionId "
                      "targetVersion expectedOperationId"))
        return is_identifier(v);
    if (field_allowed(key, len, "assetManifestDigest")) {
        if (!json_is_string(v) || json_string_length(v) != 71)
            return 0;
        const char *text = json_string_value(v);
        if (strncmp(text, "sha256:", 7))
            return 0;
        for (unsigned i = 7; i < 71; i++)
            if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
                return 0;
        return 1;
    }
    if (field_allowed(key, len, "allowProvision backupAcknowledged"))
        return json_is_boolean(v);
    if (field_allowed(key, len, "limit"))
        return json_is_integer(v) && json_integer_value(v) >= 1 && json_integer_value(v) <= 100;
    if (field_allowed(key, len, "cursor"))
        return json_is_string(v) && json_string_length(v) > 0;
    if (field_allowed(key, len, "datastore"))
        return one_of(v, "memdb scylladb");
    if (field_allowed(key, len, "component"))
        return one_of(v, "api git controller oidc oidc-smtp oidc-hydra oidc-kratos oidc-ui oidc-bridge admin storefront scylladb");
    if (field_allowed(key, len, "until"))
        return one_of(v, "accepted terminal ready");
    if (field_allowed(key, len, "kind"))
        return one_of(v, "logs events");
    if (field_allowed(key, len, "supported")) {
        if (!json_is_array(v) || json_array_size(v) < 1 || json_array_size(v) > 16)
            return 0;
        for (size_t i = 0; i < json_array_size(v); ++i) {
            gstd_protocol_version version;
            if (!version_parse(json_array_get(v, i), &version))
                return 0;
        }
        return 1;
    }
    if (field_allowed(key, len, "requiredCapabilities components")) {
        int components = field_allowed(key, len, "components");
        if (!json_is_array(v) || json_array_size(v) > (components ? 6u : 64u))
            return 0;
        for (size_t i = 0; i < json_array_size(v); ++i) {
            json_t *item = json_array_get(v, i);
            if (!json_is_string(item) || json_string_length(item) == 0)
                return 0;
            if (components && !one_of(item, "api git controller oidc admin storefront"))
                return 0;
            for (size_t j = 0; j < i; ++j)
                if (json_equal(item, json_array_get(v, j)))
                    return 0;
        }
        return 1;
    }
    return 0;
}
int gstd_request_parse(json_t *message, gstd_request *r) {
    memset(r, 0, sizeof(*r));
    if (!version_parse(json_object_get(message, "version"), &r->version))
        return -1;
    json_t *id = json_object_get(message, "requestId"),
           *method = json_object_get(message, "method");
    json_t *params = json_object_get(message, "params"),
           *key = json_object_get(message, "idempotencyKey"),
           *timeout = json_object_get(message, "timeoutMs");
    if (!is_identifier(id) || !json_is_object(params) || !json_is_string(method))
        return -1;
    r->request_id = json_string_value(id);
    r->params = params;
    size_t selected = sizeof(methods) / sizeof(*methods);
    for (size_t i = 0; i < sizeof(methods) / sizeof(*methods); ++i)
        if (string_equal(method, methods[i].name)) {
            selected = i;
            break;
        }
    if (selected == sizeof(methods) / sizeof(*methods))
        return -2;
    r->method = (gstd_method)selected;
    if (key) {
        if (!is_identifier(key))
            return -1;
        r->idempotency_key = json_string_value(key);
    }
    if ((r->method == GSTD_START || r->method == GSTD_STOP || r->method == GSTD_CANCEL ||
         r->method == GSTD_MIGRATE) &&
        !key)
        return -1;
    r->timeout_ms = (r->method == GSTD_WAIT) ? 60000u : 30000u;
    if (timeout) {
        if (!json_is_integer(timeout) || json_integer_value(timeout) < 1 ||
            json_integer_value(timeout) > GSTD_TIMEOUT_MAX)
            return -1;
        r->timeout_ms = (unsigned)json_integer_value(timeout);
    }
    const method_schema *schema = &methods[selected];
    for (void *i = json_object_iter(params); i; i = json_object_iter_next(params, i)) {
        const char *name = json_object_iter_key(i);
        size_t n = json_object_iter_key_len(i);
        if ((!field_allowed(name, n, schema->required) &&
             !field_allowed(name, n, schema->optional)) ||
            !valid_param(name, n, json_object_iter_value(i)))
            return -1;
    }
    for (const char *p = schema->required; *p;) {
        const char *end = strchr(p, ' ');
        size_t count = end ? (size_t)(end - p) : strlen(p);
        if (!json_object_getn(params, p, count))
            return -1;
        p = end ? end + 1 : p + count;
    }
    return 0;
}
