#ifndef GSTD_MIGRATION_DATASTORE_H
#define GSTD_MIGRATION_DATASTORE_H
#include "migration_execution.h"
#include "volume_records.h"
typedef struct {
    sqlite3 *database;
    const gstd_volume_root *root;
    gstd_migration_execution execution;
    char volume_id[65], producer_id[65];
    gstd_filesystem_job worker;
    gstd_volume_handle volume;
    int stage, outcome, detail;
} gstd_migration_datastore;
/* Private existing-datastore preparation, zero-initialized actor. Commit the
 * admitted migration's canonical reservation before filesystem acquisition.
 * Never allocate a new datastore, run a helper or release a prior owner. */
int gstd_migration_datastore_begin(sqlite3 *, const gstd_volume_root *, const char *operation,
                                   gstd_migration_datastore *);
int gstd_migration_datastore_poll(gstd_migration_datastore *, int cancellation_fd);
/* Finished/reaped actors only. Closes local FDs/JSON, never ledger ownership. */
int gstd_migration_datastore_close(gstd_migration_datastore *);
#endif
