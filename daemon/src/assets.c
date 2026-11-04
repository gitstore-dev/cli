#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/profiles.h"
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
    char hash[65], digest[72];
    if (gstd_fingerprint(manifest, hash))
        goto fail;
    snprintf(digest, sizeof(digest), "sha256:%s", hash);
    if (json_object_set_new(manifest, "manifestDigest", json_string(digest)))
        goto fail;
done:
    json_decref(profile);
    return manifest;
fail:
    json_decref(manifest);
    manifest = NULL;
    goto done;
}
int gstd_asset_manifest_check(const json_t *manifest) {
    if (!json_is_object(manifest))
        return -1;
    size_t size = json_dumpb(manifest, NULL, 0, JSON_COMPACT);
    if (!size || size > 65536)
        return -1;
    json_t *images = json_object_get(manifest, "images");
    if (!json_is_array(images) || !json_array_size(images) || json_array_size(images) > 3)
        return -1;
    const char *architecture =
        json_string_value(json_object_get(json_object_get(manifest, "platform"), "architecture"));
    json_t *components = json_array();
    if (!components)
        return -1;
    for (size_t i = 0; i < json_array_size(images); i++) {
        json_t *component = json_object_get(json_array_get(images, i), "component");
        if (!json_is_string(component) || json_array_append(components, component)) {
            json_decref(components);
            return -1;
        }
    }
    json_t *expected = gstd_asset_manifest(components, architecture);
    int valid = expected && json_equal(expected, manifest);
    json_decref(expected);
    json_decref(components);
    return valid ? 0 : -1;
}
