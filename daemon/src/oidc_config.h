#ifndef GSTD_OIDC_CONFIG_H
#define GSTD_OIDC_CONFIG_H
#include "gitstored/credentials.h"
#include <jansson.h>
#include <stdint.h>
typedef struct gstd_oidc_ports {
    uint16_t issuer, bridge, identity, ui, mail;
} gstd_oidc_ports;
typedef enum { GSTD_OIDC_HYDRA, GSTD_OIDC_KRATOS } gstd_oidc_provider;
/* Fixed single-node local reference mapping. Public ports are equal inside and
 * outside the owned namespace so issuer/browser redirects remain canonical.
 * Admin APIs and SMTP are private; fixed core/private ports cannot be selected.
 * These functions are preparation policy, never execution/ownership authority. */
int gstd_oidc_ports_check(const gstd_oidc_ports *);
/* Private worker only: returned JSON contains secrets. Never transfer to parent
 * IPC or operation records. Caller clears all sensitive values before release.
 * Readiness/enrollment/retained writer ownership are separate prerequisites. */
json_t *gstd_oidc_provider_config(gstd_oidc_provider, const gstd_oidc_ports *,
                                  const gstd_credential *);
/* Wipe all string values; caller then decrefs its private root. Never use on
 * shared documents, profile constants or public parent-owned JSON. */
void gstd_oidc_config_clear(json_t *);
/* Private worker output: fixed image-native shell assignments with only
 * validated numeric ports and generated hex secrets. Clear after protected write. */
int gstd_oidc_ui_environment(const gstd_oidc_ports *, const gstd_credential *, gstd_credential *);
#endif
