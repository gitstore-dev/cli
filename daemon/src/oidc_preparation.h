#ifndef GSTD_OIDC_PREPARATION_H
#define GSTD_OIDC_PREPARATION_H
#include "bootstrap.h"
#include "oidc_config.h"
/* Private bounded worker only, after committed configuration and add-only
 * credential reservations. The configuration scope is a dedicated OIDC resource,
 * separate from core configuration and provider data. Returns four read-only
 * FDs: Hydra config, Kratos config, identity schema, UI environment. No complete
 * material family is mounted or returned to the control owner. Initially all -1.
 * Existing complete files must match exactly; partial/changed scopes are retained.
 * Missing once-created credentials never authorize replacement or fallback. */
int gstd_oidc_prepare(const gstd_bootstrap_request *, const gstd_oidc_ports *,
                      const gstd_credential *, int files[4]);
/* 0=prepared, -1=rejected/unavailable, -2=unknown add-only persistence outcome.
 * Default OS store; a protected-file source is resolved from explicit OIDC owner policy.
 * initial is a one-shot parent grant, never inferred from a missing credential.
 * An optional verified prior configuration FD privately binds all secret values
 * across retained generations; its fingerprint is never mounted or sent over IPC. */
int gstd_oidc_provision(const gstd_bootstrap_request *, const gstd_oidc_ports *, int broker_fd,
                        const json_t *source, int allow_initial, int prior_configuration_fd,
                        int files[4]);
#endif
