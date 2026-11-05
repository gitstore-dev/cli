#ifndef GSTD_MIGRATION_EXECUTION_H
#define GSTD_MIGRATION_EXECUTION_H
#include "deployment_records.h"
typedef struct {
    gstd_deployment_target target;
    char source_operation[65];
} gstd_migration_execution;
/* Caller transaction, metadata only. Bind a running admitted migration to the
 * stopped source deployment's retained policy and current registered target.
 * No filesystem/native/credential effects or writer authority are granted. */
int gstd_migration_execution_read(sqlite3 *, const char *operation, gstd_migration_execution *);
/* Read-only identity snapshot during active prepared/dispatched/cleanup phases.
 * Does not authorize new preparation or dispatch; stage-specific owners must
 * verify their committed journal and held kernel capabilities separately. */
int gstd_migration_execution_observe(sqlite3 *, const char *, gstd_migration_execution *);
/* Observation-only target for a failed uncertain migration under a separately
 * running current STOP. Never authorizes preparation/helper dispatch or terminal
 * outcome rewriting. Requires retained original and migration configuration. */
int gstd_migration_execution_recovery_read(sqlite3 *, const char *migration_operation,
                                           const char *stop_operation, gstd_migration_execution *);
void gstd_migration_execution_clear(gstd_migration_execution *);
#endif
