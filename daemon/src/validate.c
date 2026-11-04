#define _POSIX_C_SOURCE 200809L
#include "gitstored/validate.h"
#include "backend_libcrun.h"
#include "gitstored/assets.h"
#include "gitstored/config_validate.h"
#include "gitstored/dependencies.h"
#include "gitstored/profiles.h"
#include "gitstored/protocol.h"
#include "image_backend.h"
#include "networking.h"
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
static int diagnostic(json_t *result, const char *code, const char *message) {
    return json_array_append_new(
        json_object_get(result, "diagnostics"),
        json_pack("{s:s,s:s,s:s}", "code", code, "status", "unmet", "message", message));
}
static json_t *empty(void) {
    return json_pack("{s:b,s:{},s:[],s:[],s:[]}", "valid", 0, "effectivePlan", "requiredAssets",
                     "hostRequirements", "diagnostics");
}
static int same(const json_t *value, const char *s) {
    return json_is_string(value) && json_string_length(value) == strlen(s) &&
           !memcmp(json_string_value(value), s, strlen(s));
}
static int selected(const json_t *components, const char *name) {
    for (size_t i = 0; i < json_array_size(components); i++)
        if (same(json_array_get(components, i), name))
            return 1;
    return 0;
}
json_t *gstd_validate(json_t *params) {
    json_t *result = empty(), *profile = NULL, *components = NULL, *envelope = NULL;
    if (!result)
        return NULL;
    envelope =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:O}", "version", "major", 1, "minor", 0, "requestId",
                  "validate-internal", "method", "deployment.validate", "params", params);
    gstd_request request;
    if (!envelope || gstd_request_parse(envelope, &request)) {
        if (diagnostic(result, "INVALID_ARGUMENT", "Unknown or invalid profile parameters"))
            goto fail;
        goto done;
    }
    profile = gstd_profile();
    if (!profile)
        goto fail;
    if (!json_equal(json_object_get(params, "profileId"), json_object_get(profile, "profileId")) ||
        !json_equal(json_object_get(params, "profileVersion"),
                    json_object_get(profile, "profileVersion"))) {
        if (diagnostic(result, "PROFILE_NOT_FOUND", "Select the installed core profile version 1"))
            goto fail;
        goto done;
    }
    json_t *requested = json_object_get(params, "components");
    components = gstd_profile_components(requested);
    if (!components) {
        if (diagnostic(result,
                       requested && json_is_array(requested) && !json_array_size(requested)
                           ? "INVALID_ARGUMENT"
                           : "COMPONENT_UNAVAILABLE",
                       "Select a nonempty unique set of verified core components"))
            goto fail;
        goto done;
    }
    const char *datastore = json_string_value(json_object_get(params, "datastore"));
    if (!datastore)
        datastore = "memdb";
    json_t *plan = json_object_get(result, "effectivePlan");
    if (json_object_set(plan, "profileId", json_object_get(profile, "profileId")) ||
        json_object_set(plan, "profileVersion", json_object_get(profile, "profileVersion")) ||
        json_object_set_new(plan, "manifestDigest", json_string(gstd_profile_digest())) ||
        json_object_set(plan, "serverRevision", json_object_get(profile, "serverRevision")) ||
        json_object_set(plan, "components", components) ||
        json_object_set_new(plan, "datastore", json_string(datastore)) ||
        json_object_set_new(plan, "allowProvisionEffective", json_false()) ||
        json_object_set_new(plan, "configuration",
                            gstd_config_plan(profile, components, datastore)) ||
        json_object_set_new(plan, "bootstrapStages", gstd_bootstrap_plan(components, datastore)))
        goto fail;
    json_t *requirements = json_object_get(result, "hostRequirements");
    const char *names[] = {"linux",
                           "user-namespaces",
                           "uid-gid-mapping",
                           "delegated-cgroup-v2",
                           "mount-support",
                           "private-rootless-network",
                           "loopback-high-ports",
                           "openat2-no-mount-crossing",
                           "atomic-no-replace-rename",
                           "asset-fsync"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (json_array_append_new(requirements, json_string(names[i])))
            goto fail;
    struct utsname host;
    if (uname(&host))
        goto fail;
    const char *architecture = (!strcmp(host.machine, "arm64") || !strcmp(host.machine, "aarch64"))
                                   ? "arm64"
                               : !strcmp(host.machine, "x86_64") ? "amd64"
                                                                 : "unknown";
    if (json_object_set_new(plan, "host",
                            json_pack("{s:s,s:s,s:s}", "os", host.sysname, "architecture",
                                      architecture, "namespaceMappingCgroupMountPortChecks",
                                      "unperformed: assessed during authorized native startup")))
        goto fail;
    if (strcmp(host.sysname, "Linux") &&
        diagnostic(result, "UNSUPPORTED_HOST",
                   "Native Linux execution is required; no macOS execution backend is verified"))
        goto fail;
    if (!strcmp(architecture, "unknown") &&
        diagnostic(result, "UNSUPPORTED_PLATFORM",
                   "Only Linux amd64/arm64 image metadata is installed"))
        goto fail;
    json_t *checks = json_array();
    if (!checks)
        goto fail;
    for (size_t i = 1; i < sizeof(names) / sizeof(*names); i++) {
        json_t *check =
            json_pack("{s:s,s:s,s:s}", "check", names[i], "status", "unperformed", "reason",
                      strcmp(host.sysname, "Linux")
                          ? "Linux host or verified guest unavailable"
                          : "Configuration assessment does not allocate or test native isolation");
        if (!check || json_array_append_new(checks, check)) {
            json_decref(checks);
            goto fail;
        }
    }
    if (json_object_set_new(plan, "hostChecks", checks) ||
        json_object_set_new(
            plan, "dependencyResolution",
            json_pack("{s:s,s:s,s:s,s:s}", "jsonLibrary", "jansson-2.15.1 code dependency",
                      "stateLibrary", "sqlite-3.51.3 code dependency", "executionLibrary",
                      gstd_native_backend_available()
                          ? "libcrun linked; host execution checks deferred"
                          : "libcrun unavailable",
                      "rootlessNetwork",
                      gstd_network_engine_available() ? "libslirp linked; namespace checks deferred"
                                                      : "libslirp unavailable")))
        goto fail;
    json_t *resolutions = gstd_dependency_resolutions();
    if (!resolutions || gstd_dependency_resolutions_check(resolutions)) {
        json_decref(resolutions);
        goto fail;
    }
    if (json_object_set_new(json_object_get(plan, "dependencyResolution"), "capabilities",
                            resolutions))
        goto fail;
    if (strcmp(architecture, "unknown")) {
        json_t *manifest = gstd_asset_manifest(components, architecture);
        if (!manifest || gstd_asset_manifest_check(manifest)) {
            json_decref(manifest);
            goto fail;
        }
        if (json_object_set(plan, "assetManifestDigest",
                            json_object_get(manifest, "manifestDigest"))) {
            json_decref(manifest);
            goto fail;
        }
        if (json_object_set_new(plan, "assetManifest", manifest))
            goto fail;
    }
    json_t *images = json_object_get(profile, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        json_t *image = json_array_get(images, i);
        if (!selected(components, json_string_value(json_object_get(image, "component"))))
            continue;
        json_t *platforms = json_object_get(image, "platforms");
        for (size_t j = 0; j < json_array_size(platforms); j++) {
            json_t *platform = json_array_get(platforms, j);
            if (!same(json_object_get(platform, "architecture"), architecture))
                continue;
            json_t *asset = json_pack("{s:O,s:O,s:O,s:O,s:s,s:s}", "component",
                                      json_object_get(image, "component"), "source",
                                      json_object_get(image, "repository"), "digest",
                                      json_object_get(platform, "digest"), "architecture",
                                      json_object_get(platform, "architecture"), "os", "linux",
                                      "status", "verification-and-size-unperformed");
            if (!asset || json_array_append_new(json_object_get(result, "requiredAssets"), asset))
                goto fail;
        }
    }
    if ((!gstd_native_backend_available() || !gstd_image_backend_available() ||
         !gstd_network_engine_available()) &&
        diagnostic(result, "BACKEND_UNAVAILABLE",
                   "libcrun execution and rootless networking integration are not installed; host "
                   "checks cannot establish runnable support"))
        goto fail;
    if (diagnostic(result, "ASSET_VERIFICATION_UNPERFORMED",
                   "Cache content verification is deferred to an authorized start; no assets were "
                   "downloaded or activated") ||
        diagnostic(result, "NATIVE_VALIDATION_UNPERFORMED",
                   "Image-native configuration/policy validation is not executed; no credentials "
                   "were generated or enrolled"))
        goto fail;
    if (!strcmp(datastore, "scylladb") &&
        diagnostic(
            result, "DATASTORE_PROFILE_UNAVAILABLE",
            "Pinned Scylla image, keyspace and compatible migration target are not yet registered"))
        goto fail;
    if (json_object_get(params, "deploymentId") &&
        diagnostic(result, "OWNERSHIP_CHECK_UNPERFORMED",
                   "Deployment ownership is not queried by this read-only profile assessment yet"))
        goto fail;
done:
    json_decref(profile);
    json_decref(components);
    json_decref(envelope);
    return result;
fail:
    json_decref(result);
    result = NULL;
    goto done;
}
