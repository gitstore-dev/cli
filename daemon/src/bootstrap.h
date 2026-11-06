#ifndef GSTD_BOOTSTRAP_H
#define GSTD_BOOTSTRAP_H
#include "gitstored/credentials.h"
#include "oidc_client.h"
#include <jansson.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    const json_t *manifest;
    const char *store_id, *deployment_id, *operation_id, *datastore;
    uint64_t generation;
    pid_t control_owner;
    int scope_fd;
    /* Public mapping from the committed provider/enrollment owner; no secret. */
    unsigned oidc_issuer_port;
    const gstd_oidc_client *oidc_client;
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
    GSTD_BOOT_COMPLETE,
    /* Append to preserve the already recorded memdb stage numbers. */
    GSTD_BOOT_LAUNCH_DATASTORE,
    GSTD_BOOT_WAIT_DATASTORE,
    GSTD_BOOT_PREPARE_KEYSPACE,
    GSTD_BOOT_MIGRATE_SCHEMA,
    GSTD_BOOT_STAGE_COUNT
} gstd_bootstrap_stage;
typedef struct {
    uint64_t generation;
    gstd_bootstrap_stage stage;
    unsigned controller, persistent, fresh_datastore;
    int outcome; /* 0=in progress/complete, -1=rejected, -2=uncertain */
    char controller_uid[129];
} gstd_bootstrap_graph;
/* Pure generation-bound core graph. Parent persists stage/known UID before
 * dispatching the next effect. result: 0=verified success, 1=pending (health
 * reads only), -1=known failure, -2=uncertain. Only enrollment may supply a
 * UID, including partial success. Missing mutation replies never authorize
 * retry. */
int gstd_bootstrap_graph_begin(const json_t *, const char *datastore, uint64_t generation,
                               gstd_bootstrap_graph *);
/* Private graph policy only. Fresh is derived from a new canonical datastore
 * allocation receipt, never wire input. Retained data skips schema mutation and
 * requires API check mode; this graph does not grant execution/lease authority.
 */
int gstd_bootstrap_graph_persistent_begin(const json_t *, uint64_t generation, int fresh,
                                          gstd_bootstrap_graph *);
int gstd_bootstrap_graph_observe(gstd_bootstrap_graph *, uint64_t generation, gstd_bootstrap_stage,
                                 int result, const char *uid);
/* Private worker, after parent intent and credential persistence. Scope must be
 * an owned configuration resource. Exact existing files are reused; partial,
 * changed, linked or foreign material is retained and rejected, never replaced.
 * Only component-specific private input files are written, with no diagnostics
 * or database/plan payload containing credentials. memdb core only initially.
 */
int gstd_bootstrap_prepare(const gstd_bootstrap_request *, const gstd_credential *);
/* Same private worker boundary. Null source selects OS store; protected-file
 * source is an explicit owner setting, never an implicit fallback.
 * allow_initial is granted only for a new accepted credential reservation,
 * never a restart. Read existing first, persist add-only, then prepare files.
 * Broker is a verified installed gs ELF FD; it receives sensitive creation data
 * only over private IPC. */
int gstd_bootstrap_provision(const gstd_bootstrap_request *, int broker_fd, const json_t *source,
                             int allow_initial);
/* Parent first persists a new public enrollment result or a prior successful
 * enrollment on the same canonical retained datastore. A fresh worker verifies
 * the prepared scope and current API kernel
 * identity, then writes the immutable controller overlay. Never overwrites a
 * prior UID or adopts another API instance, including after memdb restarts. */
int gstd_bootstrap_controller_bind(const gstd_bootstrap_request *, const gstd_credential *,
                                   const char *uid, pid_t api_pid, uint64_t api_start,
                                   const char *api_boot);
#endif
