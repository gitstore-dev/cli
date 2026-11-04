#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/dependencies.h"
#include "backend_libcrun.h"
#include "dependency_metadata.inc"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "image_backend.h"
#include "networking.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

static int add(json_t *array, const char *capability, const char *implementation,
               const char *origin, const char *version, const char *source, int available,
               const char *os, const char *architecture) {
    json_t *record = json_pack(
        "{s:s,s:s,s:s,s:s,s:s,s:s,s:{s:s,s:s},s:n,s:b}", "capability", capability, "implementation",
        implementation, "origin", origin, "version", version, "sourceIdentity", source,
        "compatibility", available ? "verified-build" : "unavailable", "platform", "os", os,
        "architecture", architecture, "pathIdentity", "runtimeProvisioning", 0);
    return !record ? -1 : json_array_append_new(array, record);
}
json_t *gstd_dependency_resolutions(void) {
    json_t *lock = gstd_json_parse((const unsigned char *)dependency_metadata,
                                   sizeof(dependency_metadata) - 1),
           *result = NULL;
    struct utsname host;
    if (!lock || uname(&host))
        goto done;
    result = json_array();
    if (!result)
        goto done;
    const char *architecture = (!strcmp(host.machine, "aarch64") || !strcmp(host.machine, "arm64"))
                                   ? "arm64"
                               : !strcmp(host.machine, "x86_64") ? "amd64"
                                                                 : "unknown";
    json_t *libraries = json_object_get(lock, "dependencies"),
           *json = json_object_get(libraries, "jansson"),
           *state = json_object_get(libraries, "sqlite");
    const char *json_version = json_string_value(json_object_get(json, "version")),
               *state_version = json_string_value(json_object_get(state, "version")),
               *json_source = json_string_value(json_object_get(json, "sha256")),
               *state_source = json_string_value(json_object_get(state, "sha256"));
    json_t *network = json_object_get(json_object_get(lock, "capabilityLibraries"), "network");
    const char *network_version = json_string_value(json_object_get(network, "version")),
               *network_revision = json_string_value(json_object_get(network, "revision")),
               *network_patch = json_string_value(json_object_get(network, "patchSha256"));
    char network_source[160];
    if (!network_version || !network_revision || !network_patch || strlen(network_revision) != 40 ||
        strlen(network_patch) != 64) {
        json_decref(result);
        result = NULL;
        goto done;
    }
    snprintf(network_source, sizeof(network_source), "git:%s;patch:%s", network_revision,
             network_patch);
    if (!json_version || !state_version || !json_source || !state_source ||
        add(result, "json-parser", "jansson", "code", json_version, json_source,
            !strcmp(jansson_version_str(), json_version) && JSON_PARSER_MAX_DEPTH == 33,
            host.sysname, architecture) ||
        add(result, "state-store", "sqlite", "code", state_version, state_source,
            !strcmp(sqlite3_libversion(), state_version) && !sqlite3_threadsafe() &&
                sqlite3_compileoption_used("OMIT_LOAD_EXTENSION"),
            host.sysname, architecture) ||
        add(result, "image-pipeline", "oci-client+ocirender", "code", "sdk-abi-1",
            "git:71c08729adeb88fbdda43966ece962d32b58fb8d;ocirender:0.2.3",
            gstd_image_backend_available(), host.sysname, architecture) ||
        add(result, "container-execution", "libcrun", "code", "1.30.1",
            "git:079ff6a7a16d029460af882f9ea44674e6a510b6", gstd_native_backend_available(),
            host.sysname, architecture) ||
        add(result, "rootless-network", "libslirp", "code", network_version, network_source,
            gstd_network_engine_available(), host.sysname, architecture)) {
        json_decref(result);
        result = NULL;
    }
done:
    json_decref(lock);
    return result;
}
int gstd_dependency_resolutions_check(const json_t *resolutions) {
    /* Only registered actual observations can prove compatibility. In particular
     * host/cache metadata cannot self-assert a compatible executable/library. */
    if (!json_is_array(resolutions) || json_array_size(resolutions) != 5)
        return -1;
    json_t *observed = gstd_dependency_resolutions();
    int valid = observed && json_equal(observed, resolutions);
    json_decref(observed);
    return valid ? 0 : -1;
}

static int stable(const struct stat *before, const struct stat *after) {
    if (before->st_dev != after->st_dev || before->st_ino != after->st_ino ||
        before->st_size != after->st_size || before->st_uid != after->st_uid ||
        before->st_mode != after->st_mode || before->st_nlink != after->st_nlink)
        return 0;
#ifdef __APPLE__
    return before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec &&
           before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
           before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec &&
           before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
#else
    return before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
#endif
}
json_t *gstd_dependency_asset_fd(const json_t *manifest, const char *component, int fd,
                                 const char *origin) {
    if (!component || !origin || (strcmp(origin, "host") && strcmp(origin, "cache")) ||
        gstd_asset_manifest_check(manifest))
        return NULL;
    json_t *image = NULL, *images = json_object_get(manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        json_t *candidate = json_array_get(images, i);
        const char *name = json_string_value(json_object_get(candidate, "component"));
        if (name && !strcmp(name, component))
            image = candidate;
    }
    if (!image)
        return NULL;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (copy < 0)
        return NULL;
    json_t *result = NULL;
    unsigned char *bytes = NULL;
    struct stat before, after;
    int flags = fcntl(copy, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(copy, &before) ||
        !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size <= 0 ||
        before.st_size > 1024 * 1024 || (before.st_uid != 0 && before.st_uid != geteuid()) ||
        (before.st_mode & 0022) ||
        (!strcmp(origin, "cache") &&
         (before.st_uid != geteuid() || (before.st_mode & 0777) != 0600)))
        goto done;
    size_t size = (size_t)before.st_size;
    bytes = malloc(size);
    if (!bytes)
        goto done;
    size_t have = 0;
    while (have < size) {
        ssize_t count = pread(copy, bytes + have, size - have, (off_t)have);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            goto done;
        have += (size_t)count;
    }
    unsigned char hash[32];
#ifdef __APPLE__
    if (!CC_SHA256(bytes, (CC_LONG)size, hash))
        goto done;
#else
    if (!SHA256(bytes, size, hash))
        goto done;
#endif
    char digest[72] = "sha256:";
    for (size_t i = 0; i < sizeof(hash); i++)
        snprintf(digest + 7 + i * 2, 3, "%02x", hash[i]);
    const char *expected = json_string_value(json_object_get(image, "digest"));
    if (!expected || strcmp(expected, digest) || fstat(copy, &after) || !stable(&before, &after))
        goto done;
    char device[32], inode[32];
    snprintf(device, sizeof(device), "%ju", (uintmax_t)before.st_dev);
    snprintf(inode, sizeof(inode), "%ju", (uintmax_t)before.st_ino);
    result = json_pack("{s:s,s:s,s:s,s:O,s:O,s:{s:O,s:O,s:O},s:s,s:{s:s,s:s,s:I,s:I,s:I},s:b}",
                       "capability", "image-manifest", "implementation", "oci-image", "origin",
                       origin, "version", json_object_get(manifest, "profileVersion"), "platform",
                       json_object_get(manifest, "platform"), "sourceIdentity", "source",
                       json_object_get(image, "source"), "digest", json_object_get(image, "digest"),
                       "configDigest", json_object_get(image, "configDigest"), "compatibility",
                       "verified-manifest-content", "pathIdentity", "device", device, "inode",
                       inode, "bytes", (json_int_t)size, "ownerUid", (json_int_t)before.st_uid,
                       "mode", (json_int_t)(before.st_mode & 0777), "runtimeProvisioning", 0);
done:
    free(bytes);
    close(copy);
    return result;
}
int gstd_dependency_asset_fd_check(const json_t *observed, const json_t *manifest,
                                   const char *component, int fd, const char *origin) {
    if (!json_is_object(observed))
        return -1;
    json_t *actual = gstd_dependency_asset_fd(manifest, component, fd, origin);
    int valid = actual && json_equal(observed, actual);
    json_decref(actual);
    return valid ? 0 : -1;
}
