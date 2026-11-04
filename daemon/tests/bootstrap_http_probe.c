#define _GNU_SOURCE 1
#include "bootstrap_http.h"
#include "gitstored/protocol.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
/* Controlled HTTP fixture only, deliberately absent from production builds. */
static int run(pid_t parent, gstd_bootstrap_http_action action, int descriptor) {
    alarm(8);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
        return 1;
    struct rlimit core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &core))
        return 1;
    gstd_credential material = {0};
    struct stat info;
    if (action == GSTD_BOOT_GIT_READY || action == GSTD_BOOT_CONTROLLER_ENROLL) {
        if (fstat(descriptor, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
            (info.st_mode & 07777) != 0600 || info.st_nlink != 1 || info.st_size <= 0 ||
            info.st_size > 4096)
            return 1;
        material.bytes = malloc((size_t)info.st_size);
        if (!material.bytes ||
            pread(descriptor, material.bytes, (size_t)info.st_size, 0) != info.st_size)
            return 1;
        material.length = (size_t)info.st_size;
    }
    gstd_bootstrap_http_context context = {.control_owner = parent, .test_fixture = 1};
    char uid[129] = {0};
    gstd_bootstrap_http_status status = gstd_bootstrap_http(&context, action, &material, uid);
    gstd_credential_clear(&material);
    // UID is public identity; no raw response, token or input is emitted.
    printf("{\"status\":%d,\"uid\":\"%s\"}\n", (int)status, uid);
    fflush(stdout);
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 3 || !geteuid())
        return 2;
    gstd_json_initialize();
    gstd_bootstrap_http_action action;
    if (!strcmp(argv[1], "api"))
        action = GSTD_BOOT_API_READY;
    else if (!strcmp(argv[1], "git"))
        action = GSTD_BOOT_GIT_READY;
    else if (!strcmp(argv[1], "controller"))
        action = GSTD_BOOT_CONTROLLER_READY;
    else if (!strcmp(argv[1], "enroll"))
        action = GSTD_BOOT_CONTROLLER_ENROLL;
    else
        return 2;
    int descriptor = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 3)
        return 2;
    pid_t parent = getpid(), child = fork();
    if (child < 0)
        return 2;
    if (!child)
        _exit(run(parent, action, descriptor));
    int status = 0;
    int valid = waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
    close(descriptor);
    return valid ? 0 : 1;
}
