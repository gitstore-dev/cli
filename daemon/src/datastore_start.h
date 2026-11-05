#ifndef GSTD_DATASTORE_START_H
#define GSTD_DATASTORE_START_H
#include "deployment_records.h"
#include "volume_records.h"
typedef struct {
    sqlite3 *database;
    const gstd_deployment_target *target;
    const gstd_volume_root *root;
    char volume_id[65], producer_id[65];
    gstd_bootstrap_graph graph;
    gstd_filesystem_job worker;
    gstd_volume_handle volume;
    int stage, outcome, detail;
} gstd_datastore_start;
/* Persistent startup prefix. Configuration scope must already be recorded.
 * Borrow target/root for this actor's lifetime; no caller transaction is open.
 * Commit canonical ownership, volume reservation and graph freshness together
 * before dispatching the filesystem worker. This does not start a service,
 * mutate a keyspace, release ownership or enable a public capability. */
int gstd_datastore_start_begin(sqlite3 *, const gstd_deployment_target *, const gstd_volume_root *,
                               const char *configuration_id, gstd_datastore_start *);
/* 0=pending, 1=finished, -1=invalid. Successful output owns data/writer FDs;
 * transfer both to the service actor before clearing. An uncertain result is
 * never permission to replace a writer or repeat schema mutations. */
int gstd_datastore_start_poll(gstd_datastore_start *, int cancellation_fd);
/* Finished actors only. Close local capabilities; never delete or unreserve. */
int gstd_datastore_start_close(gstd_datastore_start *);
#endif
