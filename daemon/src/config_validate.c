#include "gitstored/config_validate.h"
#include "oidc_profile.h"
#include <stdio.h>
#include <string.h>
/* Removed/renamed owning-service keys at the pinned revision. Native validators
 * remain authoritative for full TOML/environment semantics and policy checks. */
static const char *removed[] = {"grpc.port",
                                "git.data_dir",
                                "git.repo",
                                "hooks.git_receive_pack",
                                "schema_validation",
                                "admission_control",
                                "catalog_service",
                                "auth",
                                "git.grpc",
                                "datastore",
                                "api.rate_limit_per_second",
                                "api.rate_limit_burst",
                                "watch.namespace",
                                "features.namespace_repository_fence",
                                "git_service.metrics",
                                "controller.api_client.requests_per_second",
                                "controller.api_client.burst",
                                "controller.api_token",
                                "controller.serviceaccount.namespace",
                                "controller.serviceaccount.key_id",
                                "controller.serviceaccount.key_ref.key",
                                "controller.secret_providers.bootstrap.format",
                                "controller.secret_providers.bootstrap.env_prefix"};
static int rejected(const json_t *tree, const char *prefix, unsigned depth) {
    if (depth > 32)
        return 1;
    const char *key;
    json_t *value;
    json_object_foreach((json_t *)tree, key, value) {
        char path[512];
        int n = snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "." : "", key);
        if (n < 0 || (size_t)n >= sizeof(path))
            return 1;
        for (size_t i = 0; i < sizeof(removed) / sizeof(*removed); i++) {
            size_t length = strlen(removed[i]);
            if (!strncmp(path, removed[i], length) && (!path[length] || path[length] == '.'))
                return 1;
        }
        /* Private profiling is deliberately not exposed by this profile. */
        if (!strcmp(path, "api.profiling") || !strcmp(path, "controller.profiling"))
            return 1;
        if (json_is_object(value) && rejected(value, path, depth + 1))
            return 1;
    }
    return 0;
}
json_t *gstd_config_merge(const char *component, const json_t *layers) {
    if (!component ||
        (strcmp(component, "api") && strcmp(component, "git") && strcmp(component, "controller")) ||
        !json_is_array(layers) || !json_array_size(layers) || json_array_size(layers) > 16 ||
        (!strcmp(component, "git") && json_array_size(layers) != 1))
        return NULL;
    json_t *merged = json_object();
    if (!merged)
        return NULL;
    for (size_t i = 0; i < json_array_size(layers); i++) {
        json_t *layer = json_array_get(layers, i);
        if (!json_is_object(layer) || rejected(layer, "", 0) ||
            json_object_update_recursive(merged, layer)) {
            json_decref(merged);
            return NULL;
        }
    }
    return merged;
}
json_t *gstd_config_plan(const json_t *profile, const json_t *components, const char *datastore) {
    json_t *plans = json_array();
    if (!plans)
        return NULL;
    for (size_t i = 0; i < json_array_size(components); i++) {
        const char *name = json_string_value(json_array_get(components, i));
        if (name && !strcmp(name, "oidc")) {
            const char *roles[] = {"oidc-smtp", "oidc-hydra", "oidc-kratos", "oidc-ui",
                                   "oidc-bridge"};
            for (unsigned j = 0; j < 5; j++) {
                json_t *entry = json_pack("{s:s,s:s,s:s}", "component", roles[j], "sourceStatus",
                                          "planned-owned-protected-configuration",
                                          "nativeValidation", "unperformed");
                if (!entry || json_array_append_new(plans, entry)) {
                    json_decref(plans);
                    return NULL;
                }
            }
            continue;
        }
        json_t *definition = json_object_get(json_object_get(profile, "config"), name);
        json_t *merged = gstd_config_merge(name, json_object_get(definition, "layers"));
        if (!merged) {
            json_decref(plans);
            return NULL;
        }
        /* These are non-secret plan fields only. File content/native validation
         * is unperformed until the owned image/runtime mechanism is available. */
        json_t *plan = json_pack("{s:s,s:O,s:O,s:s,s:s}", "component", name, "sources",
                                 json_object_get(definition, "files"), "layering",
                                 json_object_get(definition, "layering"), "sourceStatus",
                                 "planned-owned-files", "nativeValidation", "unperformed");
        json_t *validator = json_object_get(definition, "nativeValidator");
        if (plan && json_is_object(validator) &&
            json_object_set(plan, "nativeValidator", validator)) {
            json_decref(plan);
            plan = NULL;
        }
        if (plan && !strcmp(name, "api"))
            json_object_set_new(plan, "datastoreBackend",
                                json_string(!strcmp(datastore, "scylladb") ? "scylla" : "memdb"));
        json_decref(merged);
        if (!plan || json_array_append_new(plans, plan)) {
            json_decref(plans);
            return NULL;
        }
    }
    return plans;
}
