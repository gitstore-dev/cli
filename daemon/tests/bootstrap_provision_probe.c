#define _GNU_SOURCE 1
#include "bootstrap.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
/* Credential/file fixture only. Does not launch a workload or claim public
 * deployment acceptance, OS-store access or actual GitStore compatibility. */
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int run(pid_t parent, int scope, int broker, const char *path) {
    alarm(25);
    CHECK(!prctl(PR_SET_PDEATHSIG, SIGKILL) && getppid() == parent);
    int a = fcntl(scope, F_DUPFD_CLOEXEC, 32), b = fcntl(broker, F_DUPFD_CLOEXEC, 32);
    CHECK(a >= 32 && b >= 32 && dup2(a, 3) == 3 && dup2(b, 4) == 4);
    CHECK(!fcntl(3, F_SETFD, FD_CLOEXEC) && !fcntl(4, F_SETFD, FD_CLOEXEC));
    CHECK(!syscall(SYS_close_range, 5u, ~0u, 0u));
    struct rlimit core = {0, 0}, files = {67108864, 67108864};
    CHECK(!setrlimit(RLIMIT_CORE, &core) && !setrlimit(RLIMIT_FSIZE, &files));
    CHECK(!clearenv() && !setenv("PATH", "/nonexistent", 1) && !setenv("LANG", "C", 1));
    umask(077);
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller");
#if defined(__aarch64__)
    json_t *manifest = gstd_asset_manifest(components, "arm64");
#else
    json_t *manifest = gstd_asset_manifest(components, "amd64");
#endif
    json_decref(components);
    CHECK(manifest);
    gstd_bootstrap_request request = {.manifest = manifest,
                                      .store_id = "0123456789abcdef0123456789abcdef",
                                      .deployment_id = "bootstrap-fixture",
                                      .operation_id = "bootstrap-fixture-operation",
                                      .datastore = "memdb",
                                      .generation = 1,
                                      .control_owner = parent,
                                      .scope_fd = 3};
    json_t *source = json_pack("{s:s,s:s}", "kind", "protected-file", "path", path);
    CHECK(source && gstd_bootstrap_provision(&request, 4, source, 0) == -1);
    struct stat info;
    CHECK(lstat(path, &info) && errno == ENOENT);
    CHECK(!gstd_bootstrap_provision(&request, 4, source, 1));
    struct stat before, after;
    CHECK(!lstat(path, &before) && S_ISREG(before.st_mode) && before.st_uid == geteuid() &&
          (before.st_mode & 07777) == 0600 && before.st_nlink == 1);
    CHECK(!gstd_bootstrap_provision(&request, 4, source, 0));
    CHECK(!lstat(path, &after) && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
          before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
          before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
    // Preserve only synthetic private material for the runner's local TOML/
    // signing-record assertions. The runner owns and removes this exact fixture.
    json_decref(source);
    json_decref(manifest);
    return 0;
}
int main(int argc, char **argv) {
    CHECK(argc == 3 && geteuid() != 0);
    gstd_json_initialize();
    int root = open(argv[1], O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
        broker = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    CHECK(root >= 3 && broker >= 3 && !fstat(root, &info) && info.st_uid == geteuid() &&
          (info.st_mode & 07777) == 0700 && !mkdirat(root, "scope", 0700) &&
          !mkdirat(root, "credentials", 0700));
    int scope = openat(root, "scope", O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(scope >= 3);
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/credentials/bootstrap.json", argv[1]);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    pid_t parent = getpid(), child = fork();
    CHECK(child >= 0);
    if (!child)
        _exit(run(parent, scope, broker, path));
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(scope);
    close(broker);
    close(root);
    puts("private bootstrap broker: absent-on-restart refused, add-only provisioning and stable "
         "reuse pass");
    return 0;
}
