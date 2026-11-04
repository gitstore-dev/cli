#include "gitstored/config_validate.h"
#include <string.h>
static int has(const json_t *components, const char *name) {
    for (size_t i = 0; i < json_array_size(components); i++) {
        const char *s = json_string_value(json_array_get(components, i));
        if (s && !strcmp(s, name))
            return 1;
    }
    return 0;
}
static int stage(json_t *stages, const char *id, const char *requires) {
    return json_array_append_new(stages, json_pack("{s:s,s:s,s:s}", "stage", id, "requires",
                                                   requires, "status", "not-performed"));
}
json_t *gstd_bootstrap_plan(const json_t *components, const char *datastore) {
    json_t *stages = json_array();
    if (!stages)
        return NULL;
    if (stage(stages, "prepare-owned-network-volumes", "validated-host-assets-ownership"))
        goto fail;
    if (has(components, "api") || has(components, "git") || has(components, "controller")) {
        if (stage(stages, "prepare-api-user-policy-hmac-issuer",
                  "persist-or-read-existing-private-material; issuer is API-only"))
            goto fail;
        if (has(components, "controller") &&
            stage(stages, "prepare-controller-signing-record",
                  "controller-only-record; stable existing private key"))
            goto fail;
        if (!strcmp(datastore, "scylladb") && stage(stages, "prepare-keyspace-exclusive-migration",
                                                    "registered-target; backup; owned-datastore"))
            goto fail;
        if (stage(stages, "launch-git",
                  "prepared-material; lazy catalogue client; retained repository writer") ||
            stage(stages, "launch-api",
                  "git RPC available; schema-compatible; private API issuer") ||
            stage(stages, "observe-api-git-ready", "public-api-ready; authenticated-public-git"))
            goto fail;
    }
    if (has(components, "controller")) {
        if (stage(stages, "enroll-controller-existing-public-key",
                  "api-git-ready; prepared-controller-key; protected-bootstrap-auth") ||
            stage(stages, "launch-controller",
                  "enrolled-api-assigned-identity; private-signing-record") ||
            stage(stages, "observe-controller-ready",
                  "current-generation-credential-and-kind-readiness"))
            goto fail;
    }
    return stages;
fail:
    json_decref(stages);
    return NULL;
}
