#include "gitstored/assets.h"
#include "gitstored/dependencies.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
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
static int rehash(json_t *manifest) {
    char hash[65], digest[72];
    json_object_del(manifest, "manifestDigest");
    if (gstd_fingerprint(manifest, hash))
        return -1;
    snprintf(digest, sizeof(digest), "sha256:%s", hash);
    return json_object_set_new(manifest, "manifestDigest", json_string(digest));
}
int main(void) {
    gstd_json_initialize();
    int result = 0;
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller"), *manifest = NULL,
           *changed = NULL, *other = NULL, *resolutions = NULL, *persistent = NULL;
    CHECK(components);
    manifest = gstd_asset_manifest(components, "arm64");
    CHECK(manifest && !gstd_asset_manifest_check(manifest));
    CHECK(json_array_size(json_object_get(manifest, "images")) == 3);
    persistent = gstd_asset_manifest_for_datastore(components, "arm64", "scylladb");
    CHECK(persistent && !gstd_asset_manifest_check(persistent));
    CHECK(json_array_size(json_object_get(persistent, "images")) == 4);
    CHECK(json_equal(json_object_get(persistent, "profileDigest"),
                     json_object_get(manifest, "profileDigest")));
    CHECK(!json_equal(json_object_get(persistent, "manifestDigest"),
                      json_object_get(manifest, "manifestDigest")));
    json_t *datastore_image = json_array_get(json_object_get(persistent, "images"), 3);
    CHECK(!strcmp(json_string_value(json_object_get(datastore_image, "source")),
                  "docker.io/scylladb/scylla"));
    CHECK(!strcmp(json_string_value(json_object_get(datastore_image, "component")), "scylladb"));
    /* Cache identities follow image contents: shared application images retain
     * their memdb keys, while the implicit datastore has a registered descriptor. */
    other = gstd_asset_manifest_component(persistent, "api");
    changed = gstd_asset_manifest_component(manifest, "api");
    CHECK(other && changed && json_equal(other, changed));
    CHECK(!gstd_asset_manifest_check(other));
    json_decref(other);
    json_decref(changed);
    other = gstd_asset_manifest_component(persistent, "scylladb");
    changed = NULL;
    CHECK(other && !gstd_asset_manifest_check(other));
    CHECK(json_array_size(json_object_get(other, "images")) == 1);
    CHECK(!json_object_get(other, "migrationTarget") &&
          !json_object_get(other, "migrationTargetDigest") &&
          !json_object_get(other, "datastoreProfileDigest"));
    CHECK(json_equal(json_array_get(json_object_get(other, "images"), 0), datastore_image));
    changed = gstd_asset_manifest_component(other, "scylladb");
    CHECK(changed && json_equal(changed, other));
    CHECK(!gstd_asset_manifest_component(other, "api"));
    CHECK(!gstd_asset_manifest_component(manifest, "scylladb"));
    CHECK(!gstd_asset_manifest_component(persistent, "unknown"));
    CHECK(!gstd_asset_manifest_component(persistent, NULL));
    json_decref(changed);
    changed = json_deep_copy(other);
    CHECK(changed && !json_object_set_new(changed, "migrationTargetDigest", json_string("bad")));
    CHECK(!rehash(changed) && gstd_asset_manifest_check(changed) < 0);
    CHECK(!gstd_asset_manifest_component(changed, "scylladb"));
    json_decref(changed);
    json_decref(other);
    changed = other = NULL;
    changed = json_deep_copy(persistent);
    CHECK(changed &&
          !json_object_set_new(changed, "migrationTargetDigest", json_string("untrusted")));
    CHECK(!rehash(changed) && gstd_asset_manifest_check(changed) == -1);
    json_decref(changed);
    changed = json_deep_copy(persistent);
    CHECK(changed && !json_array_append(json_object_get(changed, "images"), datastore_image));
    CHECK(!rehash(changed) && gstd_asset_manifest_check(changed) == -1);
    json_decref(changed);
    changed = NULL;
    other = gstd_asset_manifest_for_datastore(components, "arm64", "memdb");
    CHECK(other && json_equal(other, manifest));
    json_decref(other);
    other = NULL;
    CHECK(!gstd_asset_manifest_for_datastore(components, "arm64", "unknown"));

    other = gstd_asset_manifest(components, "amd64");
    CHECK(other && !gstd_asset_manifest_check(other));
    CHECK(!json_equal(json_object_get(other, "manifestDigest"),
                      json_object_get(manifest, "manifestDigest")));
    json_decref(other);
    other = NULL;
    CHECK(!gstd_asset_manifest(components, "unknown"));
    changed = json_deep_copy(manifest);
    CHECK(changed);
    json_t *image = json_array_get(json_object_get(changed, "images"), 0);
    CHECK(!json_object_set_new(image, "source", json_string("untrusted.invalid/owned/image")));
    CHECK(!rehash(changed));
    CHECK(gstd_asset_manifest_check(changed) < 0);
    json_decref(changed);
    changed = json_deep_copy(manifest);
    CHECK(changed);
    image = json_array_get(json_object_get(changed, "images"), 0);
    CHECK(!json_object_set_new(
        image, "digest",
        json_string("sha256:0000000000000000000000000000000000000000000000000000000000000000")));
    CHECK(!rehash(changed) && gstd_asset_manifest_check(changed) < 0);
    json_decref(changed);
    changed = json_deep_copy(manifest);
    CHECK(changed);
    CHECK(!json_object_set_new(changed, "mount", json_string("/untrusted-path")));
    CHECK(!rehash(changed) && gstd_asset_manifest_check(changed) < 0);
    json_decref(changed);
    changed = json_deep_copy(manifest);
    CHECK(changed);
    image = json_array_get(json_object_get(changed, "images"), 0);
    CHECK(!json_array_append_new(json_object_get(image, "blobOrigins"),
                                 json_string("https://untrusted.invalid")));
    CHECK(!rehash(changed) && gstd_asset_manifest_check(changed) < 0);
    CHECK(!json_array_append_new(components, json_string("api")));
    CHECK(!gstd_asset_manifest(components, "arm64"));
    json_decref(components);
    components = json_pack("[s]", "oidc");
    CHECK(components && !gstd_asset_manifest(components, "arm64"));
    json_decref(components);
    components = json_pack("[s]", "api");
    CHECK(components);
    other = gstd_asset_manifest(components, "arm64");
    CHECK(other && !gstd_asset_manifest_check(other));
    CHECK(json_array_size(json_object_get(other, "images")) == 1);
    CHECK(!json_equal(json_object_get(other, "manifestDigest"),
                      json_object_get(manifest, "manifestDigest")));
    resolutions = gstd_dependency_resolutions();
    CHECK(resolutions && !gstd_dependency_resolutions_check(resolutions));
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(resolutions, 0), "origin")),
                  "code"));
    CHECK(json_is_false(json_object_get(json_array_get(resolutions, 0), "runtimeProvisioning")));
    json_decref(changed);
    changed = json_deep_copy(resolutions);
    CHECK(changed);
    json_t *fake = json_array_get(changed, 0);
    CHECK(!json_object_set_new(fake, "origin", json_string("host")));
    CHECK(!json_object_set_new(fake, "pathIdentity",
                               json_pack("{s:s,s:b}", "path", "/untrusted-path", "trusted", 1)));
    CHECK(gstd_dependency_resolutions_check(changed) < 0);
done:
    json_decref(components);
    json_decref(manifest);
    json_decref(changed);
    json_decref(other);
    json_decref(resolutions);
    json_decref(persistent);
    return result;
}
