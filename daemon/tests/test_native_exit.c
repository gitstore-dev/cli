#define _GNU_SOURCE 1
#include "native_exit.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#ifdef __linux__
static int nonchild_watch(void) {
    int input[2], ready[2];
    CHECK(!pipe(input) && !pipe(ready));
    pid_t parent = fork();
    CHECK(parent >= 0);
    if (!parent) {
        close(input[1]);
        close(ready[0]);
        pid_t workload = fork();
        if (workload < 0)
            _exit(90);
        if (!workload) {
            close(ready[1]);
            alarm(10);
            char byte;
            _exit(read(input[0], &byte, 1) == 0 ? 23 : 91);
        }
        close(input[0]);
        if (write(ready[1], &workload, sizeof(workload)) != sizeof(workload))
            _exit(92);
        close(ready[1]);
        int status;
        _exit(waitpid(workload, &status, 0) == workload && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 23
                  ? 0
                  : 93);
    }
    close(input[0]);
    close(ready[1]);
    pid_t workload;
    CHECK(read(ready[0], &workload, sizeof(workload)) == sizeof(workload));
    close(ready[0]);
    uint64_t start;
    char boot[37];
    int watch = -1;
    CHECK(!gstd_native_boot_id(boot) && !gstd_native_process_start(workload, &start));
    CHECK(gstd_native_watch_open(workload, start + 1, boot, &watch) == -1 && watch == -1);
    char wrong[37];
    memcpy(wrong, boot, sizeof(wrong));
    wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
    CHECK(gstd_native_watch_open(workload, start, wrong, &watch) == -1 && watch == -1);
    CHECK(!gstd_native_watch_open(workload, start, boot, &watch));
    CHECK(gstd_native_watch_poll(watch) == 0 && gstd_native_watch_poll(input[1]) == -1);
    gstd_native_exit exit;
    CHECK(gstd_native_exit_poll(workload, start, &exit) == -2); /* Not our child. */
    close(input[1]);
    int status;
    CHECK(waitpid(parent, &status, 0) == parent && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(gstd_native_watch_poll(watch) == 1); /* Still exact after its parent reaped it. */
    CHECK(!close(watch));
    CHECK(gstd_native_watch_poll(watch) == -1);
    return 0;
}
#endif
int main(void) {
    gstd_native_exit result;
    CHECK(gstd_native_exit_poll(0, 1, &result) == -1);
#ifdef __linux__
    char boot[37], again[37];
    CHECK(!gstd_native_boot_id(boot) && !gstd_native_boot_id(again));
    CHECK(strlen(boot) == 36 && !memcmp(boot, again, 37));
    CHECK(gstd_native_boot_id(NULL) == -1);
    alarm(10);
    CHECK(!nonchild_watch());
    int descriptors[2];
    CHECK(!pipe(descriptors));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(descriptors[1]);
        char marker;
        _exit(read(descriptors[0], &marker, 1) == 1 ? 23 : 99);
    }
    close(descriptors[0]);
    uint64_t start;
    CHECK(!gstd_native_process_start(child, &start) && start);
    CHECK(!gstd_native_exit_poll(child, start, &result));
    CHECK(write(descriptors[1], "x", 1) == 1);
    close(descriptors[1]);
    siginfo_t info = {0};
    CHECK(!waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT));
    CHECK(gstd_native_exit_poll(child, start + 1, &result) == -2);
    CHECK(!result.exited && !result.signalled);
    CHECK(gstd_native_exit_poll(child, start, &result) == 1);
    CHECK(result.exited && !result.signalled && result.code == 23);
    CHECK(gstd_native_exit_poll(child, start, &result) == -2);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        for (;;)
            pause();
    }
    CHECK(!gstd_native_process_start(child, &start));
    CHECK(!kill(child, SIGTERM));
    CHECK(!waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT));
    CHECK(gstd_native_exit_poll(child, start, &result) == 1);
    CHECK(!result.exited && result.signalled && result.code == SIGTERM);
#else
    CHECK(gstd_native_exit_poll(getpid(), 1, &result) == -2);
#endif
    return 0;
}
