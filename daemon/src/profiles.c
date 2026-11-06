#include "gitstored/profiles.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "oidc_profile.h"
#include "profile.inc"
#include <string.h>
const char *gstd_profile_digest(void) { return profile_digest; }
static int equals(const json_t *value, const char *expected) {
    return json_is_string(value) && json_string_length(value) == strlen(expected) &&
           !memcmp(json_string_value(value), expected, strlen(expected));
}
static int identifier(const json_t *value) {
    return json_is_string(value) &&
           gstd_identifier(json_string_value(value), json_string_length(value));
}
static int digest(const json_t *value) {
    if (!json_is_string(value) || json_string_length(value) != 71)
        return 0;
    const char *s = json_string_value(value);
    if (memcmp(s, "sha256:", 7))
        return 0;
    for (size_t i = 7; i < 71; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}
int gstd_profile_check(const json_t *p) {
    if (!json_is_object(p) || json_integer_value(json_object_get(p, "schemaVersion")) != 1 ||
        !identifier(json_object_get(p, "profileId")) ||
        !identifier(json_object_get(p, "profileVersion")) ||
        !equals(json_object_get(p, "serverRevision"), "06152d193e854bc643fa884daa8b9f30b0d75d0f"))
        return -1;
    json_t *names = json_object_get(p, "components"), *images = json_object_get(p, "images");
    if (!json_is_array(names) || json_array_size(names) != 3 || !json_is_array(images) ||
        json_array_size(images) != 3)
        return -1;
    const char *expected[] = {"api", "git", "controller"};
    const char *repositories[] = {"ghcr.io/gitstore-dev/api", "ghcr.io/gitstore-dev/git-service",
                                  "ghcr.io/gitstore-dev/controller-manager"};
    for (size_t i = 0; i < 3; i++) {
        json_t *image = json_array_get(images, i);
        if (!equals(json_array_get(names, i), expected[i]) ||
            !equals(json_object_get(image, "component"), expected[i]) ||
            !equals(json_object_get(image, "repository"), repositories[i]) ||
            !digest(json_object_get(image, "indexDigest")))
            return -1;
        json_t *policy = json_object_get(image, "transportPolicy"),
               *tokens = json_object_get(policy, "tokenOrigins"),
               *blobs = json_object_get(policy, "blobOrigins");
        if (!json_is_array(tokens) || json_array_size(tokens) || !json_is_array(blobs) ||
            json_array_size(blobs) != 1 ||
            !equals(json_array_get(blobs, 0), "https://pkg-containers.githubusercontent.com"))
            return -1;
        json_t *platforms = json_object_get(image, "platforms");
        if (!json_is_array(platforms) || json_array_size(platforms) != 2)
            return -1;
        for (size_t j = 0; j < 2; j++) {
            json_t *platform = json_array_get(platforms, j);
            if (!equals(json_object_get(platform, "os"), "linux") ||
                !equals(json_object_get(platform, "architecture"), j ? "arm64" : "amd64") ||
                !digest(json_object_get(platform, "digest")) ||
                !digest(json_object_get(platform, "configDigest")))
                return -1;
            json_t *command = json_object_get(platform, "command"),
                   *entry = json_object_get(platform, "entrypoint");
            if (!json_is_array(command) || json_array_size(command) != 1 ||
                !json_is_string(json_array_get(command, 0)) || !json_is_array(entry) ||
                json_array_size(entry))
                return -1;
        }
    }
    json_t *stores = json_object_get(p, "datastores"),
           *optional = json_object_get(p, "optionalComponents");
    if (!json_is_array(stores) || json_array_size(stores) != 2 ||
        !equals(json_array_get(stores, 0), "memdb") ||
        !equals(json_array_get(stores, 1), "scylladb") || !json_is_array(optional) ||
        json_array_size(optional) ||
        !equals(json_object_get(json_object_get(p, "datastoreMapping"), "scylladb"), "scylla"))
        return -1;
    return 0;
}
json_t *gstd_profile(void) {
    json_t *p = gstd_json_parse((const unsigned char *)profile_json, sizeof(profile_json) - 1);
    if (p && gstd_profile_check(p)) {
        json_decref(p);
        p = NULL;
    }
    return p;
}
json_t *gstd_profile_components(const json_t *requested) {
    int controller = requested == NULL;
    if (requested) {
        if (!json_is_array(requested) || !json_array_size(requested) ||
            json_array_size(requested) > 3)
            return NULL;
        for (size_t i = 0; i < json_array_size(requested); i++) {
            json_t *value = json_array_get(requested, i);
            if (!equals(value, "api") && !equals(value, "git") && !equals(value, "controller"))
                return NULL;
            for (size_t j = 0; j < i; j++)
                if (json_equal(value, json_array_get(requested, j)))
                    return NULL;
            if (equals(value, "controller"))
                controller = 1;
        }
    }
    return controller ? json_pack("[s,s,s]", "api", "git", "controller")
                      : json_pack("[s,s]", "api", "git");
}

json_t *gstd_profile_components_for(const char *version, const json_t *requested) {
    return !version                ? NULL
           : !strcmp(version, "1") ? gstd_profile_components(requested)
           : !strcmp(version, "2") ? gstd_oidc_core_components(requested)
                                   : NULL;
}
json_t *gstd_profile_manifest(const char *version, const json_t *components,
                              const char *architecture, const char *datastore) {
    return !version ? NULL
           : !strcmp(version, "1")
               ? gstd_asset_manifest_for_datastore(components, architecture, datastore)
           : !strcmp(version, "2") ? gstd_oidc_core_manifest(components, architecture, datastore)
                                   : NULL;
}
const char *gstd_profile_digest_for(const char *version) {
    return !version                ? NULL
           : !strcmp(version, "1") ? gstd_profile_digest()
           : !strcmp(version, "2") ? gstd_oidc_core_profile_digest()
                                   : NULL;
}
