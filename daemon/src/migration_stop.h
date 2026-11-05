#ifndef GSTD_MIGRATION_STOP_H
#define GSTD_MIGRATION_STOP_H
#include "datastore.h"
typedef struct {
    gstd_datastore *datastore;
    gstd_native_job native;
    int stage, outcome;
    int64_t deadline_ms;
    char stop_operation[65];
    uint64_t control_generation;
} gstd_migration_stop;
/* Private standalone source shutdown. Commit the helper/source journal fence
 * before any signal. Only the exact held service identity is signalled; no
 * mutator cancellation, forced delete, owner release or migration success. */
int gstd_migration_stop_begin(gstd_datastore *, gstd_migration_stop *);
/* Recovered failed migration, after all helpers are proven terminated. The
 * separately running STOP owns a newer claimed control generation. */
int gstd_migration_stop_recovered_begin(gstd_datastore *, const char *stop_operation,
                                        uint64_t control_generation, gstd_migration_stop *);
/* 0 pending,1 finished,-1 invalid. Outcome0 means observed native deletion;
 * -2 preserves all owners and observer handles for reconciliation. */
int gstd_migration_stop_poll(gstd_migration_stop *);
#endif
