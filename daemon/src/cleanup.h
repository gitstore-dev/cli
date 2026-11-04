#ifndef GSTD_CLEANUP_H
#define GSTD_CLEANUP_H
#include "deployments.h"
typedef struct {
    gstd_deployment *deployment;
    char operation_id[65];
    uint64_t generation;
    int stage, index, outcome, started, uncertain;
    int64_t deadline_ms;
    gstd_native_job native;
    gstd_filesystem_job filesystem;
} gstd_cleanup;
/* Only an actor whose start has finished and whose short-lived workers are
 * reaped. Real stop operation is caller-owned and already accepted/running.
 * NULL operation is private retained cleanup, never permission to release DB
 * ownership or claim public stop completion. Actor initially zero. */
int gstd_cleanup_begin(gstd_deployment *, const char *operation, gstd_cleanup *);
/* Nonblocking 0=pending,1=finished,-1=invalid; outcome0=observed owned stop,
 * -2=uncertain. No volume deletion, broad signalling or forced native delete. */
int gstd_cleanup_poll(gstd_cleanup *);
/* Successful observed cleanup only; closes parent capabilities and actor memory.
 * Retained directories, volumes, journal and image cache stay on disk. */
int gstd_cleanup_dispose(gstd_cleanup *);
#endif
