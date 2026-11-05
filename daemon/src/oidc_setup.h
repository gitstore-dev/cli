#ifndef GSTD_OIDC_SETUP_H
#define GSTD_OIDC_SETUP_H
#include "deployment_records.h"
#include "deployment_types.h"
typedef struct {
    const gstd_deployment_environment *environment;
    const gstd_deployment_target *target;
    gstd_oidc_ports ports;
    char configuration_id[65], previous_id[65], credential_reference[65];
    char volume_ids[2][65], producers[2][65];
    json_t *source, *previous_identity;
    gstd_filesystem_reply configuration, previous;
    int files[4], initial;
    gstd_volume_handle volumes[2];
    int fresh[2]; /* captured before allocation; not migration/execution authority */
    gstd_volume_identity expected;
    unsigned index;
    int stage, outcome, detail;
    gstd_filesystem_job worker;
} gstd_oidc_setup;
/* Internal parent pipeline, not a public lifecycle capability. A current accepted
 * core/2 OIDC start, verified lifetime roots and reserved fixed ports are caller
 * prerequisites. Target/environment are borrowed for the entire actor lifetime.
 * Commits scope/material/source intent before private workers and commits public
 * file/volume receipts afterward. The parent never reads secret bytes.
 * Initialize to zero. No caller transaction may be open. */
int gstd_oidc_setup_begin(const gstd_deployment_environment *, const gstd_deployment_target *,
                          const gstd_oidc_ports *, gstd_oidc_setup *);
/* 0=pending, 1=preparation finished, -1=bad call. outcome 0 provides four protected
 * config FDs and two separately leased retained provider volumes; it does NOT
 * mean migration, enrollment, issuer or service readiness. Negative outcomes
 * retain metadata/files; cancellation never grants regeneration or rollback. */
int gstd_oidc_setup_poll(gstd_oidc_setup *, int cancellation_fd);
/* Reaped-worker observer close only. No deletion, durable release or native
 * termination is inferred. Native consumers must retain their own capabilities
 * until observed shutdown; environment lifetime owners are never closed here. */
int gstd_oidc_setup_close(gstd_oidc_setup *);
#endif
