#include "workload_roles.h"
#include "gitstored/assets.h"
#include <string.h>
const char *gstd_workload_name(gstd_workload_role role) {
    static const char *const names[] = {"oidc-smtp", "oidc-hydra", "oidc-kratos", "oidc-ui",
                                        "oidc-bridge", "git", "api", "controller"};
    return role >= GSTD_ROLE_SMTP && role < GSTD_ROLE_COUNT ? names[role] : NULL;
}
int gstd_workload_layout(const json_t *manifest, gstd_workload_role roles[GSTD_ROLE_COUNT],
                           unsigned *count) {
    if (!roles || !count || gstd_asset_manifest_check(manifest))
        return -1;
    unsigned selected = 0;
    const json_t *images = json_object_get(manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *name = json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (!name)
            return -1;
        if (!strcmp(name, "scylladb"))
            continue;
        unsigned role;
        for (role = 0; role < GSTD_WORKLOAD_MAX; role++)
            if (!strcmp(name, gstd_workload_name((gstd_workload_role)role)))
                break;
        if (role == GSTD_WORKLOAD_MAX || (selected & (1u << role)))
            return -1;
        selected |= 1u << role;
    }
    unsigned length = 0;
    for (unsigned role = 0; role < GSTD_WORKLOAD_MAX; role++)
        if (selected & (1u << role))
            roles[length++] = (gstd_workload_role)role;
    if (!length)
        return -1;
    *count = length;
    return 0;
}
int gstd_workload_index(const gstd_workload_role *roles, unsigned count, gstd_workload_role role) {
    if (!roles || count > GSTD_WORKLOAD_MAX || !gstd_workload_name(role))
        return -1;
    for (unsigned i = 0; i < count; i++)
        if (roles[i] == role)
            return (int)i;
    return -1;
}
int gstd_workload_oidc_health(gstd_workload_role role) {
    switch (role) {
    case GSTD_ROLE_SMTP: return GSTD_OIDC_HEALTH_SMTP;
    case GSTD_ROLE_HYDRA: return GSTD_OIDC_HEALTH_HYDRA;
    case GSTD_ROLE_KRATOS: return GSTD_OIDC_HEALTH_KRATOS;
    case GSTD_ROLE_IDENTITY_UI: return GSTD_OIDC_HEALTH_UI;
    case GSTD_ROLE_BRIDGE: return GSTD_OIDC_HEALTH_BRIDGE;
    default: return -1;
    }
}
int gstd_workload_oidc_volume(gstd_workload_role role) {
    return role == GSTD_ROLE_HYDRA ? 0 : role == GSTD_ROLE_KRATOS ? 1 : -1;
}
