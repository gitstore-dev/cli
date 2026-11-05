#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/profiles.h"
#include "migration.h"
#include "oidc_profile.h"
#include "scylla_profile.inc"
#include <stdio.h>
#include <string.h>

static int same(const json_t *value, const char *text) {
    return json_is_string(value) && json_string_length(value) == strlen(text) &&
           !memcmp(json_string_value(value), text, strlen(text));
}
static int contains(const json_t *array, const json_t *value) {
    for (size_t i = 0; i < json_array_size(array); i++)
        if (json_equal(json_array_get(array, i), value))
            return 1;
    return 0;
}
static int selections(const json_t *components) {
    size_t count = json_array_size(components);
    if (!json_is_array(components) || !count || count > 3)
        return -1;
    for (size_t i = 0; i < count; i++) {
        json_t *value = json_array_get(components, i);
        if (!same(value, "api") && !same(value, "git") && !same(value, "controller"))
            return -1;
        for (size_t j = 0; j < i; j++)
            if (json_equal(value, json_array_get(components, j)))
                return -1;
    }
    return 0;
}
static int seal(json_t *manifest) {
    json_object_del(manifest, "manifestDigest");
    char hash[65], digest[72];
    if (gstd_fingerprint(manifest, hash))
        return -1;
    snprintf(digest, sizeof(digest), "sha256:%s", hash);
    return json_object_set_new(manifest, "manifestDigest", json_string(digest));
}
json_t *gstd_asset_manifest(const json_t *components, const char *architecture) {
    if (selections(components) || !architecture ||
        (strcmp(architecture, "amd64") && strcmp(architecture, "arm64")))
        return NULL;
    json_t *profile = gstd_profile(), *manifest = NULL;
    if (!profile)
        return NULL;
    manifest = json_pack("{s:i,s:O,s:O,s:s,s:O,s:{s:s,s:s},s:[]}", "schemaVersion", 1, "profileId",
                         json_object_get(profile, "profileId"), "profileVersion",
                         json_object_get(profile, "profileVersion"), "profileDigest",
                         gstd_profile_digest(), "serverRevision",
                         json_object_get(profile, "serverRevision"), "platform", "os", "linux",
                         "architecture", architecture, "images");
    if (!manifest)
        goto done;
    json_t *images = json_object_get(profile, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        json_t *image = json_array_get(images, i), *component = json_object_get(image, "component");
        if (!contains(components, component))
            continue;
        json_t *platforms = json_object_get(image, "platforms"), *selected = NULL;
        for (size_t j = 0; j < json_array_size(platforms); j++)
            if (same(json_object_get(json_array_get(platforms, j), "architecture"), architecture))
                selected = json_array_get(platforms, j);
        if (!selected)
            goto fail;
        json_t *asset = json_pack(
            "{s:O,s:s,s:O,s:O,s:O,s:O,s:s,s:O,s:O}", "component", component, "kind", "oci-image",
            "source", json_object_get(image, "repository"), "indexDigest",
            json_object_get(image, "indexDigest"), "digest", json_object_get(selected, "digest"),
            "configDigest", json_object_get(selected, "configDigest"), "integrityPolicy",
            "source-platform-digest", "tokenOrigins",
            json_object_get(json_object_get(image, "transportPolicy"), "tokenOrigins"),
            "blobOrigins",
            json_object_get(json_object_get(image, "transportPolicy"), "blobOrigins"));
        if (!asset || json_array_append_new(json_object_get(manifest, "images"), asset))
            goto fail;
    }
    if (seal(manifest))
        goto fail;
done:
    json_decref(profile);
    return manifest;
fail:
    json_decref(manifest);
    manifest = NULL;
    goto done;
}
json_t *gstd_asset_manifest_for_datastore(const json_t *components, const char *architecture,
                                          const char *datastore) {
    if (!datastore)
        return NULL;
    if (!strcmp(datastore, "memdb"))
        return gstd_asset_manifest(components, architecture);
    if (strcmp(datastore, "scylladb"))
        return NULL;
    int api = 0;
    for (size_t i = 0; i < json_array_size(components); i++)
        if (same(json_array_get(components, i), "api"))
            api = 1;
    if (!api)
        return NULL;
    json_t *manifest = gstd_asset_manifest(components, architecture);
    json_t *profile = gstd_json_parse((const unsigned char *)scylla_profile_json,
                                      sizeof(scylla_profile_json) - 1);
    json_t *image = json_object_get(profile, "image"), *selected = NULL, *target = NULL;
    const char *version = json_string_value(json_object_get(profile, "migrationTarget"));
    char profile_hash[65], target_hash[65];
    if (!manifest || !profile || !same(json_object_get(profile, "profileId"), "core") ||
        !same(json_object_get(profile, "profileVersion"), "1") ||
        !same(json_object_get(profile, "serverRevision"),
              "06152d193e854bc643fa884daa8b9f30b0d75d0f") ||
        !same(json_object_get(profile, "datastore"), "scylladb") ||
        !same(json_object_get(profile, "serverBackend"), "scylla") || !version ||
        !(target = gstd_migration_target(version)) || gstd_fingerprint(profile, profile_hash) ||
        gstd_fingerprint(target, target_hash))
        goto fail;
    json_t *platforms = json_object_get(image, "platforms");
    for (size_t i = 0; i < json_array_size(platforms); i++)
        if (same(json_object_get(json_array_get(platforms, i), "architecture"), architecture)) {
            if (selected)
                goto fail;
            selected = json_array_get(platforms, i);
        }
    if (!selected)
        goto fail;
    json_t *asset = json_pack(
        "{s:s,s:s,s:O,s:O,s:O,s:O,s:s,s:O,s:O}", "component", "scylladb", "kind", "oci-image",
        "source", json_object_get(image, "repository"), "indexDigest",
        json_object_get(image, "indexDigest"), "digest", json_object_get(selected, "digest"),
        "configDigest", json_object_get(selected, "configDigest"), "integrityPolicy",
        "source-platform-digest", "tokenOrigins",
        json_object_get(json_object_get(image, "transportPolicy"), "tokenOrigins"), "blobOrigins",
        json_object_get(json_object_get(image, "transportPolicy"), "blobOrigins"));
    if (!asset || json_array_append_new(json_object_get(manifest, "images"), asset) ||
        json_object_set_new(manifest, "datastore", json_string("scylladb")) ||
        json_object_set_new(manifest, "datastoreProfileDigest", json_string(profile_hash)) ||
        json_object_set_new(manifest, "migrationTarget", json_string(version)) ||
        json_object_set_new(manifest, "migrationTargetDigest", json_string(target_hash)) ||
        seal(manifest))
        goto fail;
    json_decref(target);
    json_decref(profile);
    return manifest;
fail:
    json_decref(target);
    json_decref(profile);
    json_decref(manifest);
    return NULL;
}
static json_t *component_projection(const json_t *manifest, const char *component) {
    json_t *images = json_object_get(manifest, "images"), *selected = NULL;
    for (size_t i = 0; i < json_array_size(images); i++)
        if (same(json_object_get(json_array_get(images, i), "component"), component))
            selected = json_array_get(images, i);
    if (!selected)
        return NULL;
    if (gstd_oidc_component(component)) {
        const char *architecture = json_string_value(
            json_object_get(json_object_get(manifest, "platform"), "architecture"));
        json_t *single = gstd_oidc_manifest_component(architecture, component);
        if (!single ||
            !json_equal(selected, json_array_get(json_object_get(single, "images"), 0))) {
            json_decref(single);
            return NULL;
        }
        return single;
    }
    if (!strcmp(component, "scylladb") && same(json_object_get(manifest, "profileVersion"), "2")) {
        const char *architecture = json_string_value(
            json_object_get(json_object_get(manifest, "platform"), "architecture"));
        json_t *core = json_pack("[s,s]", "api", "git");
        json_t *base =
            core ? gstd_asset_manifest_for_datastore(core, architecture, "scylladb") : NULL;
        json_t *single = base ? component_projection(base, "scylladb") : NULL;
        json_decref(base);
        json_decref(core);
        return single;
    }
    if (strcmp(component, "scylladb")) {
        json_t *components = json_pack("[s]", component);
        const char *architecture = json_string_value(
            json_object_get(json_object_get(manifest, "platform"), "architecture"));
        json_t *single = components ? gstd_asset_manifest(components, architecture) : NULL;
        json_decref(components);
        if (!single ||
            !json_equal(selected, json_array_get(json_object_get(single, "images"), 0))) {
            json_decref(single);
            return NULL;
        }
        return single;
    }
    json_t *single = json_deep_copy(manifest);
    if (!single)
        return NULL;
    images = json_object_get(single, "images");
    for (size_t i = json_array_size(images); i-- > 0;)
        if (!same(json_object_get(json_array_get(images, i), "component"), component) &&
            json_array_remove(images, i)) {
            json_decref(single);
            return NULL;
        }
    /* A schema/helper policy change does not change the cached Scylla image.
     * Execution still validates the full generation-bound manifest separately. */
    json_object_del(single, "datastoreProfileDigest");
    json_object_del(single, "migrationTarget");
    json_object_del(single, "migrationTargetDigest");
    if (seal(single)) {
        json_decref(single);
        return NULL;
    }
    return single;
}
json_t *gstd_asset_manifest_component(const json_t *manifest, const char *component) {
    return component && !gstd_asset_manifest_check(manifest)
               ? component_projection(manifest, component)
               : NULL;
}
int gstd_asset_manifest_check(const json_t *manifest) {
    if (!json_is_object(manifest))
        return -1;
    size_t size = json_dumpb(manifest, NULL, 0, JSON_COMPACT);
    if (!size || size > 65536)
        return -1;
    if (same(json_object_get(manifest, "profileId"), "core") &&
        same(json_object_get(manifest, "profileVersion"), "2"))
        return gstd_oidc_core_manifest_check(manifest);
    if (same(json_object_get(manifest, "profileId"), "reference-oidc"))
        return gstd_oidc_manifest_check(manifest);
    json_t *images = json_object_get(manifest, "images");
    if (!json_is_array(images) || !json_array_size(images) || json_array_size(images) > 4)
        return -1;
    const char *architecture =
        json_string_value(json_object_get(json_object_get(manifest, "platform"), "architecture"));
    json_t *components = json_array();
    if (!components)
        return -1;
    json_t *store = json_object_get(manifest, "datastore");
    int persistent = same(store, "scylladb"), datastore_images = 0;
    if (store && !persistent) {
        json_decref(components);
        return -1;
    }
    for (size_t i = 0; i < json_array_size(images); i++) {
        json_t *component = json_object_get(json_array_get(images, i), "component");
        if (same(component, "scylladb")) {
            datastore_images++;
            continue;
        }
        if (!json_is_string(component) || json_array_append(components, component)) {
            json_decref(components);
            return -1;
        }
    }
    json_t *expected = NULL;
    if (datastore_images == persistent) {
        if (persistent && !json_array_size(components)) {
            json_t *core = json_pack("[s,s]", "api", "git");
            json_t *full =
                core ? gstd_asset_manifest_for_datastore(core, architecture, "scylladb") : NULL;
            expected = full ? component_projection(full, "scylladb") : NULL;
            json_decref(full);
            json_decref(core);
        } else
            expected = gstd_asset_manifest_for_datastore(components, architecture,
                                                         persistent ? "scylladb" : "memdb");
    }
    int valid = expected && json_equal(expected, manifest);
    json_decref(expected);
    json_decref(components);
    return valid ? 0 : -1;
}
