#ifndef GSTD_BOOTSTRAP_JOB_H
#define GSTD_BOOTSTRAP_JOB_H
#include "bootstrap.h"
#include "bootstrap_http.h"
#include <stddef.h>
typedef enum {
    GSTD_BOOT_JOB_PREPARE,
    GSTD_BOOT_JOB_BIND,
    GSTD_BOOT_JOB_HTTP
} gstd_bootstrap_job_kind;
typedef struct {
    gstd_bootstrap_request preparation;
    gstd_bootstrap_job_kind kind;
    int broker_fd, allow_initial;
    const json_t *source;
    gstd_bootstrap_http_action action;
    const gstd_network_namespace *network;
    const char *uid, *api_boot;
    pid_t api_pid;
    uint64_t api_start;
    unsigned test_pause; /* private test-only pre-effect stop; production rejects */
} gstd_bootstrap_job_input;
typedef struct {
    int32_t status;
    char uid[129];
} gstd_bootstrap_job_reply;
typedef struct {
    pid_t worker;
    int descriptor, interrupted;
    int64_t deadline_ms;
    size_t received;
    unsigned char bytes[sizeof(gstd_bootstrap_job_reply)];
} gstd_bootstrap_job;
/* Threadless parent only; accepted intent/resource/credential reservation must
 * already be committed. Copies private capabilities into an isolated bounded
 * worker. No secret enters the reply, DB, parent heap, arguments or diagnostics.
 * Parent retains resources and dispatches the next graph stage only after its
 * nonblocking poll observes completion and the next receipt is committed. */
int gstd_bootstrap_job_launch(const gstd_bootstrap_job_input *, unsigned timeout_ms,
                              gstd_bootstrap_job *);
/* 0=pending, 1=reaped with reply, -1=invalid. Deadline/cancel/crash/missing reply
 * returns uncertain (-2), never permission to regenerate material or retry an
 * enrollment. Cancellation readiness is not consumed. Only this own worker is
 * killed/reaped; no workload/resource cleanup is inferred. */
int gstd_bootstrap_job_poll(gstd_bootstrap_job *, int cancellation_fd, gstd_bootstrap_job_reply *);
#endif
