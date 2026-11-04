#ifndef GITSTORED_OPERATIONS_H
#define GITSTORED_OPERATIONS_H
#include "gitstored/protocol.h"
#include <sqlite3.h>
typedef enum {
    GSTD_ACCEPT_NEW,
    GSTD_ACCEPT_REPLAY,
    GSTD_ACCEPT_INVALID,
    GSTD_ACCEPT_CONFLICT,
    GSTD_ACCEPT_DATABASE,
    GSTD_ACCEPT_LIMIT,
    GSTD_ACCEPT_DEPLOYMENT_CONFLICT,
    GSTD_ACCEPT_NOT_FOUND,
    GSTD_ACCEPT_OWNERSHIP_UNCERTAIN
} gstd_accept_status;
int gstd_fingerprint(const json_t *intent, char output[65]);
/* Resolved registered profile defaults are supplied by trusted profile validation.
 * This API persists intent only; it never schedules or executes a workload. */
gstd_accept_status gstd_accept(sqlite3 *db, const gstd_request *request,
                               const json_t *profile_components, const char *repository_volume_id,
                               char operation_id[65]);
#endif
