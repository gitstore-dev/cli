#ifndef GSTD_DATASTORE_H
#define GSTD_DATASTORE_H
#include "datastore_start.h"
#include "deployment_assets.h"
#include "deployment_types.h"
#include "helper_owner.h"
#include "migration_datastore.h"
typedef struct {
    const gstd_deployment_environment *environment;
    const gstd_deployment_target *target;
    char configuration_id[65], volume_id[65], producer_id[65];
    gstd_datastore_start allocation;
    gstd_migration_datastore migration_allocation;
    gstd_volume_handle volume;
    gstd_bootstrap_graph graph;
    /* Bounded materializations: one service, one reusable CQL helper and one
     * schema helper. Readiness polling never copies another Scylla rootfs. */
    gstd_deployment_workload workloads[3];
    gstd_filesystem_job filesystem;
    gstd_deployment_asset asset;
    gstd_helper_owner helper;
    unsigned helper_index;
    gstd_native_job service_job;
    char service_boot[37];
    int service_stage, service_outcome;
    unsigned index;
    int stage, outcome, detail;
} gstd_datastore;
/* Private persistent preparation, after the configuration scope is recorded.
 * Borrow environment/target for the full lifetime. Own canonical volume and
 * per-purpose execution scopes before any native service/helper dispatch.
 * This actor does not enable a public profile or access bootstrap credentials. */
int gstd_datastore_prepare_begin(const gstd_deployment_environment *,
                                 const gstd_deployment_target *, const char *configuration_id,
                                 gstd_datastore *);
/* Metadata/empty observer initialization only; no allocation, lease or native
 * execution. Restoration must prove every receipt before committing adoption. */
int gstd_datastore_recovery_seed(const gstd_deployment_environment *,
                                 const gstd_deployment_target *, const char *configuration_id,
                                 gstd_datastore *);
int gstd_datastore_service_recovered(gstd_datastore *, const char *boot_id);
/* 0=pending,1=finished,-1=invalid. outcome0 exposes held scopes/image/volume
 * handles, -1=known failure,-2=uncertain. Caller retains all durable owners. */
int gstd_datastore_prepare_poll(gstd_datastore *, int cancellation_fd);
/* Commit an observed bootstrap/helper result against the exact prepared graph.
 * Positive helper results require its terminal durable deletion receipt. */
int gstd_datastore_graph_step(gstd_datastore *, gstd_bootstrap_stage, int result);
/* After verified bootstrap preparation advanced the graph to LAUNCH_DATASTORE.
 * Starts only the registered service on its canonical writer/data capabilities;
 * the parent must first commit its exact owned network attachment. */
int gstd_datastore_service_begin(gstd_datastore *, const gstd_network_namespace *,
                                 const char *boot_id, int resolver_fd, int stdout_fd,
                                 int stderr_fd);
/* 0=pending,1=finished,-1=invalid. service_outcome=0 means running, not CQL-ready.
 * Keeps all native observers/scopes after completion or uncertainty for stop and
 * reconciliation. Parent writer FD closes only after verified running commit. */
int gstd_datastore_service_poll(gstd_datastore *, int cancellation_fd);
/* Read-only held pidfd/cgroup observation; 0=matching live owned service. */
int gstd_datastore_service_live(const gstd_datastore *);
/* After the owner's verified non-forced DELETE/termination-receipt commit.
 * Requires held death/cgroup proof (or the reopened exact stopped population) and no helper. Closes
 * only service metadata; volume/scopes and durable owners remain until the normal stop
 * finalization. */
int gstd_datastore_service_retire(gstd_datastore *);
/* Private startup helpers from the prepared scopes. The running datastore and
 * exact helper namespace attachment must already be committed by the parent.
 * The journal derives allowed purpose/order from its current persistent graph.
 * FDs/network are borrowed through helper_close; no caller command is accepted. */
int gstd_datastore_helper_begin(gstd_datastore *, gstd_execution_purpose,
                                const gstd_network_namespace *, const char *boot_id,
                                int resolver_fd, int stdout_fd, int stderr_fd);
int gstd_datastore_helper_poll(gstd_datastore *, int cancellation_fd);
/* Only after terminal helper outcome; preserves its journal and all scopes. */
int gstd_datastore_helper_close(gstd_datastore *);
/* Preparation finished and workers reaped only. Closes parent capabilities;
 * never releases durable owners, deletes volumes or unpins cached images. */
int gstd_datastore_observers_close(gstd_datastore *);
#endif
