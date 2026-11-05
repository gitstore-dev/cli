#ifndef GSTD_MIGRATION_H
#define GSTD_MIGRATION_H
#include "gitstored/operations.h"
/* Control metadata only. These functions never execute a helper, acquire a
 * datastore, generate credentials or enable the candidate execution backend. */
typedef enum {
    GSTD_MIG_PREFLIGHT,
    GSTD_MIG_PREPARED,
    GSTD_MIG_DISPATCHED,
    GSTD_MIG_VERIFIED,
    GSTD_MIG_CLEANUP,
    GSTD_MIG_SUCCEEDED,
    GSTD_MIG_FAILED,
    GSTD_MIG_UNCERTAIN
} gstd_migration_stage;
typedef struct {
    int keyspace_exists, schema_compatible, datastore_exclusive, previous_helper_terminated;
} gstd_migration_observation;
json_t *gstd_migration_target(const char *version);
/* Acceptance calls these inside its existing transaction; no backend I/O. */
gstd_accept_status gstd_migration_admit(sqlite3 *, const gstd_request *);
int gstd_migration_reserve(sqlite3 *, const gstd_request *, const char *operation);
/* Parent owner supplies private backend observations, never wire booleans.
 * Policy readiness is not execution authority. Native dispatch stays gated by
 * registered image/resource preparation and observed ownership. */
gstd_accept_status gstd_migration_check(sqlite3 *, const char *,
                                        const gstd_migration_observation *);
/* Prepare requires current private observations inside the same transaction as
 * its durable stage change. The generic transition cannot bypass preflight. */
int gstd_migration_prepare(sqlite3 *, const char *, const gstd_migration_observation *);
/* Caller transaction required; source/target/stage/owner bindings are checked. */
int gstd_migration_transition(sqlite3 *, const char *, gstd_migration_stage from,
                              gstd_migration_stage to);
int gstd_migration_finish(sqlite3 *, const char *, int succeeded, int cleanup_observed);
/* -1 unknown/corrupt,0 unsafe/blocked,1 safe/allowed. Terminal cancel is handled
 * as an independent no-op by the generic control path. */
int gstd_migration_cancel_safe(sqlite3 *, const char *);
/* Caller transaction; only queued/reserved accepted intent with its single
 * control row and no dispatched preparation can be cancelled without effects. */
int gstd_migration_cancel_queued(sqlite3 *, const char *);
int gstd_migration_start_allowed(sqlite3 *, const char *deployment);
json_t *gstd_migration_inspect(sqlite3 *, const char *operation);
#endif
