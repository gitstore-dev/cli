#define _GNU_SOURCE 1
#include "execution_plan.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "Scylla plan line %d: %s\n", __LINE__, #x);                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    int result = 1, directory = -1, data = -1;
    char *root = gstd_test_dir();
    json_t *components = json_pack("[s,s]", "api", "git"), *manifest = NULL, *spec = NULL;
    gstd_execution_inputs input = {.component = "scylladb", .rootfs_fd = -1};
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++)
        input.role_fds[i] = -1;
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    CHECK(root && components);
    manifest = gstd_asset_manifest_for_datastore(components, architecture, "scylladb");
    CHECK(manifest);
    input.manifest = manifest;
    directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(directory >= 3 && !mkdirat(directory, "rootfs", 0700) &&
          !mkdirat(directory, "data", 0700));
    input.rootfs_fd = openat(directory, "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    data = openat(directory, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(input.rootfs_fd >= 3 && data >= 3);
    CHECK(!gstd_execution_build(&input));
    input.role_fds[GSTD_MOUNT_DATASTORE] = data;
#ifdef __linux__
    spec = gstd_execution_build(&input);
    CHECK(spec);
    json_t *process = json_object_get(spec, "process"), *linux = json_object_get(spec, "linux"),
           *args = json_object_get(process, "args");
    CHECK(json_array_size(args) == 14);
    CHECK(!strcmp(json_string_value(json_array_get(args, 0)), "/usr/bin/scylla"));
    CHECK(!strcmp(json_string_value(json_array_get(args, 2)),
                  "--options-file=/etc/scylla/scylla.yaml"));
    CHECK(!strcmp(json_string_value(json_object_get(process, "cwd")), "/"));
    CHECK(json_is_true(json_object_get(process, "noNewPrivileges")));
    CHECK(json_integer_value(json_object_get(
              json_object_get(json_object_get(linux, "resources"), "memory"), "limit")) ==
          2147483648LL);
#ifndef GITSTORED_INSTRUMENTED
    CHECK(json_integer_value(json_object_get(json_array_get(json_object_get(process, "rlimits"), 0),
                                             "soft")) == ((json_int_t)1 << 46));
#endif
    json_t *mounts = json_object_get(spec, "mounts");
    int found = 0;
    for (size_t i = 0; i < json_array_size(mounts); i++) {
        json_t *mount = json_array_get(mounts, i);
        const char *destination = json_string_value(json_object_get(mount, "destination"));
        if (destination && !strcmp(destination, "/var/lib/scylla")) {
            found++;
            int noexec = 0, rw = 0;
            json_t *options = json_object_get(mount, "options");
            for (size_t j = 0; j < json_array_size(options); j++) {
                const char *option = json_string_value(json_array_get(options, j));
                if (option && !strcmp(option, "noexec"))
                    noexec = 1;
                if (option && !strcmp(option, "rw"))
                    rw = 1;
            }
            CHECK(noexec && rw);
        }
    }
    CHECK(found == 1);
    input.role_fds[GSTD_MOUNT_BASE] = data;
    CHECK(!gstd_execution_build(&input));
    input.role_fds[GSTD_MOUNT_BASE] = -1;
    input.role_fds[GSTD_MOUNT_DATASTORE] = input.rootfs_fd;
    CHECK(!gstd_execution_build(&input));
    input.role_fds[GSTD_MOUNT_DATASTORE] = data;
    CHECK(!fchmod(data, 0755) && !gstd_execution_build(&input));
    CHECK(!fchmod(data, 0700));
    input.role_fds[GSTD_MOUNT_DATASTORE] = -1;
    json_decref(spec);
    spec = NULL;
    for (gstd_execution_purpose purpose = GSTD_EXEC_SCYLLA_READY;
         purpose <= GSTD_EXEC_SCHEMA_HISTORY; purpose++) {
        input.purpose = purpose;
        input.component = purpose == GSTD_EXEC_SCHEMA_MIGRATE ? "api" : "scylladb";
        spec = gstd_execution_build(&input);
        CHECK(spec);
        args = json_object_get(json_object_get(spec, "process"), "args");
        CHECK(json_array_size(args) == (purpose == GSTD_EXEC_SCHEMA_MIGRATE ? 6 : 9));
        if (purpose == GSTD_EXEC_SCHEMA_INSPECT)
            CHECK(strstr(json_string_value(json_array_get(args, 8)), "system_schema.keyspaces") &&
                  strstr(json_string_value(json_array_get(args, 8)), "system_schema.tables"));
        if (purpose == GSTD_EXEC_SCHEMA_HISTORY)
            CHECK(!strcmp(json_string_value(json_array_get(args, 8)),
                          "SELECT JSON name, checksum, done, end_time FROM gitstore.gocqlx_migrate "
                          "LIMIT 13;"));
        CHECK(!strcmp(json_string_value(json_array_get(args, 0)),
                      purpose == GSTD_EXEC_SCHEMA_MIGRATE ? "/app/gitctl" : "/usr/bin/cqlsh"));
        CHECK(json_integer_value(json_object_get(
                  json_object_get(json_object_get(json_object_get(spec, "linux"), "resources"),
                                  "memory"),
                  "limit")) == 536870912);
        CHECK(json_array_size(json_object_get(spec, "mounts")) == 4);
        input.role_fds[GSTD_MOUNT_DATASTORE] = data;
        CHECK(!gstd_execution_build(&input));
        input.role_fds[GSTD_MOUNT_DATASTORE] = -1;
        input.component = purpose == GSTD_EXEC_SCHEMA_MIGRATE ? "scylladb" : "api";
        CHECK(!gstd_execution_build(&input));
        json_decref(spec);
        spec = NULL;
    }
    input.purpose = (gstd_execution_purpose)99;
    CHECK(!gstd_execution_build(&input));
#else
    CHECK(!gstd_execution_build(&input));
#endif
    result = 0;
done:
    if (data >= 0) {
        (void)fchmod(data, 0700);
        close(data);
    }
    if (input.rootfs_fd >= 0)
        close(input.rootfs_fd);
    if (directory >= 0)
        close(directory);
    json_decref(spec);
    json_decref(manifest);
    json_decref(components);
    if (root)
        gstd_test_remove(root);
    return result;
}
