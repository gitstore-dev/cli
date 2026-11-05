#define _GNU_SOURCE 1
#include "oidc_preparation.h"
#include "gitstored/protocol.h"
#include "oidc_material.h"
#include "oidc_profile.h"
#include <string.h>
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
#include "oidc_identity_schema.inc"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static int exact_file(int root, const char *name, const char *bytes, size_t length, int create) {
    if (!length || length > 32768)
        return -1;
    int fd = openat(root, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0) {
        struct stat before, after;
        unsigned char buffer[32768];
        int valid = !fstat(fd, &before) && S_ISREG(before.st_mode) && before.st_uid == geteuid() &&
                    before.st_nlink == 1 && (before.st_mode & 07777) == 0600 &&
                    before.st_size == (off_t)length;
        ssize_t count = valid ? pread(fd, buffer, length, 0) : -1;
        valid = valid && count == (ssize_t)length && !CRYPTO_memcmp(buffer, bytes, length) &&
                !fstat(fd, &after) && before.st_dev == after.st_dev &&
                before.st_ino == after.st_ino && before.st_size == after.st_size &&
                before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
                before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
                before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
        OPENSSL_cleanse(buffer, sizeof(buffer));
        close(fd);
        return valid ? 0 : -1;
    }
    if (errno != ENOENT || !create)
        return -1;
    fd =
        openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0)
        return -1;
    struct stat info;
    int valid = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                info.st_nlink == 1 && (info.st_mode & 07777) == 0600;
    size_t sent = 0;
    while (valid && sent < length) {
        ssize_t count = write(fd, bytes + sent, length - sent);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            valid = 0;
        else
            sent += (size_t)count;
    }
    valid = valid && !fsync(fd);
    close(fd);
    return valid && !fsync(root) ? 0 : -1;
}

static int exact_descriptor(int fd, const void *bytes, size_t length) {
    if (!bytes || !length || length > 32768)
        return -1;
    struct stat before, after;
    int flags = fcntl(fd, F_GETFL);
    unsigned char buffer[32768];
    int valid =
        fd >= 3 && flags >= 0 && (flags & O_ACCMODE) == O_RDONLY && !fstat(fd, &before) &&
        S_ISREG(before.st_mode) && before.st_uid == geteuid() && (before.st_mode & 07777) == 0600 &&
        before.st_nlink == 1 && before.st_size == (off_t)length &&
        pread(fd, buffer, length, 0) == (ssize_t)length && !CRYPTO_memcmp(buffer, bytes, length) &&
        !fstat(fd, &after) && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size && before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
    OPENSSL_cleanse(buffer, sizeof(buffer));
    return valid ? 0 : -1;
}
static int private_directory(int fd, struct stat *info) {
    return fd >= 3 && !fstat(fd, info) && S_ISDIR(info->st_mode) && info->st_uid == geteuid() &&
           (info->st_mode & 07777) == 0700;
}
static int identifier(const char *text) { return text && gstd_identifier(text, strnlen(text, 65)); }
static int inventory(int fd, const char *const *allowed, size_t count) {
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (copy < 3)
        return -1;
    DIR *directory = fdopendir(copy);
    if (!directory) {
        close(copy);
        return -1;
    }
    rewinddir(directory);
    struct dirent *entry;
    int valid = 1;
    errno = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        int known = 0;
        for (size_t i = 0; i < count; i++)
            known |= !strcmp(entry->d_name, allowed[i]);
        if (!known) {
            valid = 0;
            break;
        }
    }
    if (errno)
        valid = 0;
    closedir(directory);
    return valid ? 0 : -1;
}
static int request_valid(const gstd_bootstrap_request *r, const gstd_oidc_ports *ports,
                         const int files[4], struct stat *root) {
    struct rlimit core;
    if (!r || !files || !ports || gstd_oidc_ports_check(ports) || !geteuid() ||
        r->control_owner <= 0 || r->control_owner == getpid() || prctl(PR_GET_DUMPABLE) != 0 ||
        getrlimit(RLIMIT_CORE, &core) || core.rlim_cur || !identifier(r->deployment_id) ||
        !identifier(r->operation_id) || !r->store_id || strnlen(r->store_id, 33) != 32 ||
        !r->generation || r->generation > GSTD_SAFE_INTEGER ||
        gstd_oidc_core_manifest_check(r->manifest) ||
        !json_object_get(r->manifest, "oidcProfileDigest") || !private_directory(r->scope_fd, root))
        return 0;
    for (size_t i = 0; i < 32; i++)
        if (!((r->store_id[i] >= '0' && r->store_id[i] <= '9') ||
              (r->store_id[i] >= 'a' && r->store_id[i] <= 'f')))
            return 0;
    for (size_t i = 0; i < 4; i++)
        if (files[i] != -1)
            return 0;
#if defined(__aarch64__)
    const char *architecture = "arm64";
#elif defined(__x86_64__)
    const char *architecture = "amd64";
#else
    return 0;
#endif
    const json_t *actual =
        json_object_get(json_object_get(r->manifest, "platform"), "architecture");
    return json_is_string(actual) && !strcmp(json_string_value(actual), architecture);
}
static int state(int root, int *exists) {
    static const char *const allowed[] = {
        "prepare.lock", "owner.json", "prepared.json", "material.binding", "hydra", "kratos", "ui"};
    struct stat info;
    if (inventory(root, allowed, sizeof(allowed) / sizeof(allowed[0])))
        return -1;
    *exists = !fstatat(root, "owner.json", &info, AT_SYMLINK_NOFOLLOW);
    if (!*exists && errno != ENOENT)
        return -1;
    int complete = !fstatat(root, "prepared.json", &info, AT_SYMLINK_NOFOLLOW);
    if ((!complete && errno != ENOENT) || *exists != complete ||
        (!*exists && inventory(root, NULL, 0)))
        return -1;
    return 0;
}
static int child_directory(int root, const char *name, int create, dev_t device) {
    if (create && mkdirat(root, name, 0700))
        return -1;
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    int fd = (int)syscall(SYS_openat2, root, name, &how, sizeof(how));
    struct stat info;
    if (!private_directory(fd, &info) || info.st_dev != device) {
        if (fd >= 3)
            close(fd);
        return -1;
    }
    return fd;
}
/* Private fingerprint binds the entire secret family to its owner. It never
 * crosses IPC, enters operation metadata, or gets mounted into a workload. */
static int material_binding(const gstd_bootstrap_request *r, const gstd_credential *material,
                            char hash[65]) {
    json_t *value = gstd_json_parse(material->bytes, material->length);
    char *canonical = value ? gstd_json_dump(value) : NULL;
    gstd_oidc_config_clear(value);
    json_decref(value);
    size_t length = canonical ? strlen(canonical) : 0;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned char digest[32] = {0};
    unsigned size = 0;
    static const char domain[] = "gitstored-oidc-material-binding-v1\n";
    int valid = canonical && context && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
                EVP_DigestUpdate(context, domain, sizeof(domain) - 1) == 1 &&
                EVP_DigestUpdate(context, r->store_id, 32) == 1 &&
                EVP_DigestUpdate(context, "\n", 1) == 1 &&
                EVP_DigestUpdate(context, r->deployment_id, strlen(r->deployment_id)) == 1 &&
                EVP_DigestUpdate(context, "\n", 1) == 1 &&
                EVP_DigestUpdate(context, canonical, length) == 1 &&
                EVP_DigestFinal_ex(context, digest, &size) == 1 && size == 32;
    if (valid) {
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < 32; i++) {
            hash[2 * i] = digits[digest[i] >> 4];
            hash[2 * i + 1] = digits[digest[i] & 15];
        }
        hash[64] = 0;
    }
    OPENSSL_cleanse(digest, sizeof(digest));
    if (canonical)
        OPENSSL_cleanse(canonical, length);
    free(canonical);
    EVP_MD_CTX_free(context);
    return valid ? 0 : -1;
}
static json_t *public_marker(int root, const char *name) {
    int fd = openat(root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat before, after;
    unsigned char bytes[4096];
    json_t *result = NULL;
    if (fd >= 3 && !fstat(fd, &before) && S_ISREG(before.st_mode) && before.st_uid == geteuid() &&
        (before.st_mode & 07777) == 0600 && before.st_nlink == 1 && before.st_size > 0 &&
        before.st_size <= (off_t)sizeof(bytes) &&
        pread(fd, bytes, (size_t)before.st_size, 0) == before.st_size && !fstat(fd, &after) &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec)
        result = gstd_json_parse(bytes, (size_t)before.st_size);
    OPENSSL_cleanse(bytes, sizeof(bytes));
    if (fd >= 3)
        close(fd);
    return result;
}
static int text(const json_t *value, const char *key, const char *expected) {
    const json_t *field = json_object_get(value, key);
    return expected && json_is_string(field) && json_string_length(field) == strlen(expected) &&
           !memcmp(json_string_value(field), expected, strlen(expected));
}
static int previous_binding(const gstd_bootstrap_request *r, const gstd_oidc_ports *ports,
                            const gstd_credential *material, int previous) {
    struct stat directory;
    int exists = 0;
    if (!private_directory(previous, &directory) || state(previous, &exists) || !exists)
        return -1;
    json_t *owner = public_marker(previous, "owner.json"),
           *prepared = public_marker(previous, "prepared.json");
    char device[32], inode[32], hash[65] = {0};
    snprintf(device, sizeof(device), "%llu", (unsigned long long)directory.st_dev);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)directory.st_ino);
    json_t *tuple = json_pack("[i,i,i,i,i]", (int)ports->issuer, (int)ports->bridge,
                              (int)ports->identity, (int)ports->ui, (int)ports->mail);
    const json_t *generation = json_object_get(owner, "generation");
    const char *operation = json_string_value(json_object_get(owner, "operationId"));
    int valid =
        owner && prepared && tuple && json_equal(owner, prepared) &&
        json_object_size(owner) == 10 && json_is_integer(json_object_get(owner, "schemaVersion")) &&
        json_integer_value(json_object_get(owner, "schemaVersion")) == 1 &&
        text(owner, "kind", "oidc-provider-configuration") && text(owner, "storeId", r->store_id) &&
        text(owner, "deploymentId", r->deployment_id) &&
        text(owner, "profileDigest",
             json_string_value(json_object_get(r->manifest, "profileDigest"))) &&
        text(owner, "scopeDevice", device) && text(owner, "scopeInode", inode) &&
        json_equal(tuple, json_object_get(owner, "ports")) && identifier(operation) &&
        json_string_length(json_object_get(owner, "operationId")) == strlen(operation) &&
        json_is_integer(generation) && json_integer_value(generation) > 0 &&
        ((uint64_t)json_integer_value(generation) < r->generation ||
         ((uint64_t)json_integer_value(generation) == r->generation &&
          !strcmp(operation, r->operation_id))) &&
        !material_binding(r, material, hash) &&
        !exact_file(previous, "material.binding", hash, 64, 0);
    OPENSSL_cleanse(hash, sizeof(hash));
    gstd_oidc_config_clear(owner);
    gstd_oidc_config_clear(prepared);
    json_decref(owner);
    json_decref(prepared);
    json_decref(tuple);
    return valid ? 0 : -1;
}

int gstd_oidc_prepare(const gstd_bootstrap_request *r, const gstd_oidc_ports *ports,
                      const gstd_credential *material, int files[4]) {
    struct stat root, info, named;
    int exists = 0;
    if (!request_valid(r, ports, files, &root) || gstd_oidc_material_check(material) ||
        state(r->scope_fd, &exists))
        return -1;
    int lock = openat(
        r->scope_fd, "prepare.lock",
        O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (exists ? 0 : O_CREAT | O_EXCL), 0600);
    if (lock < 3)
        return -1;
    int valid = !fstat(lock, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                info.st_nlink == 1 && !info.st_size && (info.st_mode & 07777) == 0600 &&
                !flock(lock, LOCK_EX | LOCK_NB) &&
                !fstatat(r->scope_fd, "prepare.lock", &named, AT_SYMLINK_NOFOLLOW) &&
                named.st_dev == info.st_dev && named.st_ino == info.st_ino;
    char binding[65] = {0};
    int result = -1, directories[3] = {-1, -1, -1};
    char device[32], inode[32];
    snprintf(device, sizeof(device), "%llu", (unsigned long long)root.st_dev);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)root.st_ino);
    json_t *tuple = json_pack("[i,i,i,i,i]", (int)ports->issuer, (int)ports->bridge,
                              (int)ports->identity, (int)ports->ui, (int)ports->mail);
    json_t *owner = tuple
                        ? json_pack("{s:i,s:s,s:s,s:s,s:s,s:I,s:O,s:o,s:s,s:s}", "schemaVersion", 1,
                                    "kind", "oidc-provider-configuration", "storeId", r->store_id,
                                    "deploymentId", r->deployment_id, "operationId",
                                    r->operation_id, "generation", (json_int_t)r->generation,
                                    "profileDigest", json_object_get(r->manifest, "profileDigest"),
                                    "ports", tuple, "scopeDevice", device, "scopeInode", inode)
                        : NULL;
    char *owner_bytes = owner ? gstd_json_dump(owner) : NULL;
    json_decref(owner);
    json_t *provider[2] = {NULL, NULL};
    char *bytes[2] = {NULL, NULL};
    gstd_credential ui = {0};
    if (!valid || !owner_bytes || material_binding(r, material, binding) ||
        exact_file(r->scope_fd, "owner.json", owner_bytes, strlen(owner_bytes), !exists))
        goto done;
    if (exact_file(r->scope_fd, "material.binding", binding, 64, !exists))
        goto done;
    const char *names[] = {"hydra", "kratos", "ui"};
    for (size_t i = 0; i < 3; i++) {
        directories[i] = child_directory(r->scope_fd, names[i], !exists, root.st_dev);
        if (directories[i] < 3)
            goto done;
    }
    static const char *const provider_files[] = {"provider.json"};
    static const char *const kratos_files[] = {"provider.json", "identity.schema.json"};
    static const char *const ui_files[] = {"ui.env"};
    if (inventory(directories[0], provider_files, 1) ||
        inventory(directories[1], kratos_files, 2) || inventory(directories[2], ui_files, 1))
        goto done;
    for (size_t i = 0; i < 2; i++) {
        provider[i] = gstd_oidc_provider_config((gstd_oidc_provider)i, ports, material);
        bytes[i] = provider[i] ? gstd_json_dump(provider[i]) : NULL;
        if (!bytes[i] ||
            exact_file(directories[i], "provider.json", bytes[i], strlen(bytes[i]), !exists))
            goto done;
    }
    if (exact_file(directories[1], "identity.schema.json", oidc_identity_schema,
                   sizeof(oidc_identity_schema) - 1, !exists) ||
        gstd_oidc_ui_environment(ports, material, &ui) ||
        exact_file(directories[2], "ui.env", (const char *)ui.bytes, ui.length, !exists) ||
        exact_file(r->scope_fd, "prepared.json", owner_bytes, strlen(owner_bytes), !exists))
        goto done;
    const int locations[] = {0, 1, 1, 2};
    const char *leaves[] = {"provider.json", "provider.json", "identity.schema.json", "ui.env"};
    const void *expected_bytes[] = {bytes[0], bytes[1], oidc_identity_schema, ui.bytes};
    const size_t expected_lengths[] = {strlen(bytes[0]), strlen(bytes[1]),
                                       sizeof(oidc_identity_schema) - 1, ui.length};
    for (size_t i = 0; i < 4; i++) {
        files[i] = openat(directories[locations[i]], leaves[i],
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (files[i] < 3 || exact_descriptor(files[i], expected_bytes[i], expected_lengths[i]))
            goto done;
    }
    result = 0;
done:
    OPENSSL_cleanse(binding, sizeof(binding));
    if (result)
        for (size_t i = 0; i < 4; i++) {
            if (files[i] >= 3)
                close(files[i]);
            files[i] = -1;
        }
    for (size_t i = 0; i < 3; i++)
        if (directories[i] >= 3)
            close(directories[i]);
    for (size_t i = 0; i < 2; i++) {
        if (bytes[i])
            OPENSSL_cleanse(bytes[i], strlen(bytes[i]));
        free(bytes[i]);
        gstd_oidc_config_clear(provider[i]);
        json_decref(provider[i]);
    }
    gstd_credential_clear(&ui);
    free(owner_bytes);
    close(lock);
    return result;
}
int gstd_oidc_provision(const gstd_bootstrap_request *r, const gstd_oidc_ports *ports, int broker,
                        const json_t *source, int initial, int previous, int files[4]) {
    struct stat root;
    int exists = 0;
    if (!request_valid(r, ports, files, &root) || broker < 3 || (initial != 0 && initial != 1) ||
        (source && !json_is_object(source)) || (previous != -1 && previous < 3) ||
        state(r->scope_fd, &exists))
        return -1;
    json_t *location = source ? json_deep_copy(source) : json_pack("{s:s}", "kind", "os-store");
    json_t *message = location ? json_pack("{s:{s:s,s:s,s:s,s:s},s:o}", "identity", "installation",
                                           r->store_id, "issuer", "gitstore-local/reference-oidc/1",
                                           "account", r->deployment_id, "purpose",
                                           "oidc-provider-material", "source", location)
                               : NULL;
    char *encoded = message ? gstd_json_dump(message) : NULL;
    gstd_credential material = {0}, candidate = {0};
    int result = -1;
    if (!encoded)
        goto done;
    int status = gstd_credential_run(broker, (const unsigned char *)encoded, strlen(encoded), 5000,
                                     &material);
    free(encoded);
    encoded = NULL;
    if (status == 1) {
        if (!initial || exists || previous >= 3 ||
            gstd_oidc_material_generate(r->control_owner, &candidate) ||
            json_object_set_new(message, "createIfAbsent",
                                json_stringn((const char *)candidate.bytes, candidate.length)))
            goto done;
        encoded = gstd_json_dump(message);
        if (!encoded)
            goto done;
        status = gstd_credential_run(broker, (const unsigned char *)encoded, strlen(encoded), 5000,
                                     &material);
        if (status) {
            result = -2;
            goto done;
        }
    }
    if (status || gstd_oidc_material_check(&material))
        goto done;
    if (previous >= 3 && previous_binding(r, ports, &material, previous))
        goto done;
    result = gstd_oidc_prepare(r, ports, &material, files);
done:
    if (encoded)
        OPENSSL_cleanse(encoded, strlen(encoded));
    free(encoded);
    gstd_credential_clear(&material);
    gstd_credential_clear(&candidate);
    gstd_oidc_config_clear(message);
    json_decref(message);
    return result;
}
#else
int gstd_oidc_prepare(const gstd_bootstrap_request *r, const gstd_oidc_ports *ports,
                      const gstd_credential *material, int files[4]) {
    (void)r;
    (void)ports;
    (void)material;
    (void)files;
    return -1;
}
int gstd_oidc_provision(const gstd_bootstrap_request *r, const gstd_oidc_ports *ports, int broker,
                        const json_t *source, int initial, int previous, int files[4]) {
    (void)r;
    (void)ports;
    (void)broker;
    (void)source;
    (void)initial;
    (void)previous;
    (void)files;
    return -1;
}
#endif
