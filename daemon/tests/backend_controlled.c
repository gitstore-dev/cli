#define _POSIX_C_SOURCE 200809L
#include "backend_controlled.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static gstd_test_workload *registered[8];
static unsigned serial;
static gstd_test_workload *find(const char *id) {
    if (id)
        for (unsigned i = 0; i < 8; i++)
            if (registered[i] && !strcmp(id, registered[i]->identity))
                return registered[i];
    return NULL;
}
static int observe(const char *id, int *terminated) {
    gstd_test_workload *w = find(id);
    if (!w || !terminated || w->behavior == GSTD_TEST_UNCERTAIN)
        return -1;
    if (!w->exited) {
        int status;
        pid_t result = waitpid(w->child, &status, WNOHANG);
        if (result < 0)
            return -1;
        if (result == w->child) {
            w->exited = 1;
            w->status = status;
        }
    }
    *terminated = w->exited;
    return 0;
}
static int send_control(gstd_test_workload *w) {
    if (!w || find(w->identity) != w || w->control_fd < 0)
        return -1;
    /* Closing this private pipe releases only this child and cannot SIGPIPE the
     * caller if a scripted failed child has already exited. */
    int fd = w->control_fd;
    w->control_fd = -1;
    return close(fd);
}
static int cancel(const char *id) {
    gstd_test_workload *w = find(id);
    if (!w || w->behavior == GSTD_TEST_UNSAFE_MIGRATION || w->behavior == GSTD_TEST_UNCERTAIN)
        return -1;
    return w->exited ? 0 : send_control(w);
}
const gstd_backend *gstd_test_controlled_backend(void) {
    static const gstd_backend backend = {observe, cancel};
    return &backend;
}
int gstd_test_workload_start(gstd_test_behavior behavior, gstd_test_workload *w) {
    if (!w || behavior < GSTD_TEST_SLOW || behavior > GSTD_TEST_UNSAFE_MIGRATION)
        return -1;
    for (unsigned i = 0; i < 8; i++)
        if (registered[i] == w)
            return -1;
    unsigned slot = 0;
    for (; slot < 8 && registered[slot]; slot++)
        ;
    if (slot == 8 || serial == ~0u)
        return -1;
    int pipefd[2];
    if (pipe(pipefd))
        return -1;
    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (!child) {
        close(pipefd[1]);
        /* Earlier fixtures' parent pipe ends must not keep siblings alive. */
        for (unsigned i = 0; i < 8; i++)
            if (registered[i] && registered[i]->control_fd >= 0)
                close(registered[i]->control_fd);
        if (behavior == GSTD_TEST_FAILED)
            _exit(17);
        alarm(20);
        char command;
        ssize_t n;
        do {
            n = read(pipefd[0], &command, 1);
        } while (n < 0 && errno == EINTR);
        _exit(n == 0 ? 0 : 99);
    }
    close(pipefd[0]);
    memset(w, 0, sizeof(*w));
    w->child = child;
    w->control_fd = pipefd[1];
    w->behavior = behavior;
    snprintf(w->identity, sizeof(w->identity), "controlled-%u-%ld", ++serial, (long)child);
    registered[slot] = w;
    return 0;
}
int gstd_test_workload_release(gstd_test_workload *w) { return send_control(w); }
int gstd_test_workload_reveal(gstd_test_workload *w) {
    if (!w || find(w->identity) != w || w->behavior != GSTD_TEST_UNCERTAIN)
        return -1;
    w->behavior = GSTD_TEST_SLOW;
    return 0;
}
int gstd_test_workload_close(gstd_test_workload *w) {
    if (!w || find(w->identity) != w)
        return -1;
    if (w->control_fd >= 0)
        send_control(w);
    int result = 0;
    if (!w->exited) {
        /* A registered unreaped direct child cannot have its PID reused. */
        if (kill(w->child, SIGKILL) && errno != ESRCH)
            result = -1;
        int status;
        pid_t child;
        do {
            child = waitpid(w->child, &status, 0);
        } while (child < 0 && errno == EINTR);
        if (child != w->child)
            result = -1;
        else {
            w->exited = 1;
            w->status = status;
        }
    }
    for (unsigned i = 0; i < 8; i++)
        if (registered[i] == w)
            registered[i] = NULL;
    memset(w, 0, sizeof(*w));
    w->control_fd = -1;
    return result;
}
