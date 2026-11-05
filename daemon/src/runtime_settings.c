#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "runtime_settings.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int private_directory(int fd) {
    struct stat info;
    return fd >= 3 && !fstat(fd, &info) && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
           (info.st_mode & 07777) == 0700;
}
/* No mkdir, canonicalization through symlinks or ambient helper discovery. */
static int owned_path(const char *path) {
    size_t length = path ? strnlen(path, 4097) : 0;
    if (length < 2 || length > 4095 || path[0] != '/' || path[length - 1] == '/')
        return -1;
    char *copy = strdup(path);
    if (!copy)
        return -1;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    char *saved = NULL, *part = strtok_r(copy, "/", &saved);
    while (part && fd >= 0) {
        struct stat parent;
        if (!strcmp(part, ".") || !strcmp(part, "..") || fstat(fd, &parent) ||
            (parent.st_uid != 0 && parent.st_uid != geteuid()) ||
            ((parent.st_mode & 0022) && !(parent.st_uid == 0 && (parent.st_mode & S_ISVTX)))) {
            close(fd);
            fd = -1;
            break;
        }
        int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(fd);
        fd = child;
        part = strtok_r(NULL, "/", &saved);
    }
    free(copy);
    if (fd >= 0 && !private_directory(fd)) {
        close(fd);
        fd = -1;
    }
    return fd;
}
static const char *path_string(json_t *value) {
    if (!json_is_string(value) || json_string_length(value) > 4095)
        return NULL;
    const char *path = json_string_value(value);
    return strlen(path) == json_string_length(value) ? path : NULL;
}
static int protected_source(int *output, json_t *source) {
    const char *path = path_string(json_object_get(source, "path"));
    if (!path || strlen(path) > 4000 || json_object_size(source) != 2)
        return -1;
    int directory = owned_path(path);
    if (directory < 3)
        return -1;
    *output = directory;
    return 0;
}
static int same_file(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
#define MTIM st_mtimespec
#define CTIM st_ctimespec
#else
#define MTIM st_mtim
#define CTIM st_ctim
#endif
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_uid == b->st_uid &&
           a->st_mode == b->st_mode && a->st_nlink == b->st_nlink && a->st_size == b->st_size &&
           a->MTIM.tv_sec == b->MTIM.tv_sec && a->MTIM.tv_nsec == b->MTIM.tv_nsec &&
           a->CTIM.tv_sec == b->CTIM.tv_sec && a->CTIM.tv_nsec == b->CTIM.tv_nsec;
}
void gstd_runtime_settings_clear(gstd_runtime_settings *settings) {
    if (!settings)
        return;
    json_decref(settings->credential_source);
    if (settings->credential_parent_fd >= 3)
        close(settings->credential_parent_fd);
    json_decref(settings->oidc_credential_source);
    if (settings->oidc_credential_parent_fd >= 3)
        close(settings->oidc_credential_parent_fd);
    for (unsigned i = 0; i < 9; i++)
        if (settings->offline_fds[i] >= 3)
            close(settings->offline_fds[i]);
    *settings = (gstd_runtime_settings)GSTD_RUNTIME_SETTINGS_INITIAL;
}
int gstd_runtime_settings_image(const gstd_runtime_settings *settings, const char *component) {
    static const char *names[] = {"api",         "git",        "controller",
                                  "scylladb",    "oidc-hydra", "oidc-kratos",
                                  "oidc-bridge", "oidc-ui",    "oidc-smtp"};
    for (unsigned i = 0; settings && component && i < 9; i++)
        if (!strcmp(component, names[i]))
            return settings->offline_fds[i] >= 3 ? settings->offline_fds[i] : -1;
    return -1;
}
int gstd_runtime_settings_load(int state, gstd_runtime_settings *output) {
    if (!output || !private_directory(state))
        return -1;
    int fd = openat(state, "settings.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    int open_error = errno;
    gstd_runtime_settings result = GSTD_RUNTIME_SETTINGS_INITIAL;
    result.credential_source = json_pack("{s:s}", "kind", "os-store");
    result.oidc_credential_source = json_pack("{s:s}", "kind", "os-store");
    if (!result.credential_source || !result.oidc_credential_source) {
        if (fd >= 0)
            close(fd);
        gstd_runtime_settings_clear(&result);
        return -1;
    }
    if (fd < 0) {
        if (open_error == ENOENT) {
            *output = result;
            return 0;
        }
        gstd_runtime_settings_clear(&result);
        return -1;
    }
    struct stat before, after, named;
    unsigned char bytes[16385];
    size_t used = 0;
    int valid = !fstat(fd, &before) && S_ISREG(before.st_mode) && before.st_uid == geteuid() &&
                before.st_nlink == 1 && (before.st_mode & 07777) == 0600 && before.st_size > 0 &&
                before.st_size <= 16384;
    while (valid && used < (size_t)before.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)before.st_size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            valid = 0;
            break;
        }
        used += (size_t)n;
    }
    valid = valid && !fstat(fd, &after) && same_file(&before, &after) &&
            !fstatat(state, "settings.json", &named, AT_SYMLINK_NOFOLLOW) &&
            same_file(&after, &named);
    close(fd);
    json_t *root = valid ? gstd_json_parse(bytes, used) : NULL;
    valid = json_is_object(root) && json_is_integer(json_object_get(root, "schemaVersion")) &&
            json_integer_value(json_object_get(root, "schemaVersion")) == 1;
    const char *key;
    json_t *value;
    if (valid)
        for (void *it = json_object_iter(root); it; it = json_object_iter_next(root, it)) {
            key = json_object_iter_key(it);
            if (strlen(key) != json_object_iter_key_len(it) ||
                (strcmp(key, "schemaVersion") && strcmp(key, "bootstrapCredentialSource") &&
                 strcmp(key, "oidcCredentialSource") && strcmp(key, "offlineLayouts")))
                valid = 0;
        }
    json_t *source = json_object_get(root, "bootstrapCredentialSource");
    if (valid && source) {
        const char *kind = path_string(json_object_get(source, "kind"));
        valid = kind && ((!strcmp(kind, "os-store") && json_object_size(source) == 1) ||
                         (!strcmp(kind, "protected-directory") &&
                          !protected_source(&result.credential_parent_fd, source)));
        if (valid) {
            json_decref(result.credential_source);
            result.credential_source = json_incref(source);
        }
    }
    source = json_object_get(root, "oidcCredentialSource");
    if (valid && source) {
        const char *kind = path_string(json_object_get(source, "kind"));
        valid = kind && ((!strcmp(kind, "os-store") && json_object_size(source) == 1) ||
                         (!strcmp(kind, "protected-directory") &&
                          !protected_source(&result.oidc_credential_parent_fd, source)));
        if (valid) {
            json_decref(result.oidc_credential_source);
            result.oidc_credential_source = json_incref(source);
        }
    }
    json_t *offline = json_object_get(root, "offlineLayouts");
    if (valid && offline) {
        valid = json_is_object(offline) && json_object_size(offline) <= 9;
        if (valid)
            json_object_foreach(offline, key, value) {
                int index = !strcmp(key, "api")           ? 0
                            : !strcmp(key, "git")         ? 1
                            : !strcmp(key, "controller")  ? 2
                            : !strcmp(key, "scylladb")    ? 3
                            : !strcmp(key, "oidc-hydra")  ? 4
                            : !strcmp(key, "oidc-kratos") ? 5
                            : !strcmp(key, "oidc-bridge") ? 6
                            : !strcmp(key, "oidc-ui")     ? 7
                            : !strcmp(key, "oidc-smtp")   ? 8
                                                          : -1;
                const char *path = path_string(value);
                if (strlen(key) != json_object_iter_key_len(json_object_key_to_iter(key)) ||
                    index < 0 || !path || (result.offline_fds[index] = owned_path(path)) < 3) {
                    valid = 0;
                    break;
                }
            }
    }
    json_decref(root);
    if (!valid) {
        gstd_runtime_settings_clear(&result);
        return -1;
    }
    *output = result;
    return 0;
}

static json_t *material_source(const gstd_runtime_settings *settings, const char *store,
                               const char *deployment, int oidc) {
    if (!settings || !store || strnlen(store, 33) != 32 || !deployment ||
        !gstd_identifier(deployment, strnlen(deployment, 65)))
        return NULL;
    for (unsigned i = 0; i < 32; i++)
        if (!((store[i] >= '0' && store[i] <= '9') || (store[i] >= 'a' && store[i] <= 'f')))
            return NULL;
    const json_t *source = oidc ? settings->oidc_credential_source : settings->credential_source;
    int parent_fd = oidc ? settings->oidc_credential_parent_fd : settings->credential_parent_fd;
    const char *kind = path_string(json_object_get((json_t *)source, "kind"));
    if (!kind)
        return NULL;
    if (!strcmp(kind, "os-store"))
        return json_pack("{s:s}", "kind", "os-store");
    if (strcmp(kind, "protected-directory"))
        return NULL;
    const char *path = path_string(json_object_get((json_t *)source, "path"));
    if (!path)
        return NULL;
    int named = owned_path(path);
    struct stat held, current;
    int valid = named >= 3 && private_directory(parent_fd) && !fstat(parent_fd, &held) &&
                !fstat(named, &current) && held.st_dev == current.st_dev &&
                held.st_ino == current.st_ino;
    if (named >= 0)
        close(named);
    if (!valid)
        return NULL;
    json_t *identity =
        json_pack("{s:s,s:s,s:s,s:s}", "installation", store, "issuer",
                  oidc ? "gitstore-local/reference-oidc/1" : "gitstore-local/core/1", "account",
                  deployment, "purpose", oidc ? "oidc-provider-material" : "bootstrap-material");
    char digest[65];
    int hashed = identity ? gstd_fingerprint(identity, digest) : -1;
    json_decref(identity);
    if (hashed)
        return NULL;
    char file[4096];
    int n = snprintf(file, sizeof(file), "%s/%s-%s.json", path,
                     oidc ? "oidc-provider" : "bootstrap", digest);
    if (n < 0 || (size_t)n >= sizeof(file))
        return NULL;
    return json_pack("{s:s,s:s}", "kind", "protected-file", "path", file);
}

json_t *gstd_runtime_settings_source(const gstd_runtime_settings *settings, const char *store,
                                     const char *deployment) {
    return material_source(settings, store, deployment, 0);
}
json_t *gstd_runtime_settings_oidc_source(const gstd_runtime_settings *settings, const char *store,
                                          const char *deployment) {
    return material_source(settings, store, deployment, 1);
}
