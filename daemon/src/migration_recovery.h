#ifndef GSTD_MIGRATION_RECOVERY_H
#define GSTD_MIGRATION_RECOVERY_H
#include "migration_recovery_records.h"
#include "migration_stop.h"
typedef struct {
    const gstd_deployment_environment *environment;
    gstd_migration_execution execution;
    gstd_datastore datastore;
    gstd_migration_stop stop;
    gstd_filesystem_job filesystem;
    gstd_native_job native;
    gstd_filesystem_reply configuration, network_scope;
    gstd_network_worker network;
    gstd_network_worker_observation observed;
    gstd_volume_identity expected_volume;
    json_t *expected, *discovered[3];
    char stop_operation[65], configuration_id[65], network_id[65], boot[37], lifecycle[16];
    uint64_t control_generation;
    int stage, outcome, index, discovering, network_present, network_none, keeper_death_fd,
        native_intent, mutation_dispatched;
    int64_t deadline_ms;
} gstd_migration_recovery;
/* Recover observers of one failed uncertain migration under a separately
 * running STOP. No CREATE, START, schema retry, credential access or allocation.
 * A running helper is allowed to terminate naturally; never signal a mutator.
 * Borrow environment through close; actor initially zero. */
int gstd_migration_recovery_begin(const gstd_deployment_environment *, const char *migration,
                                  const char *stop, gstd_migration_recovery *);
/* Nonblocking 0 pending,1 finished,-1 invalid. Outcome0 means observed owned
 * cleanup; the original migration remains failed. Unknown/inaccessible evidence
 * retains uncertainty and never grants replacement or a successful migration. */
int gstd_migration_recovery_poll(gstd_migration_recovery *);
/* Observer closure only after all short workers are reaped. */
int gstd_migration_recovery_close(gstd_migration_recovery *);
#endif
