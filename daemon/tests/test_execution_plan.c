#define _GNU_SOURCE 1
#include "execution_plan.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    int result = 0, directory = -1;
    char *root = gstd_test_dir();
    json_t *components = json_pack("[s]", "api"), *manifest = NULL, *spec = NULL;
    gstd_execution_inputs input = {.component = "api", .rootfs_fd = -1};
    for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
        input.role_fds[i] = -1;
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    CHECK(root && components);
    manifest = gstd_asset_manifest(components, architecture);
    CHECK(manifest);
    input.manifest = manifest;
    CHECK(!gstd_execution_build(&input));
    directory = open(root, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    CHECK(directory >= 3 && !mkdirat(directory, "rootfs", 0700));
    input.rootfs_fd = openat(directory, "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(input.rootfs_fd >= 3);
    for (unsigned i = 0; i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "role-%u", i);
        int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        CHECK(fd >= 3);
        int valid = write(fd, "synthetic-private-config", 24) == 24 && !fsync(fd);
        close(fd);
        CHECK(valid);
        input.role_fds[i] = openat(directory, name, O_RDONLY | O_CLOEXEC);
        CHECK(input.role_fds[i] >= 3);
    }
#ifdef __linux__
    spec = gstd_execution_build(&input);
    CHECK(spec);
    json_t *process = json_object_get(spec, "process"), *args = json_object_get(process, "args");
    CHECK(json_array_size(args) == 5 &&
          !strcmp(json_string_value(json_array_get(args, 0)), "/app/api"));
    CHECK(json_is_true(json_object_get(process, "noNewPrivileges")));
    CHECK(json_is_true(json_object_get(json_object_get(spec, "root"), "readonly")));
    CHECK(json_array_size(json_object_get(json_object_get(spec, "linux"), "namespaces")) == 6);
    json_t *mounts = json_object_get(spec, "mounts");
    CHECK(json_array_size(mounts) == 8);
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(mounts, 1), "destination")),
                  "/run"));
    char *bytes = gstd_json_dump(spec);
    CHECK(bytes);
    int private =
        !strstr(bytes, "synthetic-private-config") && !strstr(bytes, "controller-signing");
    free(bytes);
    CHECK(private);
    int original = input.role_fds[GSTD_MOUNT_OVERLAY];
    input.role_fds[GSTD_MOUNT_OVERLAY] = input.role_fds[GSTD_MOUNT_BASE];
    CHECK(!gstd_execution_build(&input));
    input.role_fds[GSTD_MOUNT_OVERLAY] = original;
    CHECK(!fchmod(original, 0644));
    CHECK(!gstd_execution_build(&input));
    CHECK(!fchmod(original, 0600));
    CHECK(!linkat(directory, "role-1", directory, "linked-role", 0));
    CHECK(!gstd_execution_build(&input));
    CHECK(!unlinkat(directory, "linked-role", 0));
    input.role_fds[GSTD_MOUNT_SIGNING_RECORD] = input.role_fds[GSTD_MOUNT_BASE];
    CHECK(!gstd_execution_build(&input));
    input.role_fds[GSTD_MOUNT_SIGNING_RECORD] = -1;
    input.component = "git";
    CHECK(!gstd_execution_build(&input));
    input.component = "/bin/sh";
    CHECK(!gstd_execution_build(&input));
    input.component = "api";
    CHECK(!json_object_set_new(manifest, "profileVersion", json_string("changed")));
    CHECK(!gstd_execution_build(&input));
#else
    CHECK(!gstd_execution_build(&input));
#endif
done:
    for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
        if (input.role_fds[i] >= 0)
            close(input.role_fds[i]);
    if (input.rootfs_fd >= 0)
        close(input.rootfs_fd);
    if (directory >= 0)
        close(directory);
    json_decref(spec);
    json_decref(manifest);
    json_decref(components);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
