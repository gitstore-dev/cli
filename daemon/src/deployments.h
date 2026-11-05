#ifndef GSTD_DEPLOYMENTS_H
#define GSTD_DEPLOYMENTS_H
#include "bootstrap_job.h"
#include "datastore.h"
#include "deployment_assets.h"
#include "deployment_types.h"
#include "execution_plan.h"
#include "log_capture.h"
#include "native_job.h"
#include "native_population.h"
#include "network_worker.h"
#include "runtime_settings.h"
typedef struct {
    const gstd_deployment_environment *environment;
    gstd_deployment_target target;
    char configuration_id[65], network_id[65], volume_id[65], producer_id[65], boot[37];
    gstd_filesystem_reply configuration, network_scope;
    gstd_volume_handle volume;
    gstd_network_worker network;
    gstd_network_worker_observation network_observed;
    gstd_deployment_workload workloads[3]; /* Git, API, controller */
    gstd_datastore datastore;
    int persistent, helper_output[2];
    unsigned count, index;
    int stage, outcome, detail, allow_initial, resolver_fd, cancel_requested;
    int finalizing, final_outcome, final_detail, final_host_failure;
    int recovered, recovery_proven, keeper_death_fd;
    /* Current desired snapshot after recovery or an attempted stop; target
     * retains the original immutable resource-producing start generation. */
    uint64_t recovery_generation;
    char recovery_operation[65];
    int64_t ready_at_ms;
    struct {
        int initialized, running, has_reply, status, index, reported_ready, observation_failed;
        int exit_observed;
        int64_t next_due, last_ready[3];
    } monitor;
    int64_t deadline_ms, next_health_ms;
    gstd_bootstrap_graph graph;
    json_t *credential_source;
    gstd_filesystem_job filesystem;
    gstd_log_capture capture;
    gstd_deployment_asset asset;
    gstd_native_job native_job;
    gstd_bootstrap_job bootstrap;
} gstd_deployment;
/* Internal owner actor, not a wire-command backend. Accepted current start and
 * global roots must be committed/verified first. Environment is borrowed for
 * the entire deployment lifetime. Single thread, sole DB owner/subreaper. Actor
 * initially zero. No lifecycle capability is implied by this API. */
int gstd_deployment_begin(const gstd_deployment_environment *, const char *operation,
                          gstd_deployment *);
/* Read-only recovery preparation from the original unreleased start identity.
 * Does not create or adopt resources or grant native execution. A restoration
 * owner must prove every kernel/filesystem receipt before using this actor. */
int gstd_deployment_recovery_prepare(const gstd_deployment_environment *, const char *operation,
                                     gstd_deployment *);
/* Drop only this parent's observer capabilities after all short jobs are reaped.
 * Never signal, unpin, delete, release durable owners or claim stop completion.
 * Live native resources remain recorded and require subsequent reconciliation. */
int gstd_deployment_observers_close(gstd_deployment *);
/* 0=starting, 1=start finished (outcome 0=ready,-1=failed,-2=uncertain),-1=bad call.
 * Keep actor and capabilities after ready/failure for owned stop/reconciliation.
 * Never dispose a live actor or release references merely on a terminal result. */
int gstd_deployment_poll(gstd_deployment *, int cancellation_fd);
#endif
