#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "scope_recovery.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "network_worker.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#endif
static int equal(const json_t *v, const char *key, const char *s) {
    json_t *field = json_object_get(v, key);
    return s && json_is_string(field) && json_string_length(field) == strlen(s) &&
           !memcmp(json_string_value(field), s, strlen(s));
}
static int number(const json_t *v, const char *key, uint64_t n) {
    char bytes[32];
    snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)n);
    return equal(v, key, bytes);
}
static int ordinary(int fd, struct stat *s, int private) {
    return fd >= 3 && !fstat(fd, s) && S_ISDIR(s->st_mode) && s->st_uid == geteuid() &&
           (private ? (s->st_mode & 07777) == 0700 : !(s->st_mode & 0022));
}
static int named(int parent, const char *name, const struct stat *expected) {
    struct stat s;
    return !fstatat(parent, name, &s, AT_SYMLINK_NOFOLLOW) && S_ISDIR(s.st_mode) &&
           s.st_dev == expected->st_dev && s.st_ino == expected->st_ino;
}
static json_t *owner_file(int root) {
    int fd = openat(root, "owner.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat before, after;
    json_t *v = NULL;
    if (fd >= 3 && !fstat(fd, &before) && S_ISREG(before.st_mode) && before.st_uid == geteuid() &&
        (before.st_mode & 07777) == 0600 && before.st_nlink == 1 && before.st_size > 0 &&
        before.st_size <= 4096) {
        unsigned char bytes[4097];
        ssize_t n = pread(fd, bytes, sizeof(bytes), 0);
        if (n == before.st_size && !fstat(fd, &after) && before.st_dev == after.st_dev &&
            before.st_ino == after.st_ino && before.st_size == after.st_size &&
            before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
            before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
            before.st_ctim.tv_nsec == after.st_ctim.tv_nsec)
            v = gstd_json_parse(bytes, (size_t)n);
    }
    if (fd >= 0)
        close(fd);
    return v;
}
int gstd_scope_reopen(int parent, const gstd_filesystem_input *input,
                      gstd_filesystem_reply *reply) {
    if (!input || !reply || gstd_asset_manifest_check(input->manifest) || !input->scope_expected ||
        !input->resource_id || !strcmp(input->resource_id, ".") ||
        !strcmp(input->resource_id, "..") ||
        !gstd_identifier(input->resource_id, strnlen(input->resource_id, 65)) ||
        !input->generation || input->generation > GSTD_SAFE_INTEGER || !input->resource_kind ||
        (strcmp(input->resource_kind, "configuration") &&
         strcmp(input->resource_kind, "workload") && strcmp(input->resource_kind, "network")))
        return -1;
    int workload = !strcmp(input->resource_kind, "workload");
    int network = !strcmp(input->resource_kind, "network");
    struct stat top, wrapper, data, bundle, rootfs;
    if (!ordinary(parent, &top, 1))
        return -1;
    int fds[4] = {-1, -1, -1, -1};
    fds[0] = openat(parent, input->resource_id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int valid = ordinary(fds[0], &wrapper, 1);
    if (valid)
        fds[1] = openat(fds[0], workload ? "execution" : "data",
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    valid = valid && ordinary(fds[1], &data, 1);
    const json_t *expected = input->scope_expected;
    valid = valid && json_integer_value(json_object_get(expected, "schemaVersion")) == 1 &&
            equal(expected, "storeId", input->store_id) &&
            equal(expected, "operationId", input->operation_id) &&
            json_equal(json_object_get(expected, "profileDigest"),
                       json_object_get(input->manifest, "profileDigest"));
    if (!network && (json_object_get(input->manifest, "datastore") ||
                     json_object_get(input->manifest, "baseProfileDigest")))
        valid =
            valid && json_equal(json_object_get(expected, "executionManifest"), input->manifest);
    if (network) {
        /* Network ledger binds the actual data inode and worker identity. This
         * action returns only that data FD; it grants no wrapper cleanup or
         * process control. RECOVER_NETWORK separately proves the live owner. */
        gstd_oidc_ports ports = {0};
        const json_t *selected = json_object_get(expected, "oidcPorts");
        gstd_network_worker_request request = {.manifest = input->manifest};
        unsigned roles = 0;
        if (selected) {
            valid = valid && !gstd_network_oidc_ports_parse(selected, &ports);
            request.oidc_ports = &ports;
        }
        valid = valid && !gstd_network_request_roles(&request, &roles) &&
                json_is_integer(json_object_get(expected, "roles")) &&
                json_integer_value(json_object_get(expected, "roles")) == roles &&
                json_object_size(expected) == (selected ? 12 : 11) &&
                equal(expected, "resourceId", input->resource_id) &&
                number(expected, "scopeDevice", data.st_dev) &&
                number(expected, "scopeInode", data.st_ino);
    } else {
        valid = valid && equal(expected, "kind", input->resource_kind) &&
                equal(expected, "deploymentId", input->deployment_id) &&
                json_is_integer(json_object_get(expected, "generation")) &&
                json_integer_value(json_object_get(expected, "generation")) ==
                    (json_int_t)input->generation &&
                number(expected, "scopeDevice", wrapper.st_dev) &&
                number(expected, "scopeInode", wrapper.st_ino) &&
                number(expected, "dataDevice", data.st_dev) &&
                number(expected, "dataInode", data.st_ino);
    }
    json_t *owner = valid ? owner_file(fds[0]) : NULL;
    valid =
        valid && json_is_object(owner) && json_object_size(owner) == 11 &&
        json_integer_value(json_object_get(owner, "schemaVersion")) == 1 &&
        equal(owner, "storeId", input->store_id) &&
        equal(owner, "resourceId", input->resource_id) &&
        equal(owner, "operationId", input->operation_id) &&
        equal(owner, "deploymentId", input->deployment_id) &&
        equal(owner, "kind", input->resource_kind) &&
        json_is_integer(json_object_get(owner, "generation")) &&
        json_integer_value(json_object_get(owner, "generation")) == (json_int_t)input->generation &&
        number(owner, "device", wrapper.st_dev) && number(owner, "inode", wrapper.st_ino) &&
        number(owner, "dataDevice", data.st_dev) && number(owner, "dataInode", data.st_ino);
    json_decref(owner);
    if (valid && workload) {
        fds[2] = openat(fds[0], "bundle", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        fds[3] = openat(fds[1], "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        json_t *execution = json_object_get(expected, "execution");
        valid = ordinary(fds[2], &bundle, 1) && ordinary(fds[3], &rootfs, 0) &&
                json_is_object(execution) && json_object_size(execution) == 4 &&
                number(execution, "bundleDevice", bundle.st_dev) &&
                number(execution, "bundleInode", bundle.st_ino) &&
                number(execution, "rootfsDevice", rootfs.st_dev) &&
                number(execution, "rootfsInode", rootfs.st_ino) &&
                named(fds[0], "bundle", &bundle) && named(fds[1], "rootfs", &rootfs);
    }
    valid = valid && named(parent, input->resource_id, &wrapper) &&
            named(fds[0], workload ? "execution" : "data", &data);
    if (!valid) {
        for (unsigned i = 0; i < 4; i++)
            if (fds[i] >= 0)
                close(fds[i]);
        return -1;
    }
    if (network) {
        close(fds[0]);
        reply->descriptors[0] = fds[1];
        reply->descriptor_count = 1;
        reply->scope_device = data.st_dev;
        reply->scope_inode = data.st_ino;
        return 0;
    }
    memcpy(reply->descriptors, fds, sizeof(int) * (workload ? 4 : 2));
    reply->descriptor_count = workload ? 4 : 2;
    reply->scope_device = wrapper.st_dev;
    reply->scope_inode = wrapper.st_ino;
    return 0;
}
