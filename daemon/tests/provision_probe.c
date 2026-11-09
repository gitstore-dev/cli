#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "gitstored/provisioning.h"
#include "image_backend.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#endif
static gstd_provision_status direct_bytes(const json_t *manifest, const char *component, int stage,
                                          int source, unsigned timeout,
                                          gstd_provision_result *result) {
#ifdef __linux__
    struct rlimit core = {0, 0},
                  address = {UINT64_C(1024) * 1024 * 1024, UINT64_C(1024) * 1024 * 1024},
                  files = {128, 128},
                  size = {UINT64_C(8) * 1024 * 1024 * 1024, UINT64_C(8) * 1024 * 1024 * 1024};
    if (prctl(PR_SET_DUMPABLE, 0) || setrlimit(RLIMIT_CORE, &core) ||
        setrlimit(RLIMIT_AS, &address) || setrlimit(RLIMIT_NOFILE, &files) ||
        setrlimit(RLIMIT_FSIZE, &size))
        return GSTD_PROVISION_UNAVAILABLE;
    alarm(timeout / 1000 + 2);
    json_t *plan = gstd_provision_plan(manifest, component, UINT64_C(128) * 1024 * 1024,
                                       UINT64_C(512) * 1024 * 1024, timeout);
    char *bytes = plan ? gstd_json_dump(plan) : NULL;
    json_decref(plan);
    if (!bytes)
        return GSTD_PROVISION_INVALID;
    gstd_image_auth auth = {0};
    gstd_image_result acquired = {0};
    gstd_image_status status = source >= 0
                                   ? gstd_image_materialize((const unsigned char *)bytes,
                                                            strlen(bytes), source, stage, &acquired)
                                   : gstd_image_stage((const unsigned char *)bytes, strlen(bytes),
                                                      stage, &auth, &acquired);
    free(bytes);
    result->downloaded_bytes = acquired.downloaded_bytes;
    result->merged_bytes = acquired.merged_bytes;
    return status <= GSTD_IMAGE_PANIC ? (gstd_provision_status)status : GSTD_PROVISION_WORKER;
#else
    (void)manifest;
    (void)component;
    (void)stage;
    (void)source;
    (void)timeout;
    (void)result;
    return GSTD_PROVISION_UNAVAILABLE;
#endif
}
/* Explicit opt-in developer staging probe. Never activates or executes images.
 */
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5 &&
        !(argc == 6 && (!strcmp(argv[5], "resume") || !strcmp(argv[5], "materialize") ||
                        !strcmp(argv[5], "materialize-sdk"))))
        return 2;
    char *end = NULL;
    errno = 0;
    unsigned long timeout = strtoul(argv[3], &end, 10);
    if (errno || !*argv[3] || *end || timeout < 1 || timeout > 600000)
        return 2;
    gstd_json_initialize();
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    /* Data-only developer experiment. Never activates/executes an image or
     * changes production platform selection. SDK rechecks registered identity. */
    const char *selected_architecture = getenv("GITSTORED_PROBE_ARCH");
    int foreign = selected_architecture && strcmp(selected_architecture, architecture);
    if (foreign && argc != 4)
        return 2;
    if (selected_architecture) {
        if (strcmp(selected_architecture, "amd64") && strcmp(selected_architecture, "arm64"))
            return 2;
        architecture = selected_architecture;
    }
    json_t *components = json_pack("[s]", argv[1]);
    json_t *manifest = components ? gstd_asset_manifest(components, architecture) : NULL;
    int staging = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    gstd_image_auth auth = {0};
    gstd_provision_result result = {0};
    int source = argc >= 5 ? open(argv[4], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
    int sdk_materialize = argc == 6 && !strcmp(argv[5], "materialize-sdk");
    gstd_provision_status status =
        sdk_materialize && source < 0 ? GSTD_PROVISION_INVALID
        : foreign || sdk_materialize
            ? direct_bytes(manifest, argv[1], staging, source, (unsigned)timeout, &result)
        : argc == 6 && !strcmp(argv[5], "materialize")
            ? gstd_provision_materialize(manifest, argv[1], source, staging,
                                         UINT64_C(128) * 1024 * 1024, UINT64_C(512) * 1024 * 1024,
                                         (unsigned)timeout, -1, &result)
        : argc == 6
            ? gstd_provision_resume(manifest, argv[1], source, staging, &auth,
                                    UINT64_C(128) * 1024 * 1024, UINT64_C(512) * 1024 * 1024,
                                    (unsigned)timeout, -1, &result)
        : argc == 5
            ? gstd_provision_import(manifest, argv[1], source, staging, UINT64_C(128) * 1024 * 1024,
                                    UINT64_C(512) * 1024 * 1024, (unsigned)timeout, -1, &result)
            : gstd_provision_stage(manifest, argv[1], staging, &auth, UINT64_C(128) * 1024 * 1024,
                                   UINT64_C(512) * 1024 * 1024, (unsigned)timeout, -1, &result);
    printf("{\"status\":%u,\"downloadedBytes\":%" PRIu64 ",\"mergedBytes\":%" PRIu64
           ",\"logicalBytes\":%" PRIu64 ",\"treeDigest\":\"%s\"}\n",
           (unsigned)status, result.downloaded_bytes, result.merged_bytes, result.logical_bytes,
           result.tree_digest);
    if (staging >= 0)
        close(staging);
    if (source >= 0)
        close(source);
    json_decref(manifest);
    json_decref(components);
    return status == GSTD_PROVISION_OK ? 0 : 1;
}
