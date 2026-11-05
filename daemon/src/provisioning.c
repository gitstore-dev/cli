#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/provisioning.h"
#include "gitstored/asset_tree.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "image_backend.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
static int same(const json_t *value, const char *expected) {
    return json_is_string(value) && json_string_length(value) == strlen(expected) &&
           !memcmp(json_string_value(value), expected, strlen(expected));
}
int gstd_provision_registry_auth(const json_t *manifest, const char *component,
                                 int installed_broker_fd, const unsigned char *request,
                                 size_t request_length, unsigned timeout_ms, gstd_credential *owner,
                                 gstd_image_auth *auth) {
    if (!owner || owner->bytes || owner->length || !auth || auth->kind || auth->username ||
        auth->username_length || auth->secret || auth->secret_length || !component || !request ||
        !request_length || request_length > 8192 || gstd_asset_manifest_check(manifest))
        return -1;
    json_t *body = gstd_json_parse(request, request_length), *image = NULL,
           *images = json_object_get(manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        json_t *candidate = json_array_get(images, i);
        if (same(json_object_get(candidate, "component"), component))
            image = candidate;
    }
    const char *source = image ? json_string_value(json_object_get(image, "source")) : NULL;
    const char *slash = source ? strchr(source, '/') : NULL;
    json_t *identity = json_object_get(body, "identity");
    char origin[1024];
    int count =
        slash ? snprintf(origin, sizeof(origin), "https://%.*s", (int)(slash - source), source)
              : -1;
    int valid = body && count > 0 && (size_t)count < sizeof(origin) &&
                same(json_object_get(identity, "purpose"), "registry-pull") &&
                same(json_object_get(identity, "registry"), origin) &&
                same(json_object_get(identity, "repository"), slash + 1);
    json_decref(body);
    if (!valid)
        return -1;
    if (gstd_credential_run(installed_broker_fd, request, request_length, timeout_ms, owner))
        return -2;
    unsigned char *colon = memchr(owner->bytes, ':', owner->length);
    if (!colon || colon == owner->bytes || colon == owner->bytes + owner->length - 1 ||
        (size_t)(colon - owner->bytes) > 4096) {
        gstd_credential_clear(owner);
        return -1;
    }
    auth->kind = 1;
    auth->username = owner->bytes;
    auth->username_length = (size_t)(colon - owner->bytes);
    auth->secret = colon + 1;
    auth->secret_length = owner->length - auth->username_length - 1;
    return 0;
}
json_t *gstd_provision_plan(const json_t *manifest, const char *component, uint64_t download_limit,
                            uint64_t merge_limit, unsigned timeout_ms) {
    if (!component || gstd_asset_manifest_check(manifest) || !download_limit ||
        download_limit > UINT64_C(2) * 1024 * 1024 * 1024 || merge_limit < 1024 ||
        merge_limit > UINT64_C(8) * 1024 * 1024 * 1024 || !timeout_ms || timeout_ms > 600000)
        return NULL;
    json_t *images = json_object_get(manifest, "images"), *image = NULL;
    for (size_t i = 0; i < json_array_size(images); i++) {
        json_t *candidate = json_array_get(images, i);
        if (!strcmp(component, json_string_value(json_object_get(candidate, "component"))))
            image = candidate;
    }
    if (!image)
        return NULL;
    char reference[4096];
    int count = snprintf(reference, sizeof(reference), "%s@%s",
                         json_string_value(json_object_get(image, "source")),
                         json_string_value(json_object_get(image, "digest")));
    if (count < 0 || (size_t)count >= sizeof(reference))
        return NULL;
    json_t *platform = json_object_get(manifest, "platform");
    return json_pack(
        "{s:i,s:s,s:O,s:O,s:O,s:O,s:O,s:I,s:I,s:i}", "schemaVersion", 1, "reference", reference,
        "configDigest", json_object_get(image, "configDigest"), "os",
        json_object_get(platform, "os"), "architecture", json_object_get(platform, "architecture"),
        "tokenOrigins", json_object_get(image, "tokenOrigins"), "blobOrigins",
        json_object_get(image, "blobOrigins"), "maximumDownloadBytes", (json_int_t)download_limit,
        "maximumMergedBytes", (json_int_t)merge_limit, "timeoutMs", (int)timeout_ms);
}
#ifdef __linux__
typedef struct {
    uint32_t status, reserved;
    gstd_provision_result result;
} report;
static int64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int limit(int kind, rlim_t maximum) {
    struct rlimit current;
    if (getrlimit(kind, &current))
        return -1;
    if (current.rlim_cur < maximum)
        maximum = current.rlim_cur;
    struct rlimit bounded = {maximum, maximum};
    return setrlimit(kind, &bounded);
}
static void worker(int staging, int writer, int source, int mode, pid_t parent, long maximum_fd,
                   const unsigned char *plan, size_t length, const gstd_image_auth *auth,
                   uint64_t file_limit, uint64_t logical_limit) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || prctl(PR_SET_DUMPABLE, 0) ||
        dup2(staging, 3) < 0 || dup2(writer, 4) < 0 || fcntl(3, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(4, F_SETFD, FD_CLOEXEC) < 0 ||
        (source >= 0 && (dup2(source, 5) < 0 || fcntl(5, F_SETFD, FD_CLOEXEC) < 0)))
        _exit(126);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
        _exit(126);
    for (int fd = 0; fd < 3; fd++)
        if (dup2(null, fd) < 0)
            _exit(126);
#ifdef SYS_close_range
    if (syscall(SYS_close_range, source >= 0 ? 6u : 5u, UINT_MAX, 0u))
#endif
        for (int fd = source >= 0 ? 6 : 5; fd < maximum_fd; fd++)
            close(fd);
    if (limit(RLIMIT_CORE, 0) ||
#ifdef GITSTORED_INSTRUMENTED
        /* Finite allowance for ASan shadow mappings, paired with the SDK's
         * explicitly instrumented-host build. This is not physical memory. */
        limit(RLIMIT_AS, UINT64_C(64) * 1024 * 1024 * 1024 * 1024) ||
#else
        limit(RLIMIT_AS, UINT64_C(1024) * 1024 * 1024) ||
#endif
        limit(RLIMIT_FSIZE, (rlim_t)file_limit) || limit(RLIMIT_NOFILE, 128))
        _exit(126);
    report output = {0};
    gstd_image_result image_result = {0};
    output.status =
        (uint32_t)(source >= 0
                       ? (mode == 2
                              ? gstd_image_resume(plan, length, 5, 3, auth, &image_result)
                              : (mode == 3
                                     ? gstd_image_materialize(plan, length, 5, 3, &image_result)
                                     : gstd_image_import(plan, length, 5, 3, &image_result)))
                       : gstd_image_stage(plan, length, 3, auth, &image_result));
    if (output.status == GSTD_IMAGE_OK) {
        const char *metadata[] = {"manifest.json", "config.json"};
        for (size_t i = 0; i < 2; i++) {
            int fd = openat(3, metadata[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            struct stat info;
            if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
                info.st_uid != geteuid() || fchmod(fd, 0600) || fsync(fd))
                output.status = GSTD_IMAGE_INTEGRITY;
            if (fd >= 0)
                close(fd);
            if (output.status != GSTD_IMAGE_OK)
                break;
        }
        struct stat directory;
        if (output.status == GSTD_IMAGE_OK &&
            (gstd_asset_tree_digest(3, logical_limit, 1, output.result.tree_digest,
                                    &output.result.logical_bytes) ||
             fstat(3, &directory)))
            output.status = GSTD_IMAGE_INTEGRITY;
        if (output.status == GSTD_IMAGE_OK) {
            output.result.downloaded_bytes = image_result.downloaded_bytes;
            output.result.merged_bytes = image_result.merged_bytes;
            output.result.staging_device = (uint64_t)directory.st_dev;
            output.result.staging_inode = (uint64_t)directory.st_ino;
        }
    }
    const unsigned char *bytes = (const unsigned char *)&output;
    size_t remaining = sizeof(output);
    while (remaining) {
        ssize_t wrote = write(4, bytes, remaining);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            _exit(126);
        bytes += wrote;
        remaining -= (size_t)wrote;
    }
    _exit(0);
}
#endif
static gstd_provision_status provision(const json_t *manifest, const char *component, int source_fd,
                                       int mode, int staging_fd, const gstd_image_auth *auth,
                                       uint64_t download_limit, uint64_t merge_limit,
                                       unsigned timeout_ms, int cancellation_fd,
                                       gstd_provision_result *result) {
    if (!result)
        return GSTD_PROVISION_INVALID;
    memset(result, 0, sizeof(*result));
    json_t *plan =
        gstd_provision_plan(manifest, component, download_limit, merge_limit, timeout_ms);
    if (!plan || ((mode == 0 || mode == 2) && !auth)) {
        json_decref(plan);
        return GSTD_PROVISION_INVALID;
    }
    if (!gstd_image_backend_available()) {
        json_decref(plan);
        return GSTD_PROVISION_UNAVAILABLE;
    }
#ifdef __linux__
    struct stat info;
    struct statvfs disk;
    struct sigaction child_action;
    struct utsname host;
    uint64_t required = download_limit + 2 * merge_limit + UINT64_C(1024) * 1024;
    if (!geteuid() || sigaction(SIGCHLD, NULL, &child_action) ||
        child_action.sa_handler != SIG_DFL || (child_action.sa_flags & SA_NOCLDWAIT) ||
        uname(&host)) {
        json_decref(plan);
        return GSTD_PROVISION_UNAVAILABLE;
    }
    const char *architecture = !strcmp(host.machine, "aarch64")  ? "arm64"
                               : !strcmp(host.machine, "x86_64") ? "amd64"
                                                                 : "unknown";
    if (strcmp(architecture, json_string_value(json_object_get(plan, "architecture")))) {
        json_decref(plan);
        return GSTD_PROVISION_INVALID;
    }
    if (fstat(staging_fd, &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700 ||
        (cancellation_fd >= 0 && fcntl(cancellation_fd, F_GETFD) < 0)) {
        json_decref(plan);
        return GSTD_PROVISION_INVALID;
    }
    if (source_fd >= 0) {
        struct stat source;
        if (fstat(source_fd, &source) || !S_ISDIR(source.st_mode) ||
            (source.st_uid != 0 && source.st_uid != geteuid()) || (source.st_mode & 0022) ||
            (mode >= 2 && (source.st_uid != geteuid() || (source.st_mode & 07777) != 0700)) ||
            (source.st_dev == info.st_dev && source.st_ino == info.st_ino)) {
            json_decref(plan);
            return GSTD_PROVISION_INVALID;
        }
    }
    if (fstatvfs(staging_fd, &disk) || !disk.f_frsize ||
        disk.f_bavail < required / disk.f_frsize + (required % disk.f_frsize != 0)) {
        json_decref(plan);
        return GSTD_PROVISION_QUOTA;
    }
    if (cancellation_fd >= 0) {
        struct pollfd cancel = {cancellation_fd, POLLIN, 0};
        if (poll(&cancel, 1, 0) > 0 && cancel.revents) {
            json_decref(plan);
            return GSTD_PROVISION_CANCELLED;
        }
    }
    char *bytes = gstd_json_dump(plan);
    json_decref(plan);
    if (!bytes)
        return GSTD_PROVISION_WORKER;
    size_t length = strlen(bytes);
    int pipes[2] = {-1, -1}, staging = fcntl(staging_fd, F_DUPFD_CLOEXEC, 10);
    if (staging < 0 || pipe2(pipes, O_CLOEXEC | O_NONBLOCK)) {
        if (staging >= 0)
            close(staging);
        free(bytes);
        return GSTD_PROVISION_WORKER;
    }
    /* High duplicates avoid descriptor aliasing while installing fixed 3/4. */
    int writer = fcntl(pipes[1], F_DUPFD_CLOEXEC, 10);
    close(pipes[1]);
    if (writer < 0) {
        close(staging);
        close(pipes[0]);
        free(bytes);
        return GSTD_PROVISION_WORKER;
    }
    int source = source_fd >= 0 ? fcntl(source_fd, F_DUPFD_CLOEXEC, 10) : -1;
    if (source_fd >= 0 && source < 0) {
        close(writer);
        close(staging);
        close(pipes[0]);
        free(bytes);
        return GSTD_PROVISION_WORKER;
    }
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > 1048576)
        maximum = 1048576;
    int64_t start = now_ms();
    pid_t parent = getpid();
    if (start < 0) {
        if (source >= 0)
            close(source);
        close(writer);
        close(staging);
        close(pipes[0]);
        free(bytes);
        return GSTD_PROVISION_WORKER;
    }
    uint64_t file_limit = merge_limit > download_limit ? merge_limit : download_limit;
    if (file_limit < 1024 * 1024)
        file_limit = 1024 * 1024;
    pid_t child = fork();
    if (!child)
        worker(staging, writer, source, mode, parent, maximum, (unsigned char *)bytes, length, auth,
               file_limit, merge_limit);
    close(writer);
    close(staging);
    if (source >= 0)
        close(source);
    free(bytes);
    if (child < 0) {
        close(pipes[0]);
        return GSTD_PROVISION_WORKER;
    }
    report output = {0};
    size_t have = 0;
    gstd_provision_status outcome = GSTD_PROVISION_WORKER;
    while (have < sizeof(output)) {
        int64_t now = now_ms();
        if (now < 0)
            break;
        if (now >= start + timeout_ms) {
            outcome = GSTD_PROVISION_DEADLINE;
            break;
        }
        struct pollfd fds[2] = {{pipes[0], POLLIN, 0}, {cancellation_fd, POLLIN, 0}};
        int ready = poll(fds, cancellation_fd >= 0 ? 2 : 1, (int)(start + timeout_ms - now));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            break;
        if (!ready) {
            outcome = GSTD_PROVISION_DEADLINE;
            break;
        }
        if (cancellation_fd >= 0 && fds[1].revents) {
            outcome = GSTD_PROVISION_CANCELLED;
            break;
        }
        ssize_t count = read(pipes[0], (unsigned char *)&output + have, sizeof(output) - have);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            break;
        have += (size_t)count;
    }
    close(pipes[0]);
    int status = 0, reaped = 0, gone = 0;
    while (have == sizeof(output)) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            reaped = 1;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            if (errno == ECHILD)
                gone = 1;
            break;
        }
        int64_t now = now_ms();
        if (now < 0 || now >= start + timeout_ms) {
            outcome = GSTD_PROVISION_DEADLINE;
            break;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    if (!reaped && !gone) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (reaped && WIFEXITED(status) && !WEXITSTATUS(status) && output.status <= GSTD_IMAGE_PANIC &&
        !output.reserved && output.result.downloaded_bytes <= download_limit &&
        output.result.merged_bytes <= merge_limit) {
        outcome = (gstd_provision_status)output.status;
        if (outcome == GSTD_PROVISION_OK) {
            size_t i = 0;
            while (i < 64 &&
                   ((output.result.tree_digest[i] >= '0' && output.result.tree_digest[i] <= '9') ||
                    (output.result.tree_digest[i] >= 'a' && output.result.tree_digest[i] <= 'f')))
                i++;
            if (i != 64 || output.result.tree_digest[64] ||
                output.result.logical_bytes > merge_limit ||
                output.result.staging_device != (uint64_t)info.st_dev ||
                output.result.staging_inode != (uint64_t)info.st_ino)
                outcome = GSTD_PROVISION_WORKER;
            else
                *result = output.result;
        }
    }
    return outcome;
#else
    (void)staging_fd;
    (void)source_fd;
    (void)mode;
    (void)auth;
    (void)cancellation_fd;
    json_decref(plan);
    return GSTD_PROVISION_UNAVAILABLE;
#endif
}

gstd_provision_status gstd_provision_stage(const json_t *manifest, const char *component,
                                           int staging_fd, const gstd_image_auth *auth,
                                           uint64_t download_limit, uint64_t merge_limit,
                                           unsigned timeout_ms, int cancellation_fd,
                                           gstd_provision_result *result) {
    return provision(manifest, component, -1, 0, staging_fd, auth, download_limit, merge_limit,
                     timeout_ms, cancellation_fd, result);
}
gstd_provision_status gstd_provision_import(const json_t *manifest, const char *component,
                                            int source_fd, int staging_fd, uint64_t download_limit,
                                            uint64_t merge_limit, unsigned timeout_ms,
                                            int cancellation_fd, gstd_provision_result *result) {
    if (source_fd < 0) {
        if (result)
            memset(result, 0, sizeof(*result));
        return GSTD_PROVISION_INVALID;
    }
    return provision(manifest, component, source_fd, 1, staging_fd, NULL, download_limit,
                     merge_limit, timeout_ms, cancellation_fd, result);
}

gstd_provision_status gstd_provision_resume(const json_t *manifest, const char *component,
                                            int cached_fd, int staging_fd,
                                            const gstd_image_auth *auth, uint64_t download_limit,
                                            uint64_t merge_limit, unsigned timeout_ms,
                                            int cancellation_fd, gstd_provision_result *result) {
    if (cached_fd < 0) {
        if (result)
            memset(result, 0, sizeof(*result));
        return GSTD_PROVISION_INVALID;
    }
    return provision(manifest, component, cached_fd, 2, staging_fd, auth, download_limit,
                     merge_limit, timeout_ms, cancellation_fd, result);
}

gstd_provision_status gstd_provision_materialize(const json_t *manifest, const char *component,
                                                 int cached_fd, int staging_fd,
                                                 uint64_t download_limit, uint64_t merge_limit,
                                                 unsigned timeout_ms, int cancellation_fd,
                                                 gstd_provision_result *result) {
    if (cached_fd < 0) {
        if (result)
            memset(result, 0, sizeof(*result));
        return GSTD_PROVISION_INVALID;
    }
    return provision(manifest, component, cached_fd, 3, staging_fd, NULL, download_limit,
                     merge_limit, timeout_ms, cancellation_fd, result);
}
