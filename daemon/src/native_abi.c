#define _GNU_SOURCE 1
#if defined(__linux__) && defined(GITSTORED_LIBCRUN)
#include "native_abi.h"
#include <fcntl.h>
#include <libcrun/container.h>
#include <libcrun/seccomp.h>
#include <libcrun/status.h>
#include <linux/magic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
int gstd_crun_worker_filter(const char *bytes) {
    libcrun_error_t error = NULL;
    libcrun_container_t *container = libcrun_container_load_from_memory(bytes, &error);
    int fd = memfd_create("gstd-network-policy", MFD_CLOEXEC), result = -1;
    if (container && fd >= 0) {
        struct libcrun_seccomp_gen_ctx_s generator;
        libcrun_seccomp_gen_ctx_init(&generator, container, true, LIBCRUN_SECCOMP_SKIP_CACHE);
        generator.fd = fd;
        libcrun_set_verbosity(LIBCRUN_VERBOSITY_ERROR);
        if (libcrun_generate_seccomp(&generator, &error) >= 0)
            result = libcrun_apply_seccomp(fd, -1, NULL, 0, (char *[]){NULL}, 0, &error);
    }
    if (fd >= 0)
        close(fd);
    if (container)
        libcrun_container_free(container);
    if (error)
        libcrun_error_release(&error);
    return result < 0 ? -1 : 0;
}
int gstd_crun_host_ready(void) {
    char path[256], controllers[1024];
    struct stat info;
    struct statfs filesystem;
    if (statfs("/sys/fs/cgroup", &filesystem) || filesystem.f_type != CGROUP2_SUPER_MAGIC)
        return 0;
    snprintf(path, sizeof(path), "/run/user/%lu", (unsigned long)geteuid());
    if (lstat(path, &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700)
        return 0;
    snprintf(path, sizeof(path), "/run/user/%lu/bus", (unsigned long)geteuid());
    if (lstat(path, &info) || !S_ISSOCK(info.st_mode) || info.st_uid != geteuid())
        return 0;
    snprintf(path, sizeof(path),
             "/sys/fs/cgroup/user.slice/user-%lu.slice/user@%lu.service/cgroup.controllers",
             (unsigned long)geteuid(), (unsigned long)geteuid());
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    ssize_t count = read(fd, controllers + 1, sizeof(controllers) - 3);
    close(fd);
    if (count <= 0 || count >= (ssize_t)sizeof(controllers) - 3)
        return 0;
    controllers[0] = ' ';
    controllers[count + 1] = ' ';
    controllers[count + 2] = 0;
    for (ssize_t i = 1; i <= count; i++)
        if (controllers[i] == '\n')
            controllers[i] = ' ';
    if (!strstr(controllers, " cpu ") || !strstr(controllers, " memory ") ||
        !strstr(controllers, " pids "))
        return 0;
    sd_bus *bus = NULL;
    int ready = sd_bus_open_user(&bus) >= 0;
    sd_bus_unref(bus);
    return ready;
}
static void quiet(int number, const char *message, int verbosity, void *argument) {
    (void)number;
    (void)message;
    (void)verbosity;
    (void)argument;
}
static libcrun_context_t context(const gstd_crun_request *request) {
    libcrun_set_verbosity(LIBCRUN_VERBOSITY_ERROR);
    return (libcrun_context_t){.state_root = request->state_root,
                               .id = request->id,
                               .bundle = request->bundle,
                               .detach = true,
                               .systemd_cgroup = true,
                               .no_new_keyring = true,
                               .preserve_fds = request->preserve_fds,
                               .output_handler = quiet};
}
static int finish(int result, libcrun_error_t *error, int *number) {
    if (*error) {
        *number = (*error)->status;
        libcrun_error_release(error);
    }
    return result;
}
static int member(pid_t pid, const char *group) {
    char path[64], bytes[4096], expected[4096];
    int length = snprintf(expected, sizeof(expected), "0::%s\n", group);
    if (length <= 0 || (size_t)length >= sizeof(expected))
        return 0;
    snprintf(path, sizeof(path), "/proc/%ld/cgroup", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    ssize_t count = read(fd, bytes, sizeof(bytes) - 1);
    close(fd);
    if (count <= 0 || count >= (ssize_t)sizeof(bytes) - 1)
        return 0;
    bytes[count] = 0;
    const char *found = strstr(bytes, expected);
    return found && (found == bytes || found[-1] == '\n');
}
int gstd_crun_observe(const gstd_crun_request *request, const char *bundle, const char *root,
                      gstd_crun_state *output) {
    libcrun_container_status_t status = {0};
    libcrun_error_t error = NULL;
    const char *state = NULL;
    int running = 0, result = -1;
    char scope[128];
    snprintf(scope, sizeof(scope), "/crun-%s.scope/", request->id);
    if (libcrun_read_container_status(&status, request->state_root, request->id, &error) < 0 ||
        status.pid <= 0 || !status.process_start_time || !status.systemd_cgroup || !status.bundle ||
        strcmp(status.bundle, bundle) || !status.rootfs || strcmp(status.rootfs, root) ||
        !status.cgroup_path || !strstr(status.cgroup_path, scope) ||
        libcrun_get_container_state_string(request->id, &status, request->state_root, &state,
                                           &running, &error) < 0)
        goto done;
    if (running && !member(status.pid, status.cgroup_path))
        goto done;
    output->pid = status.pid;
    output->start_time = status.process_start_time;
    output->created = !strcmp(state, "created");
    output->running = !strcmp(state, "running");
    output->stopped = !strcmp(state, "stopped");
    result = output->created || output->running || output->stopped ? 0 : -1;
done:
    libcrun_free_container_status(&status);
    return finish(result, &error, &output->error);
}
int gstd_crun_create(const gstd_crun_request *request, const char *bytes, int *number) {
    libcrun_context_t native = context(request);
    libcrun_error_t error = NULL;
    libcrun_container_t *container = libcrun_container_load_from_memory(bytes, &error);
    int result = container ? libcrun_container_create(&native, container, 0, &error) : -1;
    if (container)
        libcrun_container_free(container);
    return finish(result, &error, number);
}
int gstd_crun_start(const gstd_crun_request *request, int *number) {
    libcrun_context_t native = context(request);
    libcrun_error_t error = NULL;
    int result = libcrun_container_start(&native, native.id, &error);
    return finish(result, &error, number);
}
int gstd_crun_signal(const gstd_crun_request *request, int hard, int *number) {
    libcrun_context_t native = context(request);
    libcrun_error_t error = NULL;
    int result = libcrun_container_kill(&native, native.id, hard ? "SIGKILL" : "SIGTERM", &error);
    return finish(result, &error, number);
}
int gstd_crun_delete(const gstd_crun_request *request, int *number) {
    libcrun_context_t native = context(request);
    libcrun_error_t error = NULL;
    int result = libcrun_container_delete(&native, NULL, native.id, false, &error);
    return finish(result, &error, number);
}
#else
typedef int gstd_native_abi_disabled;
#endif
