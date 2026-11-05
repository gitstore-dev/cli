#define _GNU_SOURCE 1
/* Developer-only pinned-provider serving compatibility, never a daemon
 * method. Uses the existing native worker boundary and exact selected policy. */
#include "bootstrap_http.h"
#include "execution_plan.h"
#include "gitstored/execution.h"
#include "gitstored/protocol.h"
#include "helper.h"
#include "native_exit.h"
#include "native_job.h"
#include "native_population.h"
#include "oidc_config.h"
#include "oidc_enrollment.h"
#include "oidc_material.h"
#include "oidc_profile.h"
#include "oidc_sqlite_probe.inc"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <net/if.h>
#include <openssl/crypto.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "oidc_probe_support.h"
int main(int argc, char **argv) {
    if ((argc != 3 && argc != 4) || !geteuid() || strnlen(argv[2], 3901) > 3900 ||
        argv[2][0] != '/' ||
        (strcmp(argv[1], "hydra") && strcmp(argv[1], "kratos") && strcmp(argv[1], "ui") &&
         strcmp(argv[1], "bridge") && strcmp(argv[1], "smtp")))
        return 2;
    if (argc == 4 && !strcmp(argv[3], "recover-ui-stop")) {
        if (strcmp(argv[1], "ui"))
            return 2;
        gstd_json_initialize();
        return oidc_probe_recover_ui_stop(argv[2]);
    }
    const char *mode = argc == 4 ? argv[3] : "fixture";
    int registered = !strcmp(mode, "registered") ||
                     !strcmp(mode, "registered-lost-enrollment-reply") ||
                     !strcmp(mode, "registered-migrate");
    int migrate_first = !strcmp(mode, "registered-migrate");
    int lose_reply =
        !strcmp(mode, "lost-enrollment-reply") || !strcmp(mode, "registered-lost-enrollment-reply");
    if ((argc == 4 && !registered && !lose_reply) || (lose_reply && strcmp(argv[1], "hydra")))
        return 2;
    int sql = !strcmp(argv[1], "hydra") || !strcmp(argv[1], "kratos");
    if ((!sql && !registered) || (migrate_first && !sql))
        return 2;
    gstd_json_initialize();
    struct rlimit core = {0, 0}, files = {67108864, 67108864};
#ifndef GITSTORED_INSTRUMENTED
    if (registered) {
        struct rlimit address_space = {UINT64_C(512) * 1024 * 1024, UINT64_C(1) << 40};
        if (setrlimit(RLIMIT_AS, &address_space))
            return 1;
    }
#endif
    if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_FSIZE, &files) ||
        prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0))
        return 1;
    int owner = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (owner < 3 || fstat(owner, &info) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700 || mkdirat(owner, "bundle", 0700) ||
        mkdirat(owner, "native", 0700))
        return 1;
    if (registered && mkdirat(owner, "config", 0700))
        return 1;
    int data_fd = openat(owner, registered ? "config" : "data",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (data_fd < 3 || fstat(data_fd, &info) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700 ||
        ((sql || !strcmp(argv[1], "ui")) && oidc_probe_prepare(data_fd, owner, argv[1], 1)))
        return 1;
    close(data_fd);
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
    json_t *args =
        !strcmp(argv[1], "hydra")
            ? json_pack("[s,s,s,s,s,s]", executable, "serve", "all", "--config",
                        "/work/provider.json", "--dev")
            : json_pack("[s,s,s,s,s,s,s]", executable, "serve", "--config", "/work/provider.json",
                        "--dev", "--watch-courier", "--sqa-opt-out");
    if (!args || json_object_set_new(json_object_get(configuration, "process"), "args", args))
        return 1;
    /* Schema file shares this private fixture data mount; production uses an
     * independently owned read-only configuration role. */
    if (!strcmp(argv[1], "kratos")) {
        /* The fixed production path is provided as a second read-only bind. */
        json_t *mount =
            json_pack("{s:s,s:s,s:s,s:[s,s,s,s]}", "destination", "/run/gitstore-oidc", "type",
                      "bind", "source", data, "options", "rbind", "ro", "nosuid", "nodev");
        if (!mount || json_array_append_new(json_object_get(configuration, "mounts"), mount))
            return 1;
    }

    int bundle = openat(owner, "bundle", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int state = openat(owner, "native", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int image = open(rootfs, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int log =
        openat(owner, "provider.log", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    gstd_execution_inputs inputs = {0};
    for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
        inputs.role_fds[i] = -1;
    gstd_network_namespace registered_network = GSTD_NETWORK_NAMESPACE_INITIAL;
    gstd_oidc_ports public_ports = {24001, 24002, 24003, 24004, 24005};
    json_t *registered_manifest = NULL;
    int lease = -1;
    char component[32];
    snprintf(component, sizeof(component), "oidc-%s", argv[1]);
    if (registered) {
        if (gstd_network_namespace_open(&registered_network) != GSTD_NETWORK_OK)
            return 1;
#if defined(__aarch64__)
        registered_manifest = gstd_oidc_manifest("arm64");
#else
        registered_manifest = gstd_oidc_manifest("amd64");
#endif
        int config = openat(owner, "config", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (config < 3)
            return 1;
        if (sql || !strcmp(argv[1], "ui"))
            inputs.role_fds[GSTD_MOUNT_OVERLAY] =
                openat(config, !strcmp(argv[1], "ui") ? "ui.env" : "provider.json",
                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (!strcmp(argv[1], "kratos"))
            inputs.role_fds[GSTD_MOUNT_POLICY] =
                openat(config, "identity.schema.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        int resolver = openat(config, "resolv.conf",
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        static const char resolver_bytes[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
        if (resolver < 3 ||
            write(resolver, resolver_bytes, sizeof(resolver_bytes) - 1) !=
                sizeof(resolver_bytes) - 1 ||
            fsync(resolver))
            return 1;
        close(resolver);
        inputs.role_fds[GSTD_MOUNT_RESOLVER] =
            openat(config, "resolv.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        close(config);
        if (sql) {
            inputs.role_fds[GSTD_MOUNT_DATASTORE] =
                openat(owner, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            lease = openat(owner, "writer.lock", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                           0600);
            if (lease < 3 || flock(lease, LOCK_EX | LOCK_NB))
                return 1;
        }
        inputs.manifest = registered_manifest;
        inputs.component = component;
        inputs.purpose = GSTD_EXEC_SERVICE;
        inputs.rootfs_fd = image;
        inputs.network = &registered_network;
        inputs.oidc_ports = &public_ports;
        if (oidc_probe_adapter_policy_checks(&inputs))
            return 1;
        json_decref(configuration);
        configuration = gstd_execution_build(&inputs);
    }
    if (!configuration || bundle < 3 || state < 3 || image < 3 || log < 3 ||
        oidc_probe_record(bundle, "config.json", configuration))
        return 1;
    if (migrate_first &&
        oidc_probe_migrate_provider(owner, state, image, log, lease, id, boot, &inputs))
        return 1;
    gstd_native_context context = {.backend_id = id,
                                   .component = registered ? component : "fixture",
                                   .boot_id = registered ? boot : NULL,
                                   .control_owner = getpid(),
                                   .state_fd = state,
                                   .bundle_fd = bundle,
                                   .rootfs_fd = image,
                                   .configuration = configuration,
                                   .test_fixture = !registered,
                                   .registered_inputs = registered ? &inputs : NULL,
                                   .stdout_fd = log,
                                   .stderr_fd = log,
                                   .writer_lease_fd = lease};
    gstd_native_observation observed;
    if (oidc_probe_native(&context, GSTD_NATIVE_CREATE, &observed) || !observed.created ||
        observed.pid <= 0 || !observed.start_time)
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
    int saved = oidc_probe_record(owner, "created.json", created);
    json_decref(created);
    json_decref(identity);
    if (saved)
        return 1;
    int dispatched = oidc_probe_native(&context, GSTD_NATIVE_START, &observed);
    int64_t now = oidc_probe_now_ms();
    if (now < 0)
        return 1;
    int64_t health_deadline = now + 30000;
    int healthy = 0, last_health = -1;
    while ((now = oidc_probe_now_ms()) >= 0 && now < health_deadline &&
           !gstd_native_watch_poll(death)) {
        last_health = oidc_probe_ready(context.expected_pid, argv[1], &public_ports);
        if (!last_health) {
            healthy = 1;
            break;
        }
        oidc_probe_pause_poll();
    }
    int writer_required = registered && sql;
    int inherited = 0;
    if (writer_required && healthy) {
        struct stat held, remote;
        char inherited_path[128];
        snprintf(inherited_path, sizeof(inherited_path), "/proc/%ld/fd/3",
                 (long)context.expected_pid);
        inherited = !fstat(lease, &held) && !stat(inherited_path, &remote) &&
                    S_ISREG(remote.st_mode) && held.st_dev == remote.st_dev &&
                    held.st_ino == remote.st_ino && gstd_native_watch_poll(death) == 0;
        healthy = healthy && inherited;
    }
    json_t *health = json_pack("{s:b,s:b,s:i,s:b,s:b}", "ready", healthy, "startAcknowledged",
                               !dispatched, "lastHealth", last_health, "writerRequired",
                               writer_required, "writerInherited", inherited);
    int health_recorded = !oidc_probe_record(owner, "health.json", health);
    json_decref(health);
    int enrolled = healthy;
    if (healthy && !strcmp(argv[1], "hydra")) {
        int volume = registered
                         ? inputs.role_fds[GSTD_MOUNT_DATASTORE]
                         : openat(owner, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        enrolled = volume >= 3 && oidc_probe_enrollment(owner, owner, volume, context.expected_pid,
                                                        context.expected_start_time, boot,
                                                        lose_reply) == lose_reply;
        if (!registered && volume >= 3)
            close(volume);
    }
    if (!health_recorded || oidc_probe_native(&context, GSTD_NATIVE_TERM, &observed))
        return 1;
    int64_t deadline = oidc_probe_now_ms() + 15000;
    gstd_native_exit exited = {0};
    int reaped = 0;
    while ((now = oidc_probe_now_ms()) >= 0 && now < deadline) {
        reaped = gstd_native_exit_poll(context.expected_pid, context.expected_start_time, &exited);
        if (reaped)
            break;
        oidc_probe_pause_poll();
    }
    int empty = gstd_native_population_empty(&population);
    int exact_dead = gstd_native_watch_poll(death);
    if (reaped != 1 ||
        !(exited.exited ? exited.code == 0 : exited.signalled && exited.code == SIGTERM) ||
        empty != 1 || exact_dead != 1) {
        printf("{\"compatible\":false,\"startUncertain\":%s,\"exitObserved\":%d,\"exitCode\":%d,"
               "\"signal\":%d,\"populationEmpty\":%d}\n",
               dispatched ? "true" : "false", reaped, exited.code, exited.signalled, empty);
        return 1; /* Preserve uncertain workload and data. No forced deletion. */
    }
    if (oidc_probe_native(&context, GSTD_NATIVE_DELETE, &observed))
        return 1;
    json_t *completed = json_pack("{s:b,s:b,s:b,s:i}", "exitObserved", 1, "populationEmpty", 1,
                                  "deleted", 1, "exitCode", exited.code);
    int result = oidc_probe_record(owner, "completed.json", completed);
    json_decref(completed);
    gstd_native_population_close(&population);
    close(death);
    close(log);
    close(image);
    close(state);
    close(bundle);
    close(owner);
    json_decref(configuration);
    if (registered) {
        for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
            if (inputs.role_fds[i] >= 3)
                close(inputs.role_fds[i]);
        if (lease >= 3)
            close(lease);
        if (gstd_network_namespace_close(&registered_network) != GSTD_NETWORK_OK)
            return 1;
        json_decref(registered_manifest);
    }
    if (result || !healthy || !enrolled)
        return 1;
    printf("{\"ready\":true,\"lostEnrollmentReply\":%s,\"registeredAdapter\":%s,\"freshMigration\":"
           "%s,\"namespaceUid\":0,"
           "\"capabilities\":0,"
           "\"exitCode\":%d,\"signalled\":%d,"
           "\"populationEmpty\":true,\"nativeDeleted\":true}\n",
           lose_reply ? "true" : "false", registered ? "true" : "false",
           migrate_first ? "true" : "false", exited.code, exited.signalled);
    return 0;
}
