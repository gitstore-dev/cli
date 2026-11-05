#ifndef GSTD_MIGRATION_RUNTIME_H
#define GSTD_MIGRATION_RUNTIME_H
#include "migration_schema.h"
#include "migration_stop.h"
typedef struct {
    const gstd_deployment_environment *environment;
    gstd_migration_execution execution;
    gstd_datastore datastore;
    gstd_migration_schema schema;
    gstd_migration_stop stop;
    gstd_filesystem_job filesystem;
    gstd_filesystem_reply configuration, network_scope;
    gstd_network_worker network;
    gstd_network_worker_observation observed;
    char configuration_id[65], network_id[65], boot[37];
    int resolver_fd, keeper_death_fd;
    int stage, outcome, cleanup_observed, schema_succeeded;
    int64_t deadline_ms;
} gstd_migration_runtime;
/* Parent-owned standalone coordinator. A running admitted operation and reopened
 * canonical environment are required. Borrow environment through close. No
 * API/Git/controller startup, credentials, caller commands or keyspace creation. */
int gstd_migration_runtime_begin(const gstd_deployment_environment *, const char *operation,
                                 gstd_migration_runtime *);
/* Nonblocking 0 pending,1 finished,-1 invalid. Writes the terminal operation only
 * after schema verification and observed cleanup, or records uncertain ownership
 * without signalling a surviving/unknown helper. Scheduler completion is caller
 * owned. Metadata write failure leaves finalization pending for retry. */
int gstd_migration_runtime_poll(gstd_migration_runtime *, int cancellation_fd);
/* Terminal and reaped only. Observer closure never deletes scopes/data, signals
 * workloads or releases unknown owners. */
int gstd_migration_runtime_close(gstd_migration_runtime *);
#endif
