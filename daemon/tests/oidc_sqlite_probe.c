#define _GNU_SOURCE 1
/* Developer-only published-provider migration compatibility, never a daemon
 * method. Uses the existing native worker boundary and exact selected policy. */
#include "gitstored/execution.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "native_job.h"
#include "native_population.h"
#include "oidc_sqlite_probe.inc"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void pause_poll(void) {
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}
static int native(gstd_native_context *context, gstd_native_action action,
                  gstd_native_observation *observed) {
    gstd_native_job job = {0};
    if (gstd_native_launch(context, action, 15000, &job))
        return -1;
    gstd_native_status status;
    int done;
    while (!(done = gstd_native_poll(&job, -1, &status, observed)))
        pause_poll();
    if (done != 1 || status != GSTD_NATIVE_OK)
        fprintf(stderr, "native action=%d status=%d error=%d\n", (int)action,
                done == 1 ? (int)status : -1, observed->native_error);
    return done == 1 && status == GSTD_NATIVE_OK ? 0 : -1;
}
static int record(int directory, const char *name, json_t *value) {
    char *bytes = value ? gstd_json_dump(value) : NULL;
    if (!bytes)
        return -1;
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    size_t length = strlen(bytes);
    int valid = fd >= 3 && write(fd, bytes, length) == (ssize_t)length && !fsync(fd);
    if (fd >= 3)
        close(fd);
    free(bytes);
    return valid && !fsync(directory) ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc != 3 || !geteuid() || strnlen(argv[2], 3901) > 3900 || argv[2][0] != '/' ||
        (strcmp(argv[1], "hydra") && strcmp(argv[1], "kratos")))
        return 2;
    gstd_json_initialize();
    struct rlimit core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &core) || prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0))
        return 1;
    int owner = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (owner < 3 || fstat(owner, &info) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700 || mkdirat(owner, "bundle", 0700) ||
        mkdirat(owner, "native", 0700) || mkdirat(owner, "data", 0700))
        return 1;
    char rootfs[4096], data[4096], executable[64], id[65], boot[37];
    if (snprintf(rootfs, sizeof(rootfs), "%s/execution/rootfs", argv[2]) < 1 ||
        snprintf(data, sizeof(data), "%s/data", argv[2]) < 1 ||
        snprintf(executable, sizeof(executable), "/usr/bin/%s", argv[1]) < 1 ||
        snprintf(id, sizeof(id), "oidc-sqlite-%s-%ld", argv[1], (long)getpid()) < 1 ||
        gstd_native_boot_id(boot))
        return 1;
    json_t *configuration = gstd_json_parse((const unsigned char *)oidc_sqlite_probe_json,
                                            sizeof(oidc_sqlite_probe_json) - 1);
    json_t *policy = gstd_execution_policy();
    if (!configuration || !policy ||
        json_object_set_new(json_object_get(configuration, "root"), "path", json_string(rootfs)) ||
        json_array_set_new(json_object_get(json_object_get(configuration, "process"), "args"), 0,
                           json_string(executable)) ||
        json_object_set_new(json_array_get(json_object_get(configuration, "mounts"), 0), "source",
                            json_string(data)) ||
        json_object_set_new(
            json_array_get(json_object_get(json_object_get(configuration, "linux"), "uidMappings"),
                           0),
            "hostID", json_integer((json_int_t)geteuid())) ||
        json_object_set_new(
            json_array_get(json_object_get(json_object_get(configuration, "linux"), "gidMappings"),
                           0),
            "hostID", json_integer((json_int_t)getegid())) ||
        json_object_set(json_object_get(configuration, "linux"), "seccomp", policy))
        return 1;
    json_decref(policy);
    int bundle = openat(owner, "bundle", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int state = openat(owner, "native", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int image = open(rootfs, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int log =
        openat(owner, "provider.log", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (!configuration || bundle < 3 || state < 3 || image < 3 || log < 3 ||
        record(bundle, "config.json", configuration))
        return 1;
    gstd_native_context context = {.backend_id = id,
                                   .component = "fixture",
                                   .control_owner = getpid(),
                                   .state_fd = state,
                                   .bundle_fd = bundle,
                                   .rootfs_fd = image,
                                   .configuration = configuration,
                                   .test_fixture = 1,
                                   .stdout_fd = log,
                                   .stderr_fd = log,
                                   .writer_lease_fd = -1};
    gstd_native_observation observed;
    if (native(&context, GSTD_NATIVE_CREATE, &observed) || !observed.created || observed.pid <= 0 ||
        !observed.start_time)
        return 1;
    context.expected_pid = observed.pid;
    context.expected_start_time = observed.start_time;
    int death = -1;
    gstd_native_population population = GSTD_NATIVE_POPULATION_INITIAL;
    if (gstd_native_watch_open(observed.pid, observed.start_time, boot, &death) ||
        gstd_native_population_open(id, observed.pid, observed.start_time, boot, &population))
        return 1;
    json_t *identity = gstd_native_population_identity(&population, id, boot);
    json_t *created =
        json_pack("{s:s,s:s,s:i,s:I,s:O}", "id", id, "boot", boot, "pid", (int)observed.pid,
                  "startTime", (json_int_t)observed.start_time, "population", identity);
    int saved = record(owner, "created.json", created);
    json_decref(created);
    json_decref(identity);
    if (saved)
        return 1;
    int dispatched = native(&context, GSTD_NATIVE_START, &observed);
    int64_t now = now_ms();
    if (now < 0)
        return 1;
    int64_t deadline = now + 60000;
    gstd_native_exit exited = {0};
    int reaped = 0;
    while ((now = now_ms()) >= 0 && now < deadline) {
        reaped = gstd_native_exit_poll(context.expected_pid, context.expected_start_time, &exited);
        if (reaped)
            break;
        pause_poll();
    }
    int empty = gstd_native_population_empty(&population);
    int exact_dead = gstd_native_watch_poll(death);
    if (reaped != 1 || !exited.exited || exited.signalled || exited.code || empty != 1 ||
        exact_dead != 1) {
        printf("{\"compatible\":false,\"startUncertain\":%s,\"exitObserved\":%d,\"exitCode\":%d,"
               "\"signal\":%d,\"populationEmpty\":%d}\n",
               dispatched ? "true" : "false", reaped, exited.code, exited.signalled, empty);
        return 1; /* Preserve all original state/data; never signal a running migration. */
    }
    if (native(&context, GSTD_NATIVE_DELETE, &observed))
        return 1;
    json_t *completed = json_pack("{s:b,s:b,s:b,s:i}", "exitObserved", 1, "populationEmpty", 1,
                                  "deleted", 1, "exitCode", 0);
    int result = record(owner, "completed.json", completed);
    json_decref(completed);
    gstd_native_population_close(&population);
    close(death);
    close(log);
    close(image);
    close(state);
    close(bundle);
    close(owner);
    json_decref(configuration);
    if (result)
        return 1;
    puts("{\"compatible\":true,\"namespaceUid\":0,\"capabilities\":0,\"observedExitCode\":0,"
         "\"populationEmpty\":true,\"nativeDeleted\":true}");
    return 0;
}
