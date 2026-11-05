#ifndef GSTD_NATIVE_RECOVERY_H
#define GSTD_NATIVE_RECOVERY_H
#include "deployments.h"
typedef enum {
    GSTD_RECOVERY_UNDISPATCHED,
    GSTD_RECOVERY_NATIVE_KNOWN,
    GSTD_RECOVERY_NATIVE_DISCOVER
} gstd_native_recovery_kind;
/* Classify only the native subsection of an already reopened ledger-bound
 * workload scope. UNDISPATCHED requires reserved stage and no native intent.
 * DISCOVER grants read-only observation, never permission to create or signal.
 * Invalid/ambiguous metadata returns -1. */
int gstd_native_recovery_read(const json_t *, const char *stage, const char *resource,
                              const char *component, const char *boot, uint64_t generation,
                              gstd_native_observation *);
/* Record a discovered live init only inside the restoration transaction, after
 * the exact kernel watch/cgroup and original zero-PID ledger record match.
 * No operation terminal rewrite, readiness claim or resource release. */
int gstd_native_recovery_record(gstd_deployment *, gstd_deployment_workload *, const json_t *);
#endif
