#ifndef GSTD_WORKLOAD_ROLES_H
#define GSTD_WORKLOAD_ROLES_H
#include "bootstrap_http.h"
#include <jansson.h>
/* Fixed service identities, in dependency/start order. Core services follow the
 * providers, so reversing this order stops API consumers before their issuers.
 * Datastore and migration helpers retain separate lifecycle machinery. */
typedef enum {
    GSTD_ROLE_SMTP,
    GSTD_ROLE_HYDRA,
    GSTD_ROLE_KRATOS,
    GSTD_ROLE_IDENTITY_UI,
    GSTD_ROLE_BRIDGE,
    GSTD_ROLE_GIT,
    GSTD_ROLE_API,
    GSTD_ROLE_CONTROLLER,
    GSTD_ROLE_COUNT
} gstd_workload_role;
#define GSTD_WORKLOAD_MAX ((unsigned)GSTD_ROLE_COUNT)
const char *gstd_workload_name(gstd_workload_role);
/* Exact registered manifest only. No allocation, provisioning or execution.
 * Names/count never come from caller component strings or manifest ordering. */
int gstd_workload_layout(const json_t *manifest, gstd_workload_role roles[GSTD_ROLE_COUNT],
                           unsigned *count);
int gstd_workload_index(const gstd_workload_role *, unsigned count, gstd_workload_role);
/* -1 for core/unknown roles; private health consumers for the five providers. */
int gstd_workload_oidc_health(gstd_workload_role);
/* -1 for non-writers, 0 for Hydra, 1 for Kratos; Git is a separate volume role. */
int gstd_workload_oidc_volume(gstd_workload_role);
#endif
