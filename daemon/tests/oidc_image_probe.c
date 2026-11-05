#define _GNU_SOURCE 1
/* Developer-only immutable OIDC dependency acquisition; inputs from optional-evidence.json. Not a
 * daemon method or executable dependency. No commands are executed and no cache entry is activated.
 */
#include "gitstored/protocol.h"
#include "gitstored/provisioning.h"
#include "image_backend.h"
#include "oidc_profile.h"
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if ((argc != 3 && argc != 4) || argv[2][0] != '/' || !geteuid())
        return 2;
    if (!gstd_image_backend_available())
        return 77;
    const char *names[] = {"hydra", "kratos", "smtp", "ui", "bridge"};
    unsigned selected;
    for (selected = 0; selected < 5; selected++)
        if (!strcmp(argv[1], names[selected]))
            break;
    if (selected == 5)
        return 2;
#if defined(__aarch64__)
    const char *architecture = "arm64";
#elif defined(__x86_64__)
    const char *architecture = "amd64";
#else
    return 77;
#endif
    struct rlimit core = {0, 0},
                  address = {UINT64_C(1024) * 1024 * 1024, UINT64_C(1024) * 1024 * 1024},
                  files = {128, 128},
                  size = {UINT64_C(1024) * 1024 * 1024, UINT64_C(1024) * 1024 * 1024};
    if (prctl(PR_SET_DUMPABLE, 0) || setrlimit(RLIMIT_CORE, &core) ||
        setrlimit(RLIMIT_AS, &address) || setrlimit(RLIMIT_NOFILE, &files) ||
        setrlimit(RLIMIT_FSIZE, &size) || clearenv() || setenv("PATH", "/nonexistent", 1) ||
        setenv("LANG", "C", 1))
        return 1;
    alarm(100);
    gstd_json_initialize();
    if (mkdir(argv[2], 0700))
        return 1;
    int stage = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (stage < 3 || fstat(stage, &info) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700) {
        if (stage >= 0)
            close(stage);
        return 1;
    }
    char component[32];
    snprintf(component, sizeof(component), "oidc-%s", names[selected]);
    json_t *manifest = gstd_oidc_manifest_component(architecture, component);
    json_t *plan = manifest ? gstd_provision_plan(manifest, component, UINT64_C(512) * 1024 * 1024,
                                                  UINT64_C(1024) * 1024 * 1024, 90000)
                            : NULL;
    json_decref(manifest);
    /* Current Docker origin allowlist and prior anonymous SDK routing evidence.
     * No Docker credential store or upstream attestation is required. */
    char *encoded = plan ? gstd_json_dump(plan) : NULL;
    json_decref(plan);
    gstd_image_result result = {0};
    gstd_image_auth anonymous = {0};
    int source = argc == 4 ? open(argv[3], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
    gstd_image_status status = !encoded ? GSTD_IMAGE_INVALID
                               : argc == 4
                                   ? gstd_image_materialize((const unsigned char *)encoded,
                                                            strlen(encoded), source, stage, &result)
                                   : gstd_image_stage((const unsigned char *)encoded,
                                                      strlen(encoded), stage, &anonymous, &result);
    if (source >= 3)
        close(source);
    free(encoded);
    close(stage);
    printf("{\"imageStatus\":%d,\"mode\":\"%s\",\"compressedBytes\":%" PRIu64
           ",\"mergedBytes\":%" PRIu64 "}\n",
           (int)status, argc == 4 ? "cache-materialize" : "registry-stage", result.downloaded_bytes,
           result.merged_bytes);
    return status == GSTD_IMAGE_OK ? 0 : 1;
}
