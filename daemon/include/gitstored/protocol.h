#ifndef GITSTORED_PROTOCOL_H
#define GITSTORED_PROTOCOL_H
#include <jansson.h>
#include <stddef.h>
#include <stdint.h>
#define GSTD_FRAME_MAX 1048576u
#define GSTD_STRING_MAX 262144u
#define GSTD_ITEMS_MAX 4096u
#define GSTD_DEPTH_MAX 32u
#define GSTD_SAFE_INTEGER INT64_C(9007199254740991)
#define GSTD_CONNECTIONS_MAX 32u
#define GSTD_REQUESTS_MAX 16u
#define GSTD_SUBSCRIPTIONS_MAX 8u
#define GSTD_WORKERS_MAX 8u
#define GSTD_PENDING_MAX 64u
#define GSTD_TIMEOUT_MAX 3600000u

typedef struct {
    unsigned major, minor;
} gstd_protocol_version;
typedef enum {
    GSTD_NEGOTIATE,
    GSTD_CAPABILITIES,
    GSTD_VALIDATE,
    GSTD_START,
    GSTD_INSPECT,
    GSTD_LIST,
    GSTD_STOP,
    GSTD_OPERATION_INSPECT,
    GSTD_WAIT,
    GSTD_CANCEL,
    GSTD_MIGRATE,
    GSTD_SUBSCRIBE,
    GSTD_UNSUBSCRIBE,
    GSTD_UNKNOWN_METHOD
} gstd_method;
typedef enum {
    GSTD_ACCEPTED,
    GSTD_RUNNING,
    GSTD_SUCCEEDED,
    GSTD_FAILED,
    GSTD_CANCELLED
} gstd_operation_state;
typedef enum { GSTD_NORMAL, GSTD_RECONCILING, GSTD_UNCERTAIN } gstd_recovery;
typedef enum { GSTD_PENDING, GSTD_READY, GSTD_NOT_READY } gstd_readiness;
typedef enum { GSTD_OWNED, GSTD_RELEASED, GSTD_OWNERSHIP_UNCERTAIN } gstd_ownership;
typedef struct {
    gstd_protocol_version version;
    const char *request_id;
    gstd_method method;
    json_t *params;
    const char *idempotency_key;
    unsigned timeout_ms;
} gstd_request;
typedef struct {
    unsigned char prefix[4];
    size_t prefix_used, expected, used;
    unsigned char *payload;
    int failed;
} gstd_frame;
/* Single control-thread use. Call before ANY Jansson allocation in the process. */
void gstd_json_initialize(void);
/* Bounded compact non-secret JSON in a caller-free() owned buffer. Jansson's
 * json_dumps allocation uses the installed parser allocator and is not free(). */
char *gstd_json_dump(const json_t *value);
/* Parsing also initializes hooks for decoder-only programs. */
json_t *gstd_json_parse(const unsigned char *bytes, size_t length);
/* 0 valid; -1 invalid argument; -2 unsupported method. Views borrow message. */
int gstd_request_parse(json_t *message, gstd_request *request);
int gstd_identifier(const char *bytes, size_t length);
void gstd_frame_reset(gstd_frame *frame);
/* Returns -1 invalid, 0 incomplete, 1 complete; consumed permits coalesced frames. */
int gstd_frame_feed(gstd_frame *frame, const unsigned char *bytes, size_t length, size_t *consumed);
int gstd_frame_eof(const gstd_frame *frame);
#endif
