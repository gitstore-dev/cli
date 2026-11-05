#define _GNU_SOURCE 1
#include "gitstored/protocol.h"
#include "namespace_owner.h"
#include "native_exit.h"
#include "native_population.h"
#include "oidc_probe_support.h"
#include "oidc_profile.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC graph line %d\n", __LINE__);                                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
typedef struct {
    const char *name;
    char component[32], id[65];
    int root, config, bundle, state, image, log, lease, death;
    gstd_execution_inputs input;
    gstd_native_context native;
    gstd_native_observation observed;
    gstd_native_population population;
} service;
static int directory(int parent, const char *name, int create) {
    if (create && mkdirat(parent, name, 0700))
        return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 3)
        return -1;
    if (fstat(fd, &st) || st.st_uid != geteuid() || !S_ISDIR(st.st_mode) ||
        (st.st_mode & 07777) != 0700) {
        close(fd);
        return -1;
    }
    return fd;
}
static int resolver(int directory) {
    int fd = openat(directory, "resolv.conf", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
    static const char text[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
    if (fd < 3 || write(fd, text, sizeof(text) - 1) != sizeof(text) - 1 || fsync(fd))
        return -1;
    close(fd);
    return openat(directory, "resolv.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}
static int start(service *s, int owner, const json_t *manifest,
                 const gstd_network_namespace *network, const gstd_oidc_ports *ports,
                 const char *boot, int ordinal) {
    int sql = !strcmp(s->name, "hydra") || !strcmp(s->name, "kratos");
    s->root = directory(owner, s->name, 0);
    if (s->root < 3)
        return -1;
    s->config = directory(s->root, "config", 1);
    s->bundle = directory(s->root, "bundle", 1);
    s->state = directory(s->root, "native", 1);
    if (s->config < 3 || s->bundle < 3 || s->state < 3)
        return -1;
    if ((sql || !strcmp(s->name, "ui")) &&
        oidc_probe_prepare(s->config, owner, s->name, ordinal == 1))
        return -1;
    int execution = openat(s->root, "execution", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    s->image = execution < 3
                   ? -1
                   : openat(execution, "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (execution >= 3)
        close(execution);
    s->log =
        openat(s->root, "service.log", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (s->image < 3 || s->log < 3)
        return -1;
    snprintf(s->component, sizeof(s->component), "oidc-%s", s->name);
    snprintf(s->id, sizeof(s->id), "oidc-graph-%s-%ld", s->name, (long)getpid());
    s->input = (gstd_execution_inputs){.manifest = manifest,
                                       .component = s->component,
                                       .purpose = GSTD_EXEC_SERVICE,
                                       .rootfs_fd = s->image,
                                       .network = network,
                                       .oidc_ports = ports};
    for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
        s->input.role_fds[i] = -1;
    s->input.role_fds[GSTD_MOUNT_RESOLVER] = resolver(s->config);
    if (sql || !strcmp(s->name, "ui"))
        s->input.role_fds[GSTD_MOUNT_OVERLAY] =
            openat(s->config, !strcmp(s->name, "ui") ? "ui.env" : "provider.json",
                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (!strcmp(s->name, "kratos"))
        s->input.role_fds[GSTD_MOUNT_POLICY] =
            openat(s->config, "identity.schema.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    s->lease = -1;
    if (sql) {
        s->input.role_fds[GSTD_MOUNT_DATASTORE] = directory(s->root, "data", 1);
        s->lease = openat(s->root, "writer.lock",
                          O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (s->lease < 3 || flock(s->lease, LOCK_EX | LOCK_NB))
            return -1;
    }
    if (oidc_probe_adapter_policy_checks(&s->input))
        return -1;
    if (sql && oidc_probe_migrate_provider(s->root, s->state, s->image, s->log, s->lease, s->id,
                                           boot, &s->input))
        return -1;
    json_t *configuration = gstd_execution_build(&s->input);
    if (!configuration || oidc_probe_record(s->bundle, "config.json", configuration))
        return -1;
    s->native = (gstd_native_context){.backend_id = s->id,
                                      .component = s->component,
                                      .boot_id = boot,
                                      .state_fd = s->state,
                                      .bundle_fd = s->bundle,
                                      .rootfs_fd = s->image,
                                      .stdout_fd = s->log,
                                      .stderr_fd = s->log,
                                      .writer_lease_fd = s->lease,
                                      .control_owner = getpid(),
                                      .configuration = configuration,
                                      .registered_inputs = &s->input};
    struct stat rootfs, bundle;
    if (fstat(s->image, &rootfs) || fstat(s->bundle, &bundle))
        return -1;
    json_t *intent = json_pack("{s:s,s:s,s:I,s:I,s:I,s:I}", "id", s->id, "component", s->component,
                               "rootfsDevice", (json_int_t)rootfs.st_dev, "rootfsInode",
                               (json_int_t)rootfs.st_ino, "bundleDevice", (json_int_t)bundle.st_dev,
                               "bundleInode", (json_int_t)bundle.st_ino);
    int saved = oidc_probe_record(s->root, "service-dispatched.json", intent);
    json_decref(intent);
    if (saved || oidc_probe_native(&s->native, GSTD_NATIVE_CREATE, &s->observed) ||
        !s->observed.created)
        return -1;
    s->native.expected_pid = s->observed.pid;
    s->native.expected_start_time = s->observed.start_time;
    s->death = -1;
    s->population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
    if (gstd_native_watch_open(s->native.expected_pid, s->native.expected_start_time, boot,
                               &s->death) ||
        gstd_native_population_open(s->id, s->native.expected_pid, s->native.expected_start_time,
                                    boot, &s->population))
        return -1;
    json_t *population = gstd_native_population_identity(&s->population, s->id, boot);
    json_t *created = json_pack(
        "{s:s,s:s,s:i,s:I,s:O}", "id", s->id, "boot", boot, "pid", (int)s->native.expected_pid,
        "startTime", (json_int_t)s->native.expected_start_time, "population", population);
    saved = !created || oidc_probe_record(s->root, "created.json", created);
    json_decref(created);
    json_decref(population);
    if (saved || oidc_probe_native(&s->native, GSTD_NATIVE_START, &s->observed))
        return -1;
    int healthy = 0, last = -1;
    int64_t deadline = oidc_probe_now_ms() + 30000;
    while (oidc_probe_now_ms() < deadline && gstd_native_watch_poll(s->death) == 0) {
        last = oidc_probe_ready(s->native.expected_pid, s->name, ports);
        if (!last) {
            healthy = 1;
            break;
        }
        oidc_probe_pause_poll();
    }
    int inherited = 0;
    if (healthy && sql) {
        struct stat expected, actual;
        char path[128];
        snprintf(path, sizeof(path), "/proc/%ld/fd/3", (long)s->native.expected_pid);
        inherited = !fstat(s->lease, &expected) && !stat(path, &actual) &&
                    expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino;
        healthy = healthy && inherited;
    }
    json_t *health = json_pack("{s:b,s:i,s:b,s:b}", "ready", healthy, "lastHealth", last,
                               "writerRequired", sql, "writerInherited", inherited);
    saved = oidc_probe_record(s->root, "health.json", health);
    json_decref(health);
    return !saved && healthy &&
                   !gstd_native_population_matches(&s->population, s->id, s->native.expected_pid,
                                                   s->native.expected_start_time, boot)
               ? 0
               : -1;
}
static int stop(service *s) {
    json_t *intent = json_pack("{s:s,s:i,s:I}", "id", s->id, "pid", (int)s->native.expected_pid,
                               "startTime", (json_int_t)s->native.expected_start_time);
    int saved = oidc_probe_record(s->root, "stop-intent.json", intent);
    json_decref(intent);
    if (saved || oidc_probe_native(&s->native, GSTD_NATIVE_TERM, &s->observed))
        return -1;
    int64_t deadline = oidc_probe_now_ms() + 15000;
    gstd_native_exit exited = {0};
    int reaped = 0;
    while (oidc_probe_now_ms() < deadline) {
        reaped =
            gstd_native_exit_poll(s->native.expected_pid, s->native.expected_start_time, &exited);
        if (reaped)
            break;
        oidc_probe_pause_poll();
    }
    if (reaped != 1 ||
        !(exited.exited ? exited.code == 0 : exited.signalled && exited.code == SIGTERM) ||
        gstd_native_watch_poll(s->death) != 1 ||
        gstd_native_population_empty(&s->population) != 1 ||
        oidc_probe_native(&s->native, GSTD_NATIVE_DELETE, &s->observed))
        return -1;
    json_t *done = json_pack("{s:b,s:b,s:b,s:i}", "exitObserved", 1, "populationEmpty", 1,
                             "nativeDeleted", 1, "exitCode", exited.code);
    saved = oidc_probe_record(s->root, "completed.json", done);
    json_decref(done);
    if (saved)
        return -1;
    gstd_native_population_close(&s->population);
    close(s->death);
    for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
        if (s->input.role_fds[i] >= 3)
            close(s->input.role_fds[i]);
    if (s->lease >= 3)
        close(s->lease);
    close(s->log);
    close(s->image);
    close(s->state);
    close(s->bundle);
    close(s->config);
    close(s->root);
    json_decref((json_t *)s->native.configuration);
    return 0;
}
int main(int argc, char **argv) {
    CHECK((argc == 2 || argc == 4) && geteuid() != 0 && argv[1][0] == '/');
    gstd_json_initialize();
    struct rlimit core = {0, 0}, files = {67108864, 67108864};
    CHECK(!setrlimit(RLIMIT_CORE, &core) && !setrlimit(RLIMIT_FSIZE, &files));
#ifndef GITSTORED_INSTRUMENTED
    struct rlimit memory = {UINT64_C(512) * 1024 * 1024, UINT64_C(1) << 40};
    CHECK(!setrlimit(RLIMIT_AS, &memory));
#endif
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    CHECK(root >= 3 && !fstat(root, &st) && st.st_uid == geteuid() && (st.st_mode & 07777) == 0700);
    char boot[37];
    CHECK(!gstd_native_boot_id(boot));
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    CHECK(gstd_network_namespace_open(&network) == GSTD_NETWORK_OK);
#if defined(__aarch64__)
    json_t *manifest = gstd_oidc_manifest("arm64");
#else
    json_t *manifest = gstd_oidc_manifest("amd64");
#endif
    CHECK(manifest);
    gstd_oidc_ports ports = {24001, 24002, 24003, 24004, 24005};
    service roles[5] = {{.name = "smtp"},
                        {.name = "hydra"},
                        {.name = "kratos"},
                        {.name = "ui"},
                        {.name = "bridge"}};
    for (int i = 0; i < 5; i++)
        CHECK(!start(&roles[i], root, manifest, &network, &ports, boot, i));
    CHECK(!oidc_probe_enrollment(root, root, roles[1].input.role_fds[GSTD_MOUNT_DATASTORE],
                                 roles[1].native.expected_pid, roles[1].native.expected_start_time,
                                 boot, 0));
    json_t *ready = json_pack("{s:i,s:b,s:b,s:b}", "servicesReady", 5, "sharedOwnedNamespace", 1,
                              "providerMigrationsDeleted", 1, "clientCreated", 1);
    CHECK(ready && !oidc_probe_record(root, "graph-ready.json", ready));
    json_decref(ready);
    int courier = argc == 4 ? oidc_probe_courier(root, argv[1], &network, argv[2], argv[3]) : 0;
    for (int i = 4; i >= 0; i--)
        CHECK(!stop(&roles[i]));
    CHECK(gstd_network_namespace_close(&network) == GSTD_NETWORK_OK);
    json_t *completed = json_pack("{s:i,s:b,s:b}", "servicesDeleted", 5, "namespaceKeeperExited", 1,
                                  "dataRetained", 1);
    CHECK(completed && !oidc_probe_record(root, "graph-completed.json", completed));
    json_decref(completed);
    json_decref(manifest);
    close(root);
    CHECK(!courier);
    puts("{\"servicesReady\":5,\"clientCreated\":true,\"servicesDeleted\":5,"
         "\"sharedNamespaceClosed\":true,\"dataRetained\":true}");
    return 0;
}
