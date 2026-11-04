#ifndef GSTD_NATIVE_ABI_H
#define GSTD_NATIVE_ABI_H
#include <stdint.h>
#include <sys/types.h>
/* Keep libcrun/json-c headers outside Jansson translation units. All strings
 * are borrowed for a single call; the ABI never transfers native allocations. */
typedef struct {
    const char *state_root, *id, *bundle;
    int preserve_fds;
} gstd_crun_request;
typedef struct {
    pid_t pid;
    uint64_t start_time;
    unsigned created, running, stopped;
    int error;
} gstd_crun_state;
int gstd_crun_observe(const gstd_crun_request *, const char *, const char *, gstd_crun_state *);
int gstd_crun_create(const gstd_crun_request *, const char *, int *);
int gstd_crun_start(const gstd_crun_request *, int *);
int gstd_crun_signal(const gstd_crun_request *, int, int *);
int gstd_crun_delete(const gstd_crun_request *, int *);
int gstd_crun_host_ready(void);
/* Compile/apply a private worker's fixed OCI seccomp policy using libcrun.
 * The caller has already set no-new-privileges. No cache or path is used. */
int gstd_crun_worker_filter(const char *);
#endif
