#ifndef GITSTORED_INSPECT_H
#define GITSTORED_INSPECT_H
#include "gitstored/protocol.h"
#include <sqlite3.h>
typedef enum {
    GSTD_QUERY_OK,
    GSTD_QUERY_PENDING,
    GSTD_QUERY_INVALID,
    GSTD_QUERY_NOT_FOUND,
    GSTD_QUERY_CURSOR_EXPIRED,
    GSTD_QUERY_LIMIT,
    GSTD_QUERY_DATABASE
} gstd_query_status;
gstd_query_status gstd_inspect(sqlite3 *db, gstd_method method, json_t *params, json_t **result);
#endif
