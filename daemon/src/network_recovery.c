#define _GNU_SOURCE 1
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "network_worker.h"
#if defined(__linux__) && defined(GITSTORED_SLIRP)
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static int string(const json_t *value, const char *name, const char *expected) {
    const json_t *field = json_object_get(value, name);
    return json_is_string(field) && json_string_length(field) == strlen(expected) &&
           !memcmp(json_string_value(field), expected, strlen(expected));
}
static int integer(const json_t *value, const char *name, uint64_t maximum, uint64_t *result) {
    const json_t *field = json_object_get(value, name);
    if (!json_is_integer(field) || json_integer_value(field) < 0 ||
        (uint64_t)json_integer_value(field) > maximum)
        return -1;
    *result = (uint64_t)json_integer_value(field);
    return 0;
}
static int decimal(const json_t *value, const char *name, uint64_t *result) {
    const json_t *field = json_object_get(value, name);
    const char *bytes = json_string_value(field);
    if (!bytes || !*bytes || strlen(bytes) > 20 || json_string_length(field) != strlen(bytes))
        return -1;
    for (size_t i = 0; bytes[i]; i++)
        if (bytes[i] < '0' || bytes[i] > '9' || (i && bytes[0] == '0'))
            return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long number = strtoull(bytes, &end, 10);
    if (errno || !end || *end)
        return -1;
    *result = (uint64_t)number;
    return 0;
}
static int private_file(int fd, struct stat *info) {
    return fd >= 0 && !fstat(fd, info) && S_ISREG(info->st_mode) && info->st_uid == geteuid() &&
           info->st_nlink == 1 && (info->st_mode & 07777) == 0600;
}
static json_t *record(int scope, const char *name) {
    int fd = openat(scope, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat before, after;
    json_t *value = NULL;
    if (private_file(fd, &before) && before.st_size > 0 && before.st_size <= 4096) {
        unsigned char bytes[4097];
        ssize_t count = read(fd, bytes, sizeof(bytes));
        if (count == before.st_size && !fstat(fd, &after) && before.st_dev == after.st_dev &&
            before.st_ino == after.st_ino && before.st_size == after.st_size &&
            before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
            before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
            before.st_ctim.tv_nsec == after.st_ctim.tv_nsec)
            value = gstd_json_parse(bytes, (size_t)count);
    }
    if (fd >= 0)
        close(fd);
    return value;
}
static int active(const gstd_network_worker *worker) {
    struct pollfd exited = {worker->pid_fd, POLLIN, 0};
    uint64_t stamp = 0;
    return poll(&exited, 1, 0) == 0 && !gstd_native_process_start(worker->pid, &stamp) &&
           stamp == worker->start;
}
static int keeper_namespace(pid_t pid, const char *kind) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/%ld/ns/%s", (long)pid, kind);
    return open(path, O_RDONLY | O_CLOEXEC);
}
static int worker_lock(int scope, pid_t pid) {
    int fd = openat(scope, "worker.lock", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat named, held;
    int valid = private_file(fd, &named) && !named.st_size;
    if (fd >= 0)
        close(fd);
    char path[96];
    snprintf(path, sizeof(path), "/proc/%ld/fd/5", (long)pid);
    return valid && !stat(path, &held) && S_ISREG(held.st_mode) && held.st_dev == named.st_dev &&
           held.st_ino == named.st_ino;
}
static int listener(pid_t pid, int descriptor, uint64_t inode, uint16_t port) {
    char path[96];
    struct stat info;
    snprintf(path, sizeof(path), "/proc/%ld/fd/%d", (long)pid, descriptor);
    if (stat(path, &info) || !S_ISSOCK(info.st_mode) || (uint64_t)info.st_ino != inode)
        return 0;
    snprintf(path, sizeof(path), "/proc/%ld/net/tcp", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return 0;
    FILE *stream = fdopen(fd, "r");
    if (!stream) {
        close(fd);
        return 0;
    }
    char line[512];
    int found = 0;
    for (unsigned row = 0; row < 4096 && fgets(line, sizeof(line), stream); row++) {
        if (!strchr(line, '\n'))
            break;
        unsigned local, local_port, remote, remote_port, state;
        unsigned long long actual;
        if (sscanf(line, "%*u: %x:%x %x:%x %x %*s %*s %*s %*u %*u %llu", &local, &local_port,
                   &remote, &remote_port, &state, &actual) == 6 &&
            local == UINT32_C(0x0100007f) && local_port == port && !remote && !remote_port &&
            state == 10 && actual == inode) {
            found = 1;
            break;
        }
    }
    fclose(stream);
    return found;
}
static int observations(const json_t *proof, const gstd_network_worker *worker,
                        gstd_network_worker_observation *output) {
    static const char *const names[] = {"keeperPid",     "keeperStart",  "userDevice", "userInode",
                                        "networkDevice", "networkInode", "apiPort",    "gitPort",
                                        "apiInode",      "gitInode"};
    uint64_t numbers[10];
    if (!json_is_object(proof) || json_object_size(proof) != 12)
        return -1;
    for (size_t i = 0; i < 10; i++)
        if (integer(proof, names[i],
                    i == 0             ? INT_MAX
                    : i == 6 || i == 7 ? UINT16_MAX
                                       : INT64_MAX,
                    &numbers[i]))
            return -1;
    if (!numbers[0] || !numbers[1] || !numbers[3] || !numbers[5])
        return -1;
    const json_t *api = json_object_get(proof, "apiDescriptor"),
                 *git = json_object_get(proof, "gitDescriptor");
    if (!json_is_integer(api) || !json_is_integer(git) || json_integer_value(api) < -1 ||
        json_integer_value(api) > 1023 || json_integer_value(git) < -1 ||
        json_integer_value(git) > 1023)
        return -1;
    output->api_descriptor = (int)json_integer_value(api);
    output->git_descriptor = (int)json_integer_value(git);
    output->api_port = (uint16_t)numbers[6];
    output->git_port = (uint16_t)numbers[7];
    output->api_inode = numbers[8];
    output->git_inode = numbers[9];
    int have_api = worker->roles & 1, have_git = worker->roles & 2;
    if (!(have_api ? output->api_port >= 1024 && output->api_descriptor >= 3 && output->api_inode &&
                         listener(worker->pid, output->api_descriptor, output->api_inode,
                                  output->api_port)
                   : !output->api_port && output->api_descriptor == -1 && !output->api_inode) ||
        !(have_git ? output->git_port >= 1024 && output->git_descriptor >= 3 && output->git_inode &&
                         listener(worker->pid, output->git_descriptor, output->git_inode,
                                  output->git_port)
                   : !output->git_port && output->git_descriptor == -1 && !output->git_inode) ||
        (have_api && have_git && output->api_port == output->git_port))
        return -1;
    output->pid = worker->pid;
    output->start = worker->start;
    gstd_network_namespace *network = &output->network;
    network->keeper_pid = (pid_t)numbers[0];
    network->keeper_start = numbers[1];
    network->user_device = numbers[2];
    network->user_inode = numbers[3];
    network->network_device = numbers[4];
    network->network_inode = numbers[5];
    memcpy(network->boot_id, worker->boot_id, 37);
    network->user_fd = keeper_namespace(network->keeper_pid, "user");
    network->network_fd = keeper_namespace(network->keeper_pid, "net");
    return gstd_network_namespace_check(network);
}
#endif
int gstd_network_worker_reopen(const gstd_network_worker_request *request, const json_t *expected,
                               gstd_network_worker *output,
                               gstd_network_worker_observation *observation) {
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (!request || !output || !observation || output->pid || output->pid_fd != -1 ||
        output->scope_fd != -1 || output->channel != -1 || observation->network.user_fd != -1 ||
        observation->network.network_fd != -1 || !json_is_object(expected) ||
        json_object_size(expected) != 11 || gstd_asset_manifest_check(request->manifest))
        return -1;
    gstd_network_worker worker = GSTD_NETWORK_WORKER_INITIAL;
    gstd_network_worker_observation actual = GSTD_NETWORK_OBSERVATION_INITIAL;
    char boot[37];
    struct stat scope;
    uint64_t version, pid, roles;
    json_t *owner = NULL, *ready = NULL;
    unsigned required = 0;
    json_t *images = json_object_get(request->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *name =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (!strcmp(name, "api"))
            required |= 1;
        if (!strcmp(name, "git"))
            required |= 2;
    }
    if (!request->store_id || !request->resource_id || !request->operation_id ||
        gstd_native_boot_id(boot) || !string(expected, "bootId", boot) ||
        !string(expected, "storeId", request->store_id) ||
        !string(expected, "resourceId", request->resource_id) ||
        !string(expected, "operationId", request->operation_id) ||
        !string(expected, "profileDigest",
                json_string_value(json_object_get(request->manifest, "profileDigest"))) ||
        integer(expected, "schemaVersion", 1, &version) || version != 1 ||
        integer(expected, "pid", INT_MAX, &pid) || !pid ||
        integer(expected, "startTime", INT64_MAX, &worker.start) || !worker.start ||
        integer(expected, "roles", 3, &roles) || roles != required ||
        decimal(expected, "scopeDevice", &worker.scope_device) ||
        decimal(expected, "scopeInode", &worker.scope_inode) || !worker.scope_inode ||
        request->scope_fd < 3 || fstat(request->scope_fd, &scope) || !S_ISDIR(scope.st_mode) ||
        scope.st_uid != geteuid() || (scope.st_mode & 07777) != 0700 ||
        (uint64_t)scope.st_dev != worker.scope_device ||
        (uint64_t)scope.st_ino != worker.scope_inode)
        goto fail;
    worker.pid = (pid_t)pid;
    worker.roles = required;
    memcpy(worker.boot_id, boot, 37);
    snprintf(worker.store_id, sizeof(worker.store_id), "%s", request->store_id);
    snprintf(worker.resource_id, sizeof(worker.resource_id), "%s", request->resource_id);
    snprintf(worker.operation_id, sizeof(worker.operation_id), "%s", request->operation_id);
    snprintf(worker.profile_digest, sizeof(worker.profile_digest), "%s",
             json_string_value(json_object_get(request->manifest, "profileDigest")));
    worker.pid_fd = (int)syscall(SYS_pidfd_open, worker.pid, 0);
    if (worker.pid_fd < 3 || !active(&worker) || !worker_lock(request->scope_fd, worker.pid))
        goto fail;
    owner = record(request->scope_fd, "owner.json");
    ready = record(request->scope_fd, "ready.json");
    if (!owner || !ready || !json_equal(owner, expected) || !json_is_object(ready) ||
        json_object_size(ready) != 12)
        goto fail;
    json_t *proof = json_object_get(ready, "observed");
    if (!proof || observations(proof, &worker, &actual))
        goto fail;
    if (json_object_del(ready, "observed") || !json_equal(ready, expected) || !active(&worker))
        goto fail;
    worker.scope_fd = fcntl(request->scope_fd, F_DUPFD_CLOEXEC, 3);
    if (worker.scope_fd < 3)
        goto fail;
    worker.received = 1;
    worker.permitted = 1;
    *output = worker;
    *observation = actual;
    json_decref(owner);
    json_decref(ready);
    return 0;
fail:
    json_decref(owner);
    json_decref(ready);
    gstd_network_worker_observation_close(&actual);
    gstd_network_worker_close(&worker);
    return -1;
#else
    (void)request;
    (void)expected;
    (void)output;
    (void)observation;
    return -1;
#endif
}
