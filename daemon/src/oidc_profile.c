#include "oidc_profile.h"
#include "core_oidc_profile.inc"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/profiles.h"
#include "gitstored/protocol.h"
#include "oidc_profile.inc"
#include <stdio.h>
#include <string.h>
int gstd_oidc_component(const char *component) {
    static const char *names[] = {"oidc-hydra", "oidc-kratos", "oidc-bridge", "oidc-ui",
                                  "oidc-smtp"};
    if (!component)
        return 0;
    for (size_t i = 0; i < 5; i++)
        if (!strcmp(component, names[i]))
            return 1;
    return 0;
}
json_t *gstd_oidc_manifest(const char *architecture) {
    if (!architecture || (strcmp(architecture, "amd64") && strcmp(architecture, "arm64")))
        return NULL;
    json_t *profile =
        gstd_json_parse((const unsigned char *)oidc_profile_json, sizeof(oidc_profile_json) - 1);
    json_t *manifest = profile
                           ? json_pack("{s:i,s:s,s:s,s:s,s:O,s:{s:s,s:s},s:[]}", "schemaVersion", 1,
                                       "profileId", "reference-oidc", "profileVersion", "1",
                                       "profileDigest", oidc_profile_digest, "serverRevision",
                                       json_object_get(profile, "serverRevision"), "platform", "os",
                                       "linux", "architecture", architecture, "images")
                           : NULL;
    json_t *images = json_object_get(profile, "images");
    int valid = manifest && json_array_size(images) == 5;
    for (size_t i = 0; valid && i < 5; i++) {
        json_t *image = json_array_get(images, i), *platforms = json_object_get(image, "platforms"),
               *selected = NULL;
        const char *component = json_string_value(json_object_get(image, "component"));
        if (!gstd_oidc_component(component)) {
            valid = 0;
            break;
        }
        for (size_t j = 0; j < json_array_size(platforms); j++) {
            json_t *platform = json_array_get(platforms, j);
            const char *arch = json_string_value(json_object_get(platform, "architecture"));
            if (arch && !strcmp(arch, architecture))
                selected = platform;
        }
        json_t *policy = json_object_get(image, "transportPolicy");
        json_t *asset =
            selected
                ? json_pack("{s:s,s:s,s:O,s:O,s:O,s:O,s:s,s:O,s:O}", "component", component, "kind",
                            "oci-image", "source", json_object_get(image, "repository"),
                            "indexDigest", json_object_get(image, "indexDigest"), "digest",
                            json_object_get(selected, "digest"), "configDigest",
                            json_object_get(selected, "configDigest"), "integrityPolicy",
                            "source-platform-digest", "tokenOrigins",
                            json_object_get(policy, "tokenOrigins"), "blobOrigins",
                            json_object_get(policy, "blobOrigins"))
                : NULL;
        valid = asset && !json_array_append_new(json_object_get(manifest, "images"), asset);
    }
    if (valid) {
        char hash[65], digest[72];
        valid = !gstd_fingerprint(manifest, hash);
        snprintf(digest, sizeof(digest), "sha256:%s", valid ? hash : "");
        valid = valid && !json_object_set_new(manifest, "manifestDigest", json_string(digest));
    }
    json_decref(profile);
    if (!valid) {
        json_decref(manifest);
        return NULL;
    }
    return manifest;
}
json_t *gstd_oidc_manifest_component(const char *architecture, const char *component) {
    if (!gstd_oidc_component(component))
        return NULL;
    json_t *manifest = gstd_oidc_manifest(architecture);
    if (!manifest)
        return NULL;
    json_t *images = json_object_get(manifest, "images");
    for (size_t i = json_array_size(images); i-- > 0;) {
        const char *name =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (!name || (strcmp(name, component) && json_array_remove(images, i)))
            goto fail;
    }
    json_object_del(manifest, "manifestDigest");
    char hash[65], digest[72];
    if (json_array_size(images) != 1 || gstd_fingerprint(manifest, hash))
        goto fail;
    snprintf(digest, sizeof(digest), "sha256:%s", hash);
    if (json_object_set_new(manifest, "manifestDigest", json_string(digest)))
        goto fail;
    return manifest;
fail:
    json_decref(manifest);
    return NULL;
}
int gstd_oidc_manifest_check(const json_t *manifest) {
    if (!json_is_object(manifest))
        return -1;
    const char *architecture =
        json_string_value(json_object_get(json_object_get(manifest, "platform"), "architecture"));
    const json_t *images = json_object_get(manifest, "images");
    const char *component =
        json_array_size(images) == 1
            ? json_string_value(json_object_get(json_array_get(images, 0), "component"))
            : NULL;
    json_t *expected = json_array_size(images) == 1
                           ? gstd_oidc_manifest_component(architecture, component)
                           : gstd_oidc_manifest(architecture);
    int valid = expected && json_equal(expected, manifest);
    json_decref(expected);
    return valid ? 0 : -1;
}

static int same(const json_t *value, const char *expected) {
    return json_is_string(value) && json_string_length(value) == strlen(expected) &&
           !memcmp(json_string_value(value), expected, strlen(expected));
}
const char *gstd_oidc_core_profile_digest(void) { return core_oidc_profile_digest; }
json_t *gstd_oidc_core_components(const json_t *requested) {
    int controller = requested == NULL, oidc = 0, have_core = requested == NULL;
    if (requested) {
        if (!json_is_array(requested) || !json_array_size(requested) ||
            json_array_size(requested) > 4)
            return NULL;
        for (size_t i = 0; i < json_array_size(requested); i++) {
            const json_t *component = json_array_get(requested, i);
            if (!same(component, "api") && !same(component, "git") &&
                !same(component, "controller") && !same(component, "oidc"))
                return NULL;
            for (size_t j = 0; j < i; j++)
                if (json_equal(component, json_array_get(requested, j)))
                    return NULL;
            controller |= same(component, "controller");
            oidc |= same(component, "oidc");
            have_core |= !same(component, "oidc");
        }
    }
    json_t *components = !have_core   ? json_array()
                         : controller ? json_pack("[s,s,s]", "api", "git", "controller")
                                      : json_pack("[s,s]", "api", "git");
    if (components && oidc && json_array_append_new(components, json_string("oidc"))) {
        json_decref(components);
        return NULL;
    }
    return components;
}
static int core_definition_valid(const json_t *definition, const json_t *oidc) {
    const char *oidc_digest = json_string_value(json_object_get(oidc, "profileDigest"));
    if (!oidc_digest)
        return 0;
    json_t *expected = json_pack(
        "{s:i,s:s,s:s,s:s,s:{s:s,s:s,s:s},s:{s:s,s:s,s:s},s:[s,s,s],s:[s],s:[s,s],s:s}",
        "schemaVersion", 1, "profileId", "core", "profileVersion", "2", "serverRevision",
        "06152d193e854bc643fa884daa8b9f30b0d75d0f", "baseProfile", "profileId", "core",
        "profileVersion", "1", "digest", gstd_profile_digest(), "optionalProfile", "profileId",
        "reference-oidc", "profileVersion", "1", "digest", oidc_digest, "components", "api", "git",
        "controller", "optionalComponents", "oidc", "datastores", "memdb", "scylladb",
        "oidcDataFormat", "rootless-single-uid-sqlite-v1");
    int valid = expected && json_equal(definition, expected);
    json_decref(expected);
    return valid;
}
json_t *gstd_oidc_core_manifest(const json_t *requested, const char *architecture,
                                const char *datastore) {
    if (!datastore || (strcmp(datastore, "memdb") && strcmp(datastore, "scylladb")))
        return NULL;
    json_t *definition = gstd_json_parse((const unsigned char *)core_oidc_profile_json,
                                         sizeof(core_oidc_profile_json) - 1),
           *components = gstd_oidc_core_components(requested), *core = json_array(),
           *oidc = gstd_oidc_manifest(architecture), *manifest = NULL;
    if (!definition || !components || !core || !oidc || !core_definition_valid(definition, oidc))
        goto done;
    int selected = 0;
    for (size_t i = 0; i < json_array_size(components); i++) {
        json_t *component = json_array_get(components, i);
        if (same(component, "oidc"))
            selected = 1;
        else if (json_array_append(core, component))
            goto done;
    }
    if (json_array_size(core))
        manifest = gstd_asset_manifest_for_datastore(core, architecture, datastore);
    else if (!strcmp(datastore, "memdb"))
        manifest = json_pack("{s:i,s:s,s:s,s:s,s:O,s:O,s:[]}", "schemaVersion", 1, "profileId",
                             "core", "profileVersion", "1", "profileDigest", gstd_profile_digest(),
                             "serverRevision", json_object_get(oidc, "serverRevision"), "platform",
                             json_object_get(oidc, "platform"), "images");
    if (!manifest)
        goto done;
    if (json_object_set_new(manifest, "profileVersion", json_string("2")) ||
        json_object_set_new(manifest, "profileDigest", json_string(core_oidc_profile_digest)) ||
        json_object_set_new(manifest, "baseProfileDigest", json_string(gstd_profile_digest())))
        goto fail;
    if (selected) {
        json_t *images = json_object_get(oidc, "images");
        for (size_t i = 0; i < json_array_size(images); i++)
            if (json_array_append(json_object_get(manifest, "images"), json_array_get(images, i)))
                goto fail;
        if (json_object_set(manifest, "oidcProfileDigest", json_object_get(oidc, "profileDigest")))
            goto fail;
    }
    json_object_del(manifest, "manifestDigest");
    char hash[65], digest[72];
    if (gstd_fingerprint(manifest, hash))
        goto fail;
    snprintf(digest, sizeof(digest), "sha256:%s", hash);
    if (json_object_set_new(manifest, "manifestDigest", json_string(digest)))
        goto fail;
    goto done;
fail:
    json_decref(manifest);
    manifest = NULL;
done:
    json_decref(oidc);
    json_decref(core);
    json_decref(components);
    json_decref(definition);
    return manifest;
}
int gstd_oidc_core_manifest_check(const json_t *manifest) {
    if (!json_is_object(manifest))
        return -1;
    json_t *images = json_object_get(manifest, "images"), *components = json_array();
    size_t count = json_array_size(images);
    if (!components || !json_is_array(images) || count < 2 || count > 9) {
        json_decref(components);
        return -1;
    }
    unsigned oidc = 0, scylla = 0;
    for (size_t i = 0; i < count; i++) {
        const json_t *component = json_object_get(json_array_get(images, i), "component");
        const char *name = json_string_value(component);
        if (!name)
            goto fail;
        if (same(component, "scylladb"))
            scylla++;
        else if (gstd_oidc_component(name))
            oidc++;
        else if ((!same(component, "api") && !same(component, "git") &&
                  !same(component, "controller")) ||
                 json_array_append(components, (json_t *)component))
            goto fail;
    }
    const json_t *datastore = json_object_get(manifest, "datastore");
    if ((oidc && oidc != 5) || scylla > 1 ||
        (scylla ? !same(datastore, "scylladb") : datastore != NULL) ||
        (oidc && json_array_append_new(components, json_string("oidc"))))
        goto fail;
    const char *architecture =
        json_string_value(json_object_get(json_object_get(manifest, "platform"), "architecture"));
    json_t *expected =
        gstd_oidc_core_manifest(components, architecture, scylla ? "scylladb" : "memdb");
    int valid = expected && json_equal(expected, manifest);
    json_decref(expected);
    json_decref(components);
    return valid ? 0 : -1;
fail:
    json_decref(components);
    return -1;
}
