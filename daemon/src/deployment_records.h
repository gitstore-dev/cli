#ifndef GSTD_DEPLOYMENT_RECORDS_H
#define GSTD_DEPLOYMENT_RECORDS_H
#include "bootstrap.h"
#include "filesystem_job.h"
#include "helper.h"
#include "oidc_enrollment.h"
#include <sqlite3.h>
typedef struct {
    char operation_id[65], deployment_id[65], store_id[33];
    uint64_t generation;
    json_t *manifest, *components;
    int allow_provision;
    int migration; /* private metadata target; cannot run a deployment graph */
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
/* Persistent startup binds its graph to the reserved immutable volume ledger
 * before any allocation worker runs. Only this operation's unallocated volume
 * enables initial schema creation/migration; retained volumes use check mode.
 * A retry reuses the recorded graph, never recomputes freshness after effects. */
int gstd_deployment_persistent_graph_begin(sqlite3 *, const gstd_deployment_target *,
                                           const char *configuration_id, const char *volume_id,
                                           gstd_bootstrap_graph *);
int gstd_deployment_graph_step(sqlite3 *, const gstd_deployment_target *,
                               const char *configuration_id, gstd_bootstrap_graph *,
                               gstd_bootstrap_stage, int result, const char *uid);
/* Startup-helper journal only, inside the caller's transaction. Reserve before
 * native CREATE; commit the kernel-bound created identity before permit_start;
 * commit observed deletion before retry/graph advance. Mutating purposes are
 * never retried. Metadata is not execution or datastore lease authority. */
int gstd_deployment_helper_reserve(sqlite3 *, const gstd_deployment_target *,
                                   const char *configuration_id, const char *helper_id,
                                   gstd_execution_purpose);
/* Standalone migration helpers use the same kernel identity/deletion receipts.
 * Read-only purposes stay in preflight; the one schema mutator atomically
 * advances prepared -> dispatched/unsafe before any CREATE. No keyspace creation. */
int gstd_migration_helper_reserve(sqlite3 *, const gstd_deployment_target *, const char *,
                                  const char *, gstd_execution_purpose);
/* Fence source shutdown against every helper journal in the same transaction.
 * A dispatch without an observed deletion blocks shutdown, including read-only
 * helpers. Once stopping is committed, no new helper may be reserved. */
int gstd_migration_service_stopping(sqlite3 *, const gstd_deployment_target *,
                                    const gstd_native_context *);
/* After successful non-forced DELETE, while the exact pidfd/cgroup are held.
 * Records retained native termination only; volume/network owners stay held. */
int gstd_migration_service_deleted(sqlite3 *, const gstd_deployment_target *,
                                   const gstd_native_context *, const gstd_native_population *,
                                   int death_fd);
int gstd_deployment_helper_created(sqlite3 *, const gstd_deployment_target *, const char *helper_id,
                                   const gstd_helper *);
int gstd_deployment_helper_finished(sqlite3 *, const gstd_deployment_target *,
                                    const char *helper_id, const gstd_helper *);
/* Read-only recovery of a completed startup helper. Requires its exact policy,
 * committed deletion receipt and absence of the original native instance/init.
 * Unknown/active/malformed helpers fail closed; no retry or lease is granted. */
int gstd_deployment_helper_receipt_read(const gstd_deployment_target *, const char *helper_id,
                                        const json_t *, const char *boot_id, int native_fd);
int gstd_deployment_graph_recovery_read(const gstd_deployment_target *, const json_t *,
                                        gstd_bootstrap_graph *);
/* Persist exact rootfs/bundle inode receipts before native CREATE. Caller holds
 * a short transaction and registered workload scope. Idempotent only for the
 * same handles; no filesystem allocation, adoption or side effects occur here. */
int gstd_deployment_execution_record(sqlite3 *, const gstd_deployment_target *,
                                     const char *resource_id, const gstd_filesystem_reply *scope,
                                     int rootfs_fd);
/* Private service receipt, caller transaction. Bind the prepared execution
 * inodes and immutable native identity; this write alone grants no execution. */
int gstd_deployment_native_record(sqlite3 *, const gstd_deployment_target *,
                                  const gstd_native_context *, const char *stage);
/* Persist the captured cgroup path/inode pair after CREATE, before START. */
int gstd_deployment_population_record(sqlite3 *, const gstd_deployment_target *,
                                      const gstd_native_context *, const gstd_native_population *);
/* Compare the complete in-memory graph with its current held configuration
 * receipt inside the caller transaction, before dispatching another effect. */
int gstd_deployment_graph_matches(sqlite3 *, const gstd_deployment_target *,
                                  const char *configuration_id, const gstd_bootstrap_graph *);
/* One-shot initial creation grant, persisted before dispatch. Repeat/recovery
 * never grants regeneration. A prior reservation for this deployment also denies
 * replacement; workers still read/reuse existing canonical material first. */
int gstd_deployment_credential_reserve(sqlite3 *, const gstd_deployment_target *,
                                       const char *configuration_id, int *allow_initial);
/* Private core/2 OIDC material reservation. Caller transaction; commit before
 * invoking the broker/filesystem worker. Same-operation replay and every later
 * generation deny regeneration. Reference is public and purpose-bound, never a
 * credential value. Partial/foreign reservations fail without repair. */
int gstd_deployment_oidc_credential_reserve(sqlite3 *, const gstd_deployment_target *,
                                            const char *configuration_id, char reference[65],
                                            int *allow_initial);
/* Public-only receipt of private preparation. Caller transaction, after the
 * committed material reservation and successful filesystem reply. Records only
 * fixed ports and four read-only file identities; never reads configuration
 * bytes or material fingerprints. Replays must match every recorded identity.
 * This receipt grants no provider execution or enrollment authority. */
int gstd_deployment_oidc_prepared_record(sqlite3 *, const gstd_deployment_target *,
                                         const char *configuration_id, int configuration_fd,
                                         const gstd_oidc_ports *, const gstd_filesystem_reply *);
/* Reserve the original completed preparation before a later generation's
 * filesystem dispatch. 0=prior selected, 1=first generation/no prior, -1=blocked.
 * Caller transaction; commit before reopening the returned public-only scope
 * snapshot read-only and passing its data FD to the private material worker.
 * No filesystem lookup, repair or credential value is performed here. */
int gstd_deployment_oidc_preparation_source(sqlite3 *, const gstd_deployment_target *,
                                            const char *configuration_id, char source_id[65],
                                            json_t **source_scope);
/* Parent enrollment journal only, caller transaction. Returned records are
 * speculative until COMMIT; discard on rollback. Caller separately proves live
 * native/cgroup/network/writer capabilities and provider readiness. These
 * metadata functions never send HTTP or grant resource adoption authority.
 * Commit begin, then commit dispatch BEFORE launching one CREATE job. Repeated
 * dispatch is refused; a public match never acknowledges creation. */
int gstd_deployment_oidc_enrollment_begin(sqlite3 *, const gstd_deployment_target *,
                                          const char *configuration_id, const gstd_oidc_client *,
                                          json_t **record);
int gstd_deployment_oidc_enrollment_dispatch(sqlite3 *, const gstd_deployment_target *,
                                             const char *configuration_id, const gstd_oidc_client *,
                                             const json_t *expected, json_t **record);
/* Preserve a known original job result even after STOP changes current intent.
 * Original configuration/volume ownership must remain held or uncertain until
 * the worker is reaped and this receipt commits. Never releases or clears any
 * recovery state. Replay of the same result is metadata-only and idempotent. */
int gstd_deployment_oidc_enrollment_observe(sqlite3 *, const gstd_deployment_target *,
                                            const char *configuration_id, const gstd_oidc_client *,
                                            const json_t *expected, gstd_oidc_http_status,
                                            json_t **record);
/* Private OIDC mutation journal. Requires the exact registered migration plan,
 * prepared scope/rootfs, committed owned network attachment and the provider's
 * canonical data/writer capabilities retained by the caller. Never substitutes
 * a Scylla source or infers migration authority from allocation freshness.
 * Commit before CREATE. Reuse helper_created/finished for kernel-bound receipts.
 * Same-operation dispatch is single-attempt; failed/uncertain prior generations
 * block another dispatch against the retained provider volume. */
int gstd_deployment_oidc_helper_reserve(sqlite3 *, const gstd_deployment_target *,
                                        const char *configuration_id, const gstd_native_context *);
/* Read-only prior migration validation: count verified released reservations or
 * successfully deleted helpers, -1 for any uncertain/invalid history. */
int gstd_deployment_oidc_helper_history(sqlite3 *, const gstd_deployment_target *, const json_t *,
                                       const gstd_native_context *);
/* Caller transaction, read-only retained port selection before new network
 * effects. 0=exact prior tuple, 1=no prior prepared family, -1=blocked/corrupt.
 * Incomplete reservations never authorize replacement/repair or new ports. */
int gstd_deployment_oidc_ports_read(sqlite3 *, const gstd_deployment_target *, gstd_oidc_ports *);
/* Pure immutable OIDC recovery predicates; no dispatch/retry or writer grant. */
int gstd_deployment_oidc_recovery_configuration_read(const gstd_deployment_target *, const json_t *,
                                                     gstd_oidc_ports *);
int gstd_deployment_oidc_helper_receipt_read(const gstd_deployment_target *, const char *,
                                             const json_t *, const json_t *, const char *, int);
#endif
