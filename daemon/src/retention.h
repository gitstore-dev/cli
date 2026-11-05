#ifndef GSTD_RETENTION_H
#define GSTD_RETENTION_H
#include <sqlite3.h>
#include <stddef.h>
/* Sole database owner, inside the caller's acceptance transaction. Collection
 * removes only expired, unreferenced terminal history, never runtime resources.
 * 0=capacity available, 1=RESOURCE_LIMIT, -1=database/measurement failure. */
int gstd_retention_admit(sqlite3 *, int control, size_t intent_bytes);
/* Bounded maintenance; at most 256 records in one caller-owned transaction. */
int gstd_retention_collect(sqlite3 *, unsigned *removed);
#endif
