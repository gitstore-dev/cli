#define _GNU_SOURCE 1
#include "filesystem_job.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "oidc_profile.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "OIDC port worker line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
#ifndef __linux__
    return 77;
#else
    gstd_json_initialize();
    char *root = gstd_test_dir();
    CHECK(root && geteuid());
    int directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(directory >= 3);
    json_t *components = json_pack("[s]", "oidc");
#if defined(__aarch64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    json_t *manifest = gstd_oidc_core_manifest(components, architecture, "memdb");
    gstd_filesystem_input input = {.action = GSTD_FS_OIDC_PORTS, .manifest = manifest,
        .store_id = "0123456789abcdef0123456789abcdef", .operation_id = "accepted",
        .deployment_id = "fixture", .resource_id = "owned-network",
        .descriptors = {directory, -1, -1}};
    gstd_filesystem_job worker = {0};
    gstd_filesystem_input invalid = input;
    invalid.manifest = NULL;
    CHECK(gstd_filesystem_launch(&invalid, 5000, &worker) == -1 && !worker.worker);
    for (unsigned mode = 0; mode < 2; mode++) {
        input.test_pause = mode;
        CHECK(!gstd_filesystem_launch(&input, 5000, &worker));
        int marker[2];
        CHECK(!pipe2(marker, O_CLOEXEC));
        if (mode) {
            int status;
            CHECK(waitpid(worker.worker, &status, WUNTRACED) == worker.worker && WIFSTOPPED(status));
            CHECK(write(marker[1], "s", 1) == 1);
        }
        gstd_filesystem_reply reply = {0};
        int ready;
        while (!(ready = gstd_filesystem_poll(&worker, mode ? marker[0] : -1, &reply)))
            usleep(1000);
        CHECK(ready == 1 && !reply.descriptor_count);
        if (mode) {
            char byte;
            CHECK(reply.status == -2 && read(marker[0], &byte, 1) == 1 && byte == 's');
        } else
            CHECK(!reply.status && !gstd_oidc_ports_check(&reply.oidc_ports));
        close(marker[0]); close(marker[1]);
        gstd_filesystem_reply_close(&reply);
    }
    json_decref(components); json_decref(manifest);
    close(directory);
    CHECK(!gstd_test_remove(root));
    return 0;
#endif
}
