#define _GNU_SOURCE 1
#include "bootstrap_job.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
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
int main(void) {
    alarm(10);
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
    for (unsigned mode = 0; mode < 3; mode++) {
        CHECK(!gstd_bootstrap_job_launch(&input, mode == 2 ? 1 : 5000, &job));
        int status;
        CHECK(waitpid(job.worker, &status, WUNTRACED) == job.worker && WIFSTOPPED(status));
        int pipes[2];
        CHECK(!pipe2(pipes, O_CLOEXEC));
        if (mode == 0)
            CHECK(write(pipes[1], "x", 1) == 1);
        else if (mode == 1)
            CHECK(!kill(job.worker, SIGSEGV) && !kill(job.worker, SIGCONT));
        else {
            struct timespec pause = {0, 2000000};
            nanosleep(&pause, NULL);
        }
        CHECK(finish(&job, mode == 0 ? pipes[0] : -1, &reply) == 1 &&
              reply.status == GSTD_BOOT_HTTP_UNCERTAIN && !*reply.uid);
        if (mode == 0) {
            char marker;
            CHECK(read(pipes[0], &marker, 1) == 1 && marker == 'x');
        }
        close(pipes[0]);
        close(pipes[1]);
        CHECK(!job.worker && job.descriptor == -1);
        CHECK(faccessat(directory, "owner.json", F_OK, 0) != 0);
    }
    json_decref(components);
    json_decref(manifest);
    json_decref(source);
    close(directory);
    close(broker);
    CHECK(!gstd_test_remove(root));
    return 0;
}
