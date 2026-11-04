#ifndef GSTD_DEPLOYMENT_ASSETS_H
#define GSTD_DEPLOYMENT_ASSETS_H
#include "deployment_records.h"
#include "gitstored/asset_records.h"
typedef struct {
    sqlite3 *database;
    const gstd_deployment_target *target;
    char component[16], resource[65], key[65], staging_name[138];
    int cache_fd, source_fd, staging_fd, active_fd;
    int stage, outcome, detail;
    int64_t deadline_ms, retry_ms;
    uint64_t download_limit, merge_limit, cache_limit;
    gstd_filesystem_job worker;
    gstd_provision_result proof;
} gstd_deployment_asset;
/* Parent-owned asynchronous acquisition. Resource must already be reserved for
 * this accepted start. No caller transaction may be open. Target, cache and
 * optional offline source are borrowed until the actor finishes. No secret
 * lookup, hashing, transfer or filesystem mutation runs in the control parent.
 * Limits are owner policy, not lifecycle wire arguments. Initialize actor to zero. */
int gstd_deployment_asset_begin(sqlite3 *, const gstd_deployment_target *, const char *component,
                                const char *resource, int cache_fd, int offline_fd,
                                uint64_t download_limit, uint64_t merge_limit, uint64_t cache_limit,
                                gstd_deployment_asset *);
/* 0=pending, 1=finished, -1=invalid. Finished outcome 0 exposes active_fd owned
 * by this actor and freshly verified, -1=known failure, -2=uncertain. detail is
 * a non-secret asset/provision status. Interrupted actors retain DB references
 * and partial bytes; cleanup requires separate observed resource release. */
int gstd_deployment_asset_poll(gstd_deployment_asset *, int cancellation_fd);
/* Only after finished/no worker; closes owned handles, never unpins or deletes. */
int gstd_deployment_asset_close(gstd_deployment_asset *);
#endif
