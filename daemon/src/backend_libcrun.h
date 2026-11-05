#ifndef GITSTORED_BACKEND_LIBCRUN_H
#define GITSTORED_BACKEND_LIBCRUN_H
#include <jansson.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct gstd_execution_inputs gstd_execution_inputs;
/* Private native worker boundary, never protocol parameters. The owner must
 * generate configuration from a registered profile, commit resource ownership,
 * and dispatch in a bounded, threadless worker with unrelated descriptors closed.
 * This interface does not launch a worker, authorize mounts or write SQLite.
 * Production dispatch/profile construction remain separate integration work. */
typedef enum {
    GSTD_NATIVE_CREATE,
    GSTD_NATIVE_START,
    GSTD_NATIVE_OBSERVE,
    GSTD_NATIVE_TERM,
    GSTD_NATIVE_KILL,
    GSTD_NATIVE_DELETE
} gstd_native_action;
typedef enum {
    GSTD_NATIVE_OK,
    GSTD_NATIVE_INVALID,
    GSTD_NATIVE_UNAVAILABLE,
    GSTD_NATIVE_HOST_UNMET,
    GSTD_NATIVE_CONFLICT,
    GSTD_NATIVE_FAILED,
    GSTD_NATIVE_UNCERTAIN
} gstd_native_status;
typedef struct {
    const char *backend_id;
    const char *component;
    const char *boot_id; /* creator boot; never substitute current during recovery */
    int state_fd, bundle_fd, rootfs_fd;
    int stdout_fd, stderr_fd; /* optional private collector pipes; never wire input */
    int writer_lease_fd;      /* CREATE: Git/Scylla service writer preserved at FD 3 */
    pid_t control_owner;
    pid_t expected_pid;
    uint64_t expected_start_time;
    const json_t *configuration;
    const gstd_execution_inputs *registered_inputs;
    int test_fixture; /* recognized only in explicitly built test facilities */
} gstd_native_context;
typedef struct {
    pid_t pid;
    uint64_t start_time;
    unsigned created, running, stopped, effect_possible;
    int native_error;
} gstd_native_observation;
int gstd_native_backend_available(void);
/* Worker must have CORE=0, finite AS soft limit <=1GiB and FSIZE <=8GiB.
 * Container RLIMIT_AS may raise its soft limit within the inherited hard bound;
 * cgroups enforce physical memory. No force-no-cgroup or force-delete fallback.
 * Unknown worker outcomes are never proof of workload termination/rollback.
 * Only the control owner persists observations and decides retries/cleanup. */
/* stopped is libcrun's init-process state, not proof that every descendant left
 * its cgroup or that a repository writer lease may be released. */
gstd_native_status gstd_native_apply(const gstd_native_context *context, gstd_native_action action,
                                     gstd_native_observation *observed);
#endif
