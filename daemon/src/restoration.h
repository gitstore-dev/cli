#ifndef GSTD_RESTORATION_H
#define GSTD_RESTORATION_H
#include "deployments.h"
typedef struct {
    gstd_deployment *deployment;
    int stage, index, outcome;
    int64_t deadline_ms;
    json_t *expected;
    gstd_filesystem_job filesystem;
    gstd_native_job native;
} gstd_restoration;
/* Recover only exact, still-observable core resources of an unreleased start.
 * No create/start, credential access, readiness claim or workload signal. Failed
 * or incomplete proof retains uncertainty. Accepted stop is a separate action. */
int gstd_restoration_begin(const gstd_deployment_environment *, const char *original_start,
                           gstd_deployment *, gstd_restoration *);
/* Nonblocking: 0=pending,1=finished. outcome0=all identities proved,-2=uncertain. */
int gstd_restoration_poll(gstd_restoration *);
/* Close only observer capabilities after workers are reaped; no metadata owner
 * release or workload signal. Use after failed proof, never as stop completion. */
int gstd_restoration_discard(gstd_restoration *);
#endif
