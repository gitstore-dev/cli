#ifndef GSTD_BOOTSTRAP_H
#define GSTD_BOOTSTRAP_H
#include "gitstored/credentials.h"
#include <jansson.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    const json_t *manifest;
    const char *store_id, *deployment_id, *operation_id, *datastore;
    uint64_t generation;
    pid_t control_owner;
    int scope_fd;
} gstd_bootstrap_request;
typedef enum {
    GSTD_BOOT_PREPARE,
    GSTD_BOOT_LAUNCH_GIT,
    GSTD_BOOT_LAUNCH_API,
    GSTD_BOOT_WAIT_API,
    GSTD_BOOT_WAIT_GIT,
    GSTD_BOOT_ENROLL,
    GSTD_BOOT_BIND_CONTROLLER,
    GSTD_BOOT_LAUNCH_CONTROLLER,
    GSTD_BOOT_WAIT_CONTROLLER,
    GSTD_BOOT_COMPLETE
} gstd_bootstrap_stage;
typedef struct {
    uint64_t generation;
    gstd_bootstrap_stage stage;
    unsigned controller;
    int outcome; /* 0=in progress/complete, -1=rejected, -2=uncertain */
    char controller_uid[129];
} gstd_bootstrap_graph;
/* Pure generation-bound core graph. Parent persists stage/known UID before
 * dispatching the next effect. result: 0=verified success, 1=pending (health
 * reads only), -1=known failure, -2=uncertain. Only enrollment may supply a UID,
 * including partial success. Missing mutation replies never authorize retry. */
int gstd_bootstrap_graph_begin(const json_t *, const char *datastore, uint64_t generation,
                               gstd_bootstrap_graph *);
int gstd_bootstrap_graph_observe(gstd_bootstrap_graph *, uint64_t generation, gstd_bootstrap_stage,
                                 int result, const char *uid);
/* Private worker, after parent intent and credential persistence. Scope must be
 * an owned configuration resource. Exact existing files are reused; partial,
 * changed, linked or foreign material is retained and rejected, never replaced.
 * Only component-specific private input files are written, with no diagnostics
 * or database/plan payload containing credentials. memdb core only initially. */
int gstd_bootstrap_prepare(const gstd_bootstrap_request *, const gstd_credential *);
/* Same private worker boundary. Null source selects OS store; protected-file
 * source is an explicit owner setting, never an implicit fallback. allow_initial
 * is granted only for a new accepted credential reservation, never a restart.
 * Read existing first, persist add-only, then prepare files. Broker is a verified
 * installed gs ELF FD; it receives sensitive creation data only over private IPC. */
int gstd_bootstrap_provision(const gstd_bootstrap_request *, int broker_fd, const json_t *source,
                             int allow_initial);
/* Parent must first persist the public enrollment result for this exact API
 * instance. A fresh host worker verifies the prepared scope and current kernel
 * identity, then writes the immutable controller overlay. Never overwrites a
 * prior UID or adopts another API instance, including after memdb restarts. */
int gstd_bootstrap_controller_bind(const gstd_bootstrap_request *, const gstd_credential *,
                                   const char *uid, pid_t api_pid, uint64_t api_start,
                                   const char *api_boot);
#endif
