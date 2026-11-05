#ifndef GSTD_HELPER_H
#define GSTD_HELPER_H
#include "execution_plan.h"
#include "native_exit.h"
#include "native_job.h"
#include "native_population.h"
typedef struct {
    gstd_native_context context;
    gstd_native_job worker;
    gstd_native_observation observed;
    gstd_native_population population;
    gstd_native_exit exit;
    int death_fd, stage, outcome, may_exist, deleted;
    int64_t deadline_ms;
} gstd_helper;
/* One registered image-native helper, never an arbitrary command or service.
 * Caller is the sole DB owner/subreaper and must commit its own scope/resource
 * reservation and mutation-dispatch policy before this call. Context inputs,
 * strings, JSON and FDs are borrowed for the actor's lifetime. Core helpers
 * accept no writer or writable data/configuration mount. Fixed OIDC SQLite
 * migrations require the provider's already reserved data/writer descriptors;
 * the parent retains that lease through observed helper termination/deletion.
 * Cancellation/deadlines never signal a running database mutator. */
int gstd_helper_begin(const gstd_native_context *, gstd_helper *);
/* Pure registered-plan validation, before committing the dispatch journal. */
int gstd_helper_validate(const gstd_native_context *);
/* 0=pending, 1=created/awaiting durable identity commit, 2=finished, -1=invalid.
 * At 1, persist context.expected_pid/start_time plus boot/scope before permitting
 * START. Repeated polling at this boundary never dispatches the program. */
int gstd_helper_poll(gstd_helper *, int cancellation_fd);
int gstd_helper_permit_start(gstd_helper *);
/* Withhold START after a failed identity commit. No signal or deletion; the
 * paused native resource and its durable owner remain for reconciliation. */
int gstd_helper_withhold_start(gstd_helper *);
/* Read-only kernel-bound observation at the pre-START commit boundary. */
int gstd_helper_identity_ready(const gstd_helper *);
/* Finished outcome 0 requires normal exit(0), held pidfd death, empty captured
 * cgroup and successful non-forced DELETE. -1=known failure, -2=uncertain. A
 * deadline/cancellation never signals a native helper or releases its owner.
 * may_exist/deleted remain separate from outcome for resource reconciliation.
 * Closing only drops local observations after the host worker was reaped. */
int gstd_helper_close(gstd_helper *);
/* Drop only observer handles after its host worker is reaped, even if a native
 * helper may remain. Never signal/delete or change a durable outcome. */
int gstd_helper_observers_close(gstd_helper *);
#endif
