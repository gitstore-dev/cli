#ifndef GSTD_TERMINATION_H
#define GSTD_TERMINATION_H
#include "deployments.h"
/* Caller transaction, after successful non-forced libcrun deletion. The held
 * pidfd and cgroup must still prove exit/emptiness, or restoration must have
 * proved the exact stopped runtime tuple and reopened recorded empty cgroup. Atomically record the
 * receipt and retained stage; no resource/lease release occurs here. */
int gstd_termination_record(gstd_deployment *, gstd_deployment_workload *,
                            const char *stop_operation, uint64_t desired_generation);
/* Read-only: validate a previously committed receipt from its source resource,
 * native identity and still-retained stop history. No missing PID/path is proof. */
int gstd_termination_read(sqlite3 *, const gstd_deployment_target *, const char *resource,
                          const json_t *backend_identity);
#endif
