#define _GNU_SOURCE 1
#include "bootstrap_job.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "oidc_profile.h"
#include "support.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "bootstrap job line %d\n", __LINE__);                                  \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int finish(gstd_bootstrap_job *job, int cancellation, gstd_bootstrap_job_reply *reply) {
    for (unsigned i = 0; i < 2000; i++) {
        int ready = gstd_bootstrap_job_poll(job, cancellation, reply);
        if (ready)
            return ready;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    return -1;
}
static int acknowledged_stop_race(unsigned mode) {
    int reports[2], cancellation[2];
    CHECK(!pipe2(reports, O_CLOEXEC | O_NONBLOCK) && !pipe2(cancellation, O_CLOEXEC));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(reports[0]);
        gstd_bootstrap_job_reply reply = {.status = mode == 2 ? GSTD_OIDC_PUBLIC_MATCH
                                                              : GSTD_OIDC_CLIENT_CREATED};
        if (mode == 1)
            reply.uid[0] = 'x';
        if (write(reports[1], &reply, sizeof(reply)) != sizeof(reply))
            _exit(1);
        if (mode == 3)
            raise(SIGSTOP); /* A pipe reply alone is not an observed completion. */
        _exit(0);
    }
    close(reports[1]);
    if (mode == 3) {
        int status;
        CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    } else {
        siginfo_t info = {0};
        CHECK(!waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) && info.si_pid == child);
    }
    CHECK(write(cancellation[1], "s", 1) == 1);
    gstd_bootstrap_job job = {.worker = child, .descriptor = reports[0], .deadline_ms = 0,
                              .kind = GSTD_BOOT_JOB_OIDC_HTTP, .oidc_action = GSTD_OIDC_HTTP_CREATE};
    gstd_bootstrap_job_reply reply;
    CHECK(finish(&job, cancellation[0], &reply) == 1 &&
          reply.status == (mode == 0 ? GSTD_OIDC_CLIENT_CREATED : GSTD_OIDC_UNCERTAIN));
    char marker;
    CHECK(read(cancellation[0], &marker, 1) == 1 && marker == 's');
    close(cancellation[0]);
    close(cancellation[1]);
    return 0;
}
int main(void) {
    alarm(10);
    for (unsigned mode = 0; mode < 4; mode++)
        CHECK(!acknowledged_stop_race(mode));
    gstd_json_initialize();
    char *root = gstd_test_dir();
    CHECK(root);
    int directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    /* Invalid broker deliberately proves these boundary tests cannot consult an
     * OS store or create a secret. Actual broker flow has a separate native run. */
    int broker = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(directory >= 3 && broker >= 3);
    json_t *components = json_pack("[s,s]", "api", "git");
#if defined(__aarch64__)
    json_t *manifest = gstd_asset_manifest(components, "arm64");
#else
    json_t *manifest = gstd_asset_manifest(components, "amd64");
#endif
    json_t *source = json_pack("{s:s}", "kind", "os-store");
    gstd_bootstrap_job_input input = {
        .preparation = {.manifest = manifest,
                        .store_id = "0123456789abcdef0123456789abcdef",
                        .deployment_id = "fixture",
                        .operation_id = "fixture-op",
                        .datastore = "memdb",
                        .generation = 1,
                        .control_owner = getpid(),
                        .scope_fd = directory},
        .broker_fd = broker,
        .source = source,
        .test_pause = 1};
    CHECK(manifest && source);
    gstd_bootstrap_job job = {0};
    gstd_bootstrap_job_reply reply;
    gstd_bootstrap_job_input invalid = input;
    invalid.preparation.control_owner++;
    CHECK(gstd_bootstrap_job_launch(&invalid, 5000, &job) == -1 && !job.worker);
    json_t *oidc_components = json_pack("[s]", "oidc");
#if defined(__aarch64__)
    json_t *oidc_manifest = gstd_oidc_core_manifest(oidc_components, "arm64", "memdb");
#else
    json_t *oidc_manifest = gstd_oidc_core_manifest(oidc_components, "amd64", "memdb");
#endif
    CHECK(oidc_manifest);
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    for (unsigned mode = 0; mode < 9; mode++) {
        if (mode == 3) {
            input.kind = GSTD_BOOT_JOB_OIDC_HTTP;
            input.preparation.manifest = oidc_manifest;
            input.network = &network;
            input.oidc_ports = (gstd_oidc_ports){24001, 24002, 24003, 24004, 24005};
            invalid = input;
            invalid.preparation.manifest = manifest;
            CHECK(gstd_bootstrap_job_launch(&invalid, 5000, &job) == -1 && !job.worker);
            invalid = input;
            invalid.allow_initial = 1;
            CHECK(gstd_bootstrap_job_launch(&invalid, 5000, &job) == -1 && !job.worker);
            invalid = input;
            invalid.oidc_ports.ui = 4445;
            CHECK(gstd_bootstrap_job_launch(&invalid, 5000, &job) == -1 && !job.worker);
            invalid = input;
            invalid.oidc_action = GSTD_OIDC_HTTP_CREATE;
            CHECK(gstd_bootstrap_job_launch(&invalid, 5000, &job) == -1 && !job.worker);
        }
        if (mode == 6) {
            input.kind = GSTD_BOOT_JOB_OIDC_HEALTH;
            input.broker_fd = -1;
            input.source = NULL;
            input.oidc_health_role = GSTD_OIDC_HEALTH_HYDRA;
            invalid = input;
            invalid.oidc_health_role = (gstd_oidc_health_role)99;
            CHECK(gstd_bootstrap_job_launch(&invalid, 5000, &job) == -1 && !job.worker);
            invalid = input;
            invalid.oidc_ports.ui = invalid.oidc_ports.mail;
            CHECK(gstd_bootstrap_job_launch(&invalid, 5000, &job) == -1 && !job.worker);
            invalid = input;
            invalid.preparation.manifest = manifest;
            CHECK(gstd_bootstrap_job_launch(&invalid, 5000, &job) == -1 && !job.worker);
        }
        unsigned failure = mode % 3;
        CHECK(!gstd_bootstrap_job_launch(&input, failure == 2 ? 1 : 5000, &job));
        int status;
        CHECK(waitpid(job.worker, &status, WUNTRACED) == job.worker && WIFSTOPPED(status));
        int pipes[2];
        CHECK(!pipe2(pipes, O_CLOEXEC));
        if (failure == 0)
            CHECK(write(pipes[1], "x", 1) == 1);
        else if (failure == 1)
            CHECK(!kill(job.worker, SIGSEGV) && !kill(job.worker, SIGCONT));
        else {
            struct timespec pause = {0, 2000000};
            nanosleep(&pause, NULL);
        }
        CHECK(finish(&job, failure == 0 ? pipes[0] : -1, &reply) == 1 &&
              reply.status == GSTD_BOOT_HTTP_UNCERTAIN && !*reply.uid);
        if (failure == 0) {
            char marker;
            CHECK(read(pipes[0], &marker, 1) == 1 && marker == 'x');
        }
        close(pipes[0]);
        close(pipes[1]);
        CHECK(!job.worker && job.descriptor == -1);
        CHECK(faccessat(directory, "owner.json", F_OK, 0) != 0);
    }
    input.test_pause = 0;
    CHECK(!gstd_bootstrap_job_launch(&input, 5000, &job));
    CHECK(finish(&job, -1, &reply) == 1 && reply.status == GSTD_OIDC_REJECTED && !*reply.uid);
    CHECK(faccessat(directory, "prepared.json", F_OK, 0) != 0 &&
          faccessat(directory, "owner.json", F_OK, 0) != 0);
    json_decref(oidc_components);
    json_decref(oidc_manifest);
    json_decref(components);
    json_decref(manifest);
    json_decref(source);
    close(directory);
    close(broker);
    CHECK(!gstd_test_remove(root));
    return 0;
}
