#ifndef GSTD_LOGS_H
#define GSTD_LOGS_H
#include "streams.h"
#include <stdint.h>
typedef struct gstd_logs gstd_logs;
typedef struct {
    size_t segment_bytes, deployment_bytes, global_bytes;
    unsigned segments, records, max_age_seconds;
} gstd_log_limits;
#define GSTD_LOG_LIMITS                                                                            \
    {1024u * 1024u, 64u * 1024u * 1024u, 1024u * 1024u * 1024u, 1024u, 131072u, 604800u}
typedef enum {
    GSTD_LOG_OK,
    GSTD_LOG_INVALID,
    GSTD_LOG_IO,
    GSTD_LOG_CURSOR_EXPIRED,
    GSTD_LOG_LIMIT
} gstd_log_status;
/* Filesystem worker/collector only, outside the control-owner transaction.
 * Separate private SQLite store; never consumes/deletes operation metadata.
 * Limits may only tighten the published ceilings. Physical DB+journal capacity
 * is bounded too, so capacity pressure may expire logs earlier than payload caps. */
gstd_log_status gstd_logs_open(const char *path, const gstd_log_limits *, gstd_logs **);
void gstd_logs_close(gstd_logs *);
/* Worker-only, fixed dedicated directory and store marker. No operation DB or
 * repository capability is retained by the collector after narrowing its root. */
int gstd_logs_directory(int state_fd, const char *store_id, int allow_create);
/* allow_create=0 is read-only and never repairs or creates a missing index. */
gstd_log_status gstd_logs_open_at(int directory_fd, const gstd_log_limits *, int allow_create,
                                  gstd_logs **);
/* Already-redacted bounded JSON only. Cursor/component/time are canonical store
 * fields, never supplied by a wire client or read from a service's raw output. */
gstd_log_status gstd_logs_append(gstd_logs *, const char *deployment, const char *component,
                                 gstd_stream_kind, const json_t *redacted, int64_t now);
gstd_log_status gstd_logs_prune(gstd_logs *, int64_t now);
/* At most 256 records per page. Unknown, foreign, old-index or evicted cursors
 * expire explicitly. Results are public payloads with cursor/component/time. */
gstd_log_status gstd_logs_replay(gstd_logs *, const char *deployment, const char *component,
                                 gstd_stream_kind, const char *cursor, unsigned limit, int64_t now,
                                 json_t **records);
size_t gstd_logs_retained_bytes(gstd_logs *);
#endif
