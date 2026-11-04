#ifndef GITSTORED_WAIT_H
#define GITSTORED_WAIT_H
#include "gitstored/inspect.h"
/* Nonblocking observation. Caller schedules monotonic deadline checks; no writes. */
gstd_query_status gstd_wait_poll(sqlite3 *db, const char *operation, const char *until,
                                 json_t **result);
#endif
