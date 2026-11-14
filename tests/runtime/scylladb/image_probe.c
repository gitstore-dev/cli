#define _GNU_SOURCE 1
/* Developer-only immutable image acquisition. Not a daemon method or executable
 * dependency. No commands are executed and no cache entry is activated. */
#include "gitstored/protocol.h"
#include "image_backend.h"
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
    if (argc != 2 || argv[1][0] != '/' || !geteuid())
        return 2;
    if (!gstd_image_backend_available())
        return 77;
#if defined(__aarch64__)
    const char *architecture = "arm64";
    const char *digest = "sha256:e65d9d7113cb5b324baa4cf982ab27f9ace4a2f11666942bd9a8c09293297768";
    const char *config = "sha256:7eb8c9cef67abe8d5d15cbb2e29cfdb64551e256e00acd1d0e33067feb8bbbba";
#elif defined(__x86_64__)
    const char *architecture = "amd64";
    const char *digest = "sha256:474d2cd76dc7d2edd6754ce82fd372a8bf8cce9de4bef000c55bcc843e3761a6";
    const char *config = "sha256:1dafda4ad7bf9e1257b57e51eb7a35c18598ed227ead3508cd09b5270a4a6c20";
#else
    return 77;
#endif
    struct rlimit core = {0, 0},
                  address = {UINT64_C(1024) * 1024 * 1024, UINT64_C(1024) * 1024 * 1024},
                  files = {256, 256},
                  size = {UINT64_C(8) * 1024 * 1024 * 1024, UINT64_C(8) * 1024 * 1024 * 1024};
    if (prctl(PR_SET_DUMPABLE, 0) || setrlimit(RLIMIT_CORE, &core) ||
        setrlimit(RLIMIT_AS, &address) || setrlimit(RLIMIT_NOFILE, &files) ||
        setrlimit(RLIMIT_FSIZE, &size) || clearenv() || setenv("PATH", "/nonexistent", 1) ||
        setenv("LANG", "C", 1))
        return 1;
    alarm(310);
    gstd_json_initialize();
    if (mkdir(argv[1], 0700))
        return 1;
    int stage = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (stage < 3 || fstat(stage, &info) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700) {
        if (stage >= 0)
            close(stage);
        return 1;
    }
    char reference[256];
    snprintf(reference, sizeof(reference), "docker.io/scylladb/scylla@%s", digest);
    json_t *plan = json_pack(
        "{s:i,s:s,s:s,s:s,s:s,s:[s],s:[s],s:I,s:I,s:i}", "schemaVersion", 1, "reference", reference,
        "configDigest", config, "os", "linux", "architecture", architecture, "tokenOrigins",
        "https://auth.docker.io", "blobOrigins", "https://production.cloudfront.docker.com",
        "maximumDownloadBytes", (json_int_t)(UINT64_C(512) * 1024 * 1024), "maximumMergedBytes",
        (json_int_t)(UINT64_C(2) * 1024 * 1024 * 1024), "timeoutMs", 300000);
    /* Exact origin observed by the anonymous pinned-image routing probe. */
    char *encoded = plan ? gstd_json_dump(plan) : NULL;
    json_decref(plan);
    gstd_image_result result = {0};
    gstd_image_auth anonymous = {0};
    gstd_image_status status = encoded
                                   ? gstd_image_stage((const unsigned char *)encoded,
                                                      strlen(encoded), stage, &anonymous, &result)
                                   : GSTD_IMAGE_INVALID;
    free(encoded);
    close(stage);
    printf("{\"imageStatus\":%d,\"downloadedBytes\":%" PRIu64 ",\"mergedBytes\":%" PRIu64 "}\n",
           (int)status, result.downloaded_bytes, result.merged_bytes);
    return status == GSTD_IMAGE_OK ? 0 : 1;
}
