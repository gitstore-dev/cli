#include "gitstored/assets.h"
#include "gitstored/config_validate.h"
#include "gitstored/dependencies.h"
#include "gitstored/profiles.h"
#include "gitstored/protocol.h"
#include "gitstored/validate.h"
#include "support.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int diagnostic(json_t *result, const char *code) {
    json_t *items = json_object_get(result, "diagnostics");
    for (size_t i = 0; i < json_array_size(items); i++) {
        const char *actual = json_string_value(json_object_get(json_array_get(items, i), "code"));
        if (actual && !strcmp(actual, code))
            return 1;
    }
    return 0;
}
int main(void) {
    gstd_json_initialize();
    int result = 0;
    json_t *params = NULL, *answer = NULL, *profile = NULL, *layers = NULL, *merged = NULL;
    char *root = gstd_test_dir();
    CHECK(root);
    params =
        json_pack("{s:s,s:s,s:b}", "profileId", "core", "profileVersion", "1", "allowProvision", 1);
    CHECK(params);
    answer = gstd_validate(params);
    CHECK(answer);
    CHECK(json_is_false(json_object_get(answer, "valid")));
    json_t *linked = gstd_dependency_resolutions();
    CHECK(linked);
    int backend_unavailable = 0;
    for (size_t i = 2; i < json_array_size(linked); i++)
        if (strcmp(json_string_value(json_object_get(json_array_get(linked, i), "compatibility")),
                   "verified-build"))
            backend_unavailable = 1;
    json_decref(linked);
    CHECK(diagnostic(answer, "BACKEND_UNAVAILABLE") == backend_unavailable);
    CHECK(diagnostic(answer, "NATIVE_VALIDATION_UNPERFORMED"));
    CHECK(json_array_size(json_object_get(answer, "requiredAssets")) == 3);
    json_t *effective = json_object_get(answer, "effectivePlan");
    CHECK(!gstd_asset_manifest_check(json_object_get(effective, "assetManifest")));
    CHECK(!gstd_dependency_resolutions_check(
        json_object_get(json_object_get(effective, "dependencyResolution"), "capabilities")));
    json_t *stages = json_object_get(json_object_get(answer, "effectivePlan"), "bootstrapStages");
    CHECK(json_array_size(stages) == 9);
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(stages, 2), "stage")),
                  "prepare-controller-signing-record"));
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(stages, 6), "stage")),
                  "enroll-controller-existing-public-key"));
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(stages, 7), "stage")),
                  "launch-controller"));
    json_decref(answer);
    answer = NULL;
    CHECK(!json_object_set_new(params, "profileVersion", json_string("unknown")));
    answer = gstd_validate(params);
    CHECK(answer && diagnostic(answer, "PROFILE_NOT_FOUND"));
    json_decref(answer);
    answer = NULL;
    CHECK(!json_object_set_new(params, "profileVersion", json_string("1")));
    CHECK(!json_object_set_new(params, "components", json_pack("[s]", "oidc")));
    answer = gstd_validate(params);
    CHECK(answer && diagnostic(answer, "COMPONENT_UNAVAILABLE"));
    json_decref(answer);
    answer = NULL;
    CHECK(!json_object_set_new(params, "components", json_pack("[s]", "git")));
    CHECK(!json_object_set_new(params, "datastore", json_string("other")));
    answer = gstd_validate(params);
    CHECK(answer && diagnostic(answer, "INVALID_ARGUMENT"));
    json_decref(answer);
    answer = NULL;
    profile = gstd_profile();
    CHECK(profile);
    CHECK(gstd_profile_check(profile) == 0);
    CHECK(!json_object_set_new(profile, "profileVersion", json_string("../../escape")));
    CHECK(gstd_profile_check(profile) < 0);
    layers = json_pack("[{s:{s:i,s:i}},{s:{s:i}}]", "api", "port", 4000, "git_port", 9000, "api",
                       "port", 4001);
    merged = gstd_config_merge("api", layers);
    CHECK(merged);
    CHECK(json_integer_value(json_object_get(json_object_get(merged, "api"), "port")) == 4001);
    CHECK(json_integer_value(json_object_get(json_object_get(merged, "api"), "git_port")) == 9000);
    CHECK(!gstd_config_merge("git", layers));
    CHECK(!json_array_append_new(layers, json_pack("{s:{s:i}}", "api", "rate_limit_burst", 10)));
    CHECK(!gstd_config_merge("api", layers));
    CHECK(!json_object_set_new(params, "datastore", json_string("scylladb")));
    answer = gstd_validate(params);
    CHECK(answer && !diagnostic(answer, "DATASTORE_PROFILE_UNAVAILABLE"));
    CHECK(json_array_size(json_object_get(answer, "requiredAssets")) == 3);
    json_t *configuration =
        json_object_get(json_object_get(answer, "effectivePlan"), "configuration");
    CHECK(!strcmp(
        json_string_value(json_object_get(json_array_get(configuration, 0), "datastoreBackend")),
        "scylla"));
    json_t *host_checks = json_object_get(json_object_get(answer, "effectivePlan"), "hostChecks");
    const char *filesystem_checks[] = {"openat2-no-mount-crossing", "atomic-no-replace-rename",
                                       "asset-fsync"};
    for (size_t i = 0; i < sizeof(filesystem_checks) / sizeof(*filesystem_checks); i++) {
        int found = 0;
        for (size_t j = 0; j < json_array_size(host_checks); j++) {
            json_t *check = json_array_get(host_checks, j);
            if (!strcmp(json_string_value(json_object_get(check, "check")), filesystem_checks[i])) {
                CHECK(!strcmp(json_string_value(json_object_get(check, "status")), "unperformed"));
                found = 1;
            }
        }
        CHECK(found);
    }
    /* No root/cache/config/credential argument exists in the validation API. */
    DIR *dir = opendir(root);
    CHECK(dir);
    size_t entries = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            entries++;
    closedir(dir);
    CHECK(!entries);
done:
    json_decref(params);
    json_decref(answer);
    json_decref(profile);
    json_decref(layers);
    json_decref(merged);
    if (root)
        gstd_test_remove(root);
    return result;
}
