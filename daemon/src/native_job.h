#ifndef GSTD_NATIVE_JOB_H
#define GSTD_NATIVE_JOB_H
#include "backend_libcrun.h"
#include <stddef.h>
typedef struct {
    uint32_t status;
    gstd_native_observation observed;
} gstd_native_reply;
typedef struct {
    pid_t worker;
    int descriptor, interrupted;
    int64_t deadline_ms;
    size_t received;
    unsigned char reply[sizeof(gstd_native_reply)];
} gstd_native_job;
/* Internal asynchronous worker plumbing. Parent must be single-threaded, a
 * subreaper, and the sole durable-state owner. Reserve/commit the resource before
 * launch, outside transactions. No public method or arbitrary command entry.
 * job initially zero (descriptor ignored until worker >0). Borrowed role FDs
 * are copied into the child; all unrelated descriptors are closed there.
 * Ordinary dumpability is required for rootless mapping/procfs; never change a
 * hardened parent's setting automatically. CORE=0 prevents core-file emission.
 * Instrumented ASan builds omit address-space limits, retaining other bounds. */
int gstd_native_launch(const gstd_native_context *, gstd_native_action, unsigned timeout_ms,
                       gstd_native_job *);
/* Nonblocking: 0=pending, 1=worker reaped with result, -1=invalid call. A readable
 * cancellation descriptor is not consumed. Cancellation/deadline/missing reply
 * is UNCERTAIN and never proof that the actual native workload terminated. Keep
 * its ownership reservation and reconcile it separately. */
int gstd_native_poll(gstd_native_job *, int cancellation_fd, gstd_native_status *,
                     gstd_native_observation *);
#endif
