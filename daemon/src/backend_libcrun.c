#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "backend_libcrun.h"
#include "execution_plan.h"
#include "gitstored/execution.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) && defined(GITSTORED_LIBCRUN)
#include "native_abi.h"
#include <linux/openat2.h>
#include <sys/syscall.h>
static int same(const json_t *value, const char *text) {
    return json_is_string(value) && json_string_length(value) == strlen(text) &&
           !memcmp(json_string_value(value), text, strlen(text));
}
static int directory(int fd, int private) {
    struct stat info;
    return fd >= 3 && !fstat(fd, &info) && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
           !(info.st_mode & 0022) && (!private || (info.st_mode & 07777) == 0700);
}
static int path(int fd, char output[4096]) {
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(link, output, 4095);
    if (length <= 0 || length >= 4095)
        return -1;
    output[length] = 0;
    return output[0] == '/' && !strstr(output, " (deleted)") ? 0 : -1;
}
static int bounded_worker(void) {
    struct rlimit bound;
    if (getrlimit(RLIMIT_CORE, &bound) || bound.rlim_cur)
        return 0;
#ifndef GITSTORED_INSTRUMENTED
    if (getrlimit(RLIMIT_AS, &bound) || bound.rlim_cur == RLIM_INFINITY ||
        bound.rlim_cur > UINT64_C(1024) * 1024 * 1024)
        return 0;
#endif
    return !getrlimit(RLIMIT_FSIZE, &bound) && bound.rlim_cur != RLIM_INFINITY &&
           bound.rlim_cur <= UINT64_C(8) * 1024 * 1024 * 1024;
}
static int mapped(const json_t *value, uid_t host) {
    json_t *entry = json_array_get(value, 0);
    return json_is_array(value) && json_array_size(value) == 1 &&
           json_integer_value(json_object_get(entry, "containerID")) == 0 &&
           json_is_integer(json_object_get(entry, "containerID")) &&
           json_integer_value(json_object_get(entry, "hostID")) == (json_int_t)host &&
           json_integer_value(json_object_get(entry, "size")) == 1;
}
static int configuration(const gstd_native_context *context, const char *root) {
    int joined =
        !context->test_fixture && context->registered_inputs && context->registered_inputs->network;
    if (!context->test_fixture) {
        const gstd_execution_inputs *input = context->registered_inputs;
        if (!input || !input->component || strcmp(input->component, context->component) ||
            input->rootfs_fd != context->rootfs_fd)
            return -1;
        json_t *registered = gstd_execution_build(input);
        int valid = registered && json_equal(registered, context->configuration);
        json_decref(registered);
        if (!valid)
            return -1;
    }
    const json_t *spec = context->configuration;
    json_t *process = json_object_get(spec, "process"), *linux = json_object_get(spec, "linux"),
           *caps = json_object_get(process, "capabilities"), *policy = gstd_execution_policy();
    int valid =
        json_is_object(spec) && !json_object_get(spec, "hooks") &&
        json_is_false(json_object_get(process, "terminal")) &&
        json_is_true(json_object_get(process, "noNewPrivileges")) &&
        json_integer_value(json_object_get(json_object_get(process, "user"), "uid")) == 0 &&
        json_is_integer(json_object_get(json_object_get(process, "user"), "uid")) &&
        json_integer_value(json_object_get(json_object_get(process, "user"), "gid")) == 0 &&
        json_is_integer(json_object_get(json_object_get(process, "user"), "gid")) &&
        json_is_true(json_object_get(json_object_get(spec, "root"), "readonly")) &&
        same(json_object_get(json_object_get(spec, "root"), "path"), root) && policy &&
        json_equal(policy, json_object_get(linux, "seccomp")) &&
        (joined ? (!json_object_get(linux, "uidMappings") && !json_object_get(linux, "gidMappings"))
                : (mapped(json_object_get(linux, "uidMappings"), geteuid()) &&
                   mapped(json_object_get(linux, "gidMappings"), getegid()))) &&
        !json_object_get(linux, "cgroupsPath") && !json_object_get(linux, "sysctl");
    json_decref(policy);
    const char *sets[] = {"bounding", "effective", "inheritable", "permitted", "ambient"};
    if (!json_is_object(caps) || json_object_size(caps) != 5)
        valid = 0;
    for (size_t i = 0; i < 5; i++) {
        json_t *set = json_object_get(caps, sets[i]);
        if (!json_is_array(set) || json_array_size(set))
            valid = 0;
    }
    const char *types[] = {"user", "pid", "mount", "uts", "ipc", "network"};
    unsigned seen = 0;
    json_t *namespaces = json_object_get(linux, "namespaces");
    if (!json_is_array(namespaces) || json_array_size(namespaces) != 6)
        valid = 0;
    for (size_t i = 0; i < json_array_size(namespaces); i++) {
        json_t *entry = json_array_get(namespaces, i);
        unsigned bit = 0;
        for (size_t j = 0; j < 6; j++)
            if (same(json_object_get(entry, "type"), types[j]))
                bit = 1u << j;
        int path = json_object_get(entry, "path") != NULL;
        int expected_path = joined && (bit == 1 || bit == 32);
        if (!bit || (seen & bit) || path != expected_path)
            valid = 0;
        seen |= bit;
    }
    json_t *resources = json_object_get(linux, "resources");
    uint64_t memory =
        context->test_fixture ? UINT64_C(128) * 1024 * 1024 : UINT64_C(512) * 1024 * 1024;
    int tasks = context->test_fixture ? 64 : 256;
    int quota = context->test_fixture ? 50000 : 100000;
    if (json_integer_value(json_object_get(json_object_get(resources, "memory"), "limit")) !=
            (json_int_t)memory ||
        json_integer_value(json_object_get(json_object_get(resources, "pids"), "limit")) != tasks ||
        json_integer_value(json_object_get(json_object_get(resources, "cpu"), "quota")) != quota ||
        json_integer_value(json_object_get(json_object_get(resources, "cpu"), "period")) != 100000)
        valid = 0;
    return valid && seen == 63 ? 0 : -1;
}
static int observe(const gstd_crun_request *native, const gstd_native_context *expected,
                   gstd_native_observation *output, const char *bundle, const char *root) {
    gstd_crun_state state = {0};
    if (gstd_crun_observe(native, bundle, root, &state) ||
        (expected->expected_pid && (state.pid != expected->expected_pid ||
                                    state.start_time != expected->expected_start_time))) {
        output->native_error = state.error;
        return -1;
    }
    output->pid = state.pid;
    output->start_time = state.start_time;
    output->created = state.created;
    output->running = state.running;
    output->stopped = state.stopped;
    return 0;
}
#endif

int gstd_native_backend_available(void) {
#if defined(__linux__) && defined(GITSTORED_LIBCRUN)
    return 1;
#else
    return 0;
#endif
}
gstd_native_status gstd_native_apply(const gstd_native_context *context, gstd_native_action action,
                                     gstd_native_observation *output) {
    if (!output)
        return GSTD_NATIVE_INVALID;
    memset(output, 0, sizeof(*output));
    if (!context || action < GSTD_NATIVE_CREATE || action > GSTD_NATIVE_DELETE ||
        !context->backend_id || !context->component || context->control_owner <= 0 ||
        context->control_owner == getpid())
        return GSTD_NATIVE_INVALID;
    if (context->expected_pid < 0 || (!context->expected_pid && context->expected_start_time) ||
        (action == GSTD_NATIVE_CREATE && (context->expected_pid || context->expected_start_time)) ||
        (action != GSTD_NATIVE_CREATE && action != GSTD_NATIVE_OBSERVE &&
         (!context->expected_pid || !context->expected_start_time)))
        return GSTD_NATIVE_INVALID;
#if defined(__linux__) && defined(GITSTORED_LIBCRUN)
    if (!geteuid() || !gstd_execution_kernel_supported() || !bounded_worker())
        return GSTD_NATIVE_HOST_UNMET;
    /* The pinned library uses pidfds but retains legacy ENOSYS fallbacks.
     * This supported kernel lane requires both APIs before any native effect. */
    int self = (int)syscall(SYS_pidfd_open, getpid(), 0);
    if (self < 0)
        return GSTD_NATIVE_HOST_UNMET;
    int pidfd_supported = !syscall(SYS_pidfd_send_signal, self, 0, NULL, 0);
    close(self);
    if (!pidfd_supported)
        return GSTD_NATIVE_HOST_UNMET;
    if (action == GSTD_NATIVE_CREATE && !gstd_crun_host_ready())
        return GSTD_NATIVE_HOST_UNMET;
    int fixture = 0;
#ifdef GITSTORED_TESTING
    fixture = context->test_fixture && !strcmp(context->component, "fixture");
#endif
    if ((context->test_fixture && !fixture) ||
        (!fixture && strcmp(context->component, "api") && strcmp(context->component, "git") &&
         strcmp(context->component, "controller")))
        return GSTD_NATIVE_INVALID;
    char boot[37];
    if (!fixture &&
        (!context->boot_id || gstd_native_boot_id(boot) || strcmp(boot, context->boot_id)))
        return action == GSTD_NATIVE_CREATE ? GSTD_NATIVE_INVALID : GSTD_NATIVE_UNCERTAIN;
    if (action == GSTD_NATIVE_CREATE && !fixture) {
        if (!strcmp(context->component, "git")) {
            struct stat lease;
            if (context->writer_lease_fd != 3 || fstat(3, &lease) || !S_ISREG(lease.st_mode) ||
                lease.st_uid != geteuid() || lease.st_nlink != 1 || lease.st_size ||
                (lease.st_mode & 07777) != 0600 || flock(3, LOCK_EX | LOCK_NB))
                return GSTD_NATIVE_INVALID;
        } else if (context->writer_lease_fd >= 3)
            return GSTD_NATIVE_INVALID;
    }
    size_t length = strnlen(context->backend_id, 70);
    if (!length || length >= 70 || !gstd_identifier(context->backend_id, length > 64 ? 64 : length))
        return GSTD_NATIVE_INVALID;
    for (size_t i = 0; i < length; i++)
        if (!(context->backend_id[i] == '-' || context->backend_id[i] == '_' ||
              (context->backend_id[i] >= 'a' && context->backend_id[i] <= 'z') ||
              (context->backend_id[i] >= '0' && context->backend_id[i] <= '9')))
            return GSTD_NATIVE_INVALID;
    if (!directory(context->state_fd, 1) || !directory(context->bundle_fd, 1) ||
        !directory(context->rootfs_fd, 0))
        return GSTD_NATIVE_INVALID;
    char state_path[64], bundle[4096], root[4096];
    snprintf(state_path, sizeof(state_path), "/proc/self/fd/%d", context->state_fd);
    if (path(context->bundle_fd, bundle) || path(context->rootfs_fd, root) ||
        (action == GSTD_NATIVE_CREATE && configuration(context, root)))
        return GSTD_NATIVE_INVALID;
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    int owned =
        (int)syscall(SYS_openat2, context->state_fd, context->backend_id, &how, sizeof(how));
    if (action == GSTD_NATIVE_CREATE) {
        if (owned >= 0) {
            close(owned);
            return GSTD_NATIVE_CONFLICT;
        }
        if (errno != ENOENT)
            return GSTD_NATIVE_FAILED;
    } else if (owned < 0 || !directory(owned, 1)) {
        if (owned >= 0)
            close(owned);
        return GSTD_NATIVE_UNCERTAIN;
    }
    if (owned >= 0)
        close(owned);
    gstd_crun_request native = {state_path, context->backend_id, bundle,
                                action == GSTD_NATIVE_CREATE && context->writer_lease_fd == 3 ? 1
                                                                                              : 0};
    gstd_native_status result = GSTD_NATIVE_FAILED;
    if (action != GSTD_NATIVE_CREATE && observe(&native, context, output, bundle, root))
        return GSTD_NATIVE_UNCERTAIN;
    int status = -1;
    if (action == GSTD_NATIVE_OBSERVE)
        return GSTD_NATIVE_OK;
    output->effect_possible = 1;
    if (action == GSTD_NATIVE_CREATE) {
        char *bytes = gstd_json_dump(context->configuration);
        if (!bytes)
            return GSTD_NATIVE_FAILED;
        status = gstd_crun_create(&native, bytes, &output->native_error);
        free(bytes);
    } else if (action == GSTD_NATIVE_START)
        status = gstd_crun_start(&native, &output->native_error);
    else if (action == GSTD_NATIVE_TERM || action == GSTD_NATIVE_KILL)
        status = gstd_crun_signal(&native, action == GSTD_NATIVE_KILL, &output->native_error);
    else if (action == GSTD_NATIVE_DELETE && output->stopped)
        status = gstd_crun_delete(&native, &output->native_error);
    if (status >= 0)
        result = action == GSTD_NATIVE_DELETE || !observe(&native, context, output, bundle, root)
                     ? GSTD_NATIVE_OK
                     : GSTD_NATIVE_UNCERTAIN;
    return result;
#else
    return GSTD_NATIVE_UNAVAILABLE;
#endif
}
