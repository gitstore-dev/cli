#ifndef GSTD_DEPLOYMENT_RECORDS_H
#define GSTD_DEPLOYMENT_RECORDS_H
#include "bootstrap.h"
#include "filesystem_job.h"
#include <sqlite3.h>
typedef struct {
    char operation_id[65], deployment_id[65], store_id[33];
    uint64_t generation;
    json_t *manifest, *components;
    int allow_provision;
} gstd_deployment_target;
/* Parent-only metadata. Load only the deployment's current start identity;
 * derive dependencies from the registered profile, never a caller config/path.
 * Target owns its JSON handles; clear after all referencing workers terminate. */
int gstd_deployment_target_read(sqlite3 *, const char *operation_id, gstd_deployment_target *);
/* Read only an unreleased original start's ledger-bound configuration identity.
 * Uses its recorded resource generation, even after a stop advanced desired
 * generation. Does not authorize execution, provisioning or credential creation. */
int gstd_deployment_target_recovery_read(sqlite3 *, const char *operation_id,
                                         gstd_deployment_target *);
void gstd_deployment_target_clear(gstd_deployment_target *);
/* These short operations require a caller transaction. Commit reservation and
 * graph state before filesystem/native/credential effects. No I/O worker, secret
 * lookup, raw config or public mutation executes here. Graph outputs are
 * speculative until COMMIT succeeds; discard/reload on rollback or commit error.
 * Network identity is managed separately by network_records and cannot be set
 * through scope_record. */
int gstd_deployment_resource_reserve(sqlite3 *, const gstd_deployment_target *,
                                     const char *resource_id, const char *kind);
int gstd_deployment_scope_record(sqlite3 *, const gstd_deployment_target *, const char *resource_id,
                                 const gstd_filesystem_reply *);
int gstd_deployment_graph_begin(sqlite3 *, const gstd_deployment_target *,
                                const char *configuration_id, gstd_bootstrap_graph *);
int gstd_deployment_graph_step(sqlite3 *, const gstd_deployment_target *,
                               const char *configuration_id, gstd_bootstrap_graph *,
                               gstd_bootstrap_stage, int result, const char *uid);
/* Persist exact rootfs/bundle inode receipts before native CREATE. Caller holds
 * a short transaction and registered workload scope. Idempotent only for the
 * same handles; no filesystem allocation, adoption or side effects occur here. */
int gstd_deployment_execution_record(sqlite3 *, const gstd_deployment_target *,
                                     const char *resource_id, const gstd_filesystem_reply *scope,
                                     int rootfs_fd);
/* One-shot initial creation grant, persisted before dispatch. Repeat/recovery
 * never grants regeneration. A prior reservation for this deployment also denies
 * replacement; workers still read/reuse existing canonical material first. */
int gstd_deployment_credential_reserve(sqlite3 *, const gstd_deployment_target *,
                                       const char *configuration_id, int *allow_initial);
#endif
