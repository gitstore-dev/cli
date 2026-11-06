#ifndef GSTD_OIDC_RUNTIME_H
#define GSTD_OIDC_RUNTIME_H
#include "bootstrap_job.h"
#include "deployment_assets.h"
#include "helper_owner.h"
#include "oidc_setup.h"
#include "workload_roles.h"
#define GSTD_OIDC_SERVICE_COUNT 5u
#define GSTD_OIDC_WORKLOAD_COUNT 7u
typedef struct {
    const gstd_deployment_environment *environment;
    const gstd_deployment_target *target;
    gstd_oidc_setup setup;
    /* SMTP/Hydra/Kratos/UI/bridge, then the two distinct migration scopes. */
    gstd_deployment_workload workloads[GSTD_OIDC_WORKLOAD_COUNT];
    gstd_filesystem_job filesystem;
    gstd_deployment_asset asset;
    gstd_helper_owner migration;
    gstd_native_job native_job;
    gstd_bootstrap_job http;
    const gstd_network_namespace *network;
    const gstd_network_worker *network_owner;
    int resolver_fd, output[GSTD_OIDC_SERVICE_COUNT][2];
    char boot[37], client_id[65], redirect_uri[128];
    gstd_oidc_client client;
    json_t *enrollment;
    unsigned index, migrated, started;
    int stage, outcome, detail, cancel_requested, execute_stage, execute_outcome;
    int enrollment_action, sweeping, final_check, final_verified;
    int64_t deadline_ms, next_health_ms;
} gstd_oidc_runtime;
/* Parent-owned production coordinator. Current accepted core/2 target and fixed
 * port selection must already be committed. Lifetime owners/target are borrowed.
 * Preparation reserves before private effects and retains every scope, cache
 * reference, configuration and provider writer on failure. No native execution. */
int gstd_oidc_runtime_prepare_begin(const gstd_deployment_environment *,
                                    const gstd_deployment_target *, const gstd_oidc_ports *,
                                    gstd_oidc_runtime *);
int gstd_oidc_runtime_prepare_poll(gstd_oidc_runtime *, int cancellation_fd);
/* Read-only recovery seed: fixed resource names and immutable canonical volume
 * metadata only. No ports, credentials, writer lease or execution permission. */
int gstd_oidc_runtime_recovery_seed(const gstd_deployment_environment *,
                                    const gstd_deployment_target *, gstd_oidc_runtime *);
/* After preparation, commit attachments for all seven scopes before this call.
 * Output FDs are borrowed through completion; caller closes each only after its
 * service CREATE has returned. Providers retain their parent's canonical leases
 * through stop; the native init must independently carry the same writer FD 3.
 * API integration and cleanup/recovery are handled by the owning deployment. */
int gstd_oidc_runtime_execute_begin(gstd_oidc_runtime *, const gstd_network_namespace *,
                                    const gstd_network_worker *network_owner, const char *boot_id,
                                    int resolver_fd, const int output[GSTD_OIDC_SERVICE_COUNT][2]);
/* 0=pending,1=finished,-1=invalid. execute_outcome 0 requires current native,
 * cgroup, namespace, provider writer, enrollment and five fresh health reads.
 * Lost mutating replies remain uncertain; no automatic POST/helper replay. */
int gstd_oidc_runtime_execute_poll(gstd_oidc_runtime *, int cancellation_fd);
/* Read-only fresh sweep after core startup; never repeats migrations/enrollment.
 * The entire final sweep and readiness commit must fit in its 30-second window. */
int gstd_oidc_runtime_final_health_begin(gstd_oidc_runtime *);
int gstd_oidc_runtime_service_live(const gstd_oidc_runtime *, unsigned service);
/* Reaped-worker observer close only. Never signal/delete or release a ledger.
 * Callers must retain resources until separately observed owned termination. */
int gstd_oidc_runtime_observers_close(gstd_oidc_runtime *);
#endif
