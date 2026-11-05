#ifndef GSTD_MIGRATION_RECOVERY_RECORDS_H
#define GSTD_MIGRATION_RECOVERY_RECORDS_H
#include "deployment_types.h"
#include "migration_execution.h"
/* Caller transactions. Only a separately accepted/running STOP may claim a
 * newer control generation. The original failed migration result stays intact.
 * These are metadata boundaries, never permission for helper CREATE/START. */
int gstd_migration_recovery_claim(sqlite3 *, const char *migration, const char *stop,
                                  gstd_migration_execution *, uint64_t *control_generation);
/* Classify a previously dispatched helper without granting START/retry. */
int gstd_migration_recovery_helper_active_read(const gstd_deployment_target *, const char *id,
                                               const json_t *, const char *boot,
                                               gstd_native_observation *);
/* Discovered OCI-created init only, never a running unjournaled application. */
int gstd_migration_recovery_native_discovered(sqlite3 *, const gstd_deployment_target *,
                                              const char *stop, const gstd_deployment_workload *,
                                              const json_t *expected);
/* Completed source receipt (normal STOP or original migration cleanup). */
int gstd_migration_recovery_service_receipt_read(sqlite3 *, const gstd_deployment_target *,
                                                 const char *id, const json_t *, const char *boot,
                                                 int native_state_fd);
/* After exact live/stopped helper observation, held death/empty population and
 * successful non-forced DELETE. Record unknown exit status honestly; no helper
 * success, mutation retry or original outcome rewrite is implied. */
int gstd_migration_recovery_helper_deleted(sqlite3 *, const gstd_deployment_target *,
                                           const char *stop, const gstd_deployment_workload *);
/* Validate an earlier owned deletion with unknown exit status, including its
 * STOP provenance and original native instance absence. Read-only. */
int gstd_migration_recovery_helper_receipt_read(sqlite3 *, const gstd_deployment_target *,
                                                const char *id, const json_t *, const char *boot,
                                                int native_state_fd);
/* Fence every helper against observed deletion or untouched preparation before
 * signalling the source. Requires current STOP and held adopted resources. */
int gstd_migration_recovery_service_stopping(sqlite3 *, const gstd_deployment_target *,
                                             const char *stop, const gstd_native_context *);
/* After owned source/writer/network cleanup. Preserves the original operation's
 * state/result/error, clears only its resource recovery observation and makes
 * this STOP terminal. All image refs and volume owners remain bound until here. */
int gstd_migration_recovery_finish(sqlite3 *, const gstd_deployment_target *, const char *stop);
#endif
