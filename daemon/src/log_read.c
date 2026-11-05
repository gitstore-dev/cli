#define _GNU_SOURCE 1
#include "log_read.h"
#include "gitstored/protocol.h"
#include "gitstored/scheduler.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
static int64_t milliseconds(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts) ? 0
                                               : (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#ifdef __linux__
static int send_all(int fd, const void *bytes, size_t length) {
    const unsigned char *p = bytes;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}
static void child(const gstd_log_read_input *input, int channel, pid_t parent) {
    int state = fcntl(input->state_fd, F_DUPFD_CLOEXEC, 5);
    int output = fcntl(channel, F_DUPFD_CLOEXEC, 5);
    if (state < 0 || output < 0 || dup2(state, 3) < 0 || dup2(output, 4) < 0)
        _exit(1);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || prctl(PR_SET_DUMPABLE, 0) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
        _exit(1);
    long maximum = sysconf(_SC_OPEN_MAX);
    if (syscall(SYS_close_range, 5u, ~0u, 0u)) {
        if (maximum < 0 || maximum > 1048576)
            _exit(1);
        for (int fd = 5; fd < maximum; fd++)
            close(fd);
    }
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
        _exit(1);
    for (int fd = 0; fd < 3; fd++)
        if (dup2(null, fd) < 0)
            _exit(1);
    close(null);
    struct rlimit zero = {0, 0}, descriptors = {32, 32};
    if (setrlimit(RLIMIT_CORE, &zero) || setrlimit(RLIMIT_FSIZE, &zero) ||
        setrlimit(RLIMIT_NOFILE, &descriptors))
        _exit(1);
#ifndef GITSTORED_INSTRUMENTED
    struct rlimit memory = {512u * 1024u * 1024u, 512u * 1024u * 1024u};
    if (setrlimit(RLIMIT_AS, &memory))
        _exit(1);
#endif
    gstd_logs *logs = NULL;
    json_t *records = NULL;
    gstd_log_status status = GSTD_LOG_IO;
    struct stat info;
    if (fstatat(3, "logs", &info, AT_SYMLINK_NOFOLLOW) && errno == ENOENT) {
        status = input->cursor ? GSTD_LOG_CURSOR_EXPIRED : GSTD_LOG_OK;
        if (status == GSTD_LOG_OK)
            records = json_array();
    } else {
        int directory = gstd_logs_directory(3, input->store_id, 0);
        close(3);
        if (directory >= 3) {
            status = gstd_logs_open_at(directory, NULL, 0, &logs);
            close(directory);
            if (status == GSTD_LOG_OK)
                status = gstd_logs_replay(logs, input->deployment_id, input->component, input->kind,
                                          input->cursor, input->limit, input->now, &records);
        }
    }
    gstd_logs_close(logs);
    size_t length = records ? json_dumpb(records, NULL, 0, JSON_COMPACT) : 0;
    char *encoded = length && length <= GSTD_STREAM_BYTES ? malloc(length) : NULL;
    if (length && encoded && json_dumpb(records, encoded, length, JSON_COMPACT) != length) {
        free(encoded);
        encoded = NULL;
    }
    json_decref(records);
    if ((status == GSTD_LOG_OK && !encoded) || length > GSTD_STREAM_BYTES) {
        status = GSTD_LOG_IO;
        length = 0;
    }
    unsigned char header[8] = {(unsigned char)status,
                               0,
                               0,
                               0,
                               (unsigned char)(length >> 24),
                               (unsigned char)(length >> 16),
                               (unsigned char)(length >> 8),
                               (unsigned char)length};
    int failed = send_all(4, header, sizeof(header)) || (length && send_all(4, encoded, length));
    free(encoded);
    close(4);
    _exit(failed ? 1 : 0);
}
#endif
int gstd_log_read_launch(sqlite3 *db, const gstd_log_read_input *input, gstd_log_read *read_job) {
    if (!db || !input || !read_job || read_job->worker || read_job->complete ||
        input->state_fd < 3 || !input->store_id || strnlen(input->store_id, 33) != 32 ||
        !input->subscription_id || !input->deployment_id ||
        !gstd_identifier(input->subscription_id, strnlen(input->subscription_id, 65)) ||
        !gstd_identifier(input->deployment_id, strnlen(input->deployment_id, 65)) ||
        (input->component && !gstd_identifier(input->component, strnlen(input->component, 65))) ||
        (input->cursor && strnlen(input->cursor, 65) > 64) ||
        (input->kind != GSTD_STREAM_LOGS && input->kind != GSTD_STREAM_EVENTS_KIND) ||
        !input->limit || input->limit > 256 || input->now < 0)
        return -1;
#ifndef __linux__
    return -1;
#else
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC))
        return -1;
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    int reserved = gstd_schedule_read_reserve(db, input->subscription_id, input->deployment_id);
    if (reserved != 1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return reserved;
    }
    pid_t parent = getpid(), pid = fork();
    if (!pid)
        child(input, pipefd[1], parent);
    close(pipefd[1]);
    snprintf(read_job->subscription, sizeof(read_job->subscription), "%s", input->subscription_id);
    read_job->status = GSTD_LOG_IO;
    if (pid < 0) {
        close(pipefd[0]);
        read_job->reaped = 1;
        /* Retain a failed-release object so the caller can retry its durable release. */
        if (gstd_schedule_read_finish(db, read_job->subscription))
            return 1;
        memset(read_job, 0, sizeof(*read_job));
        read_job->channel = -1;
        return -1;
    }
    read_job->worker = pid;
    read_job->channel = pipefd[0];
    read_job->deadline_ms = milliseconds() + 5000;
    return 1;
#endif
}
void gstd_log_read_abandon(gstd_log_read *job) {
    if (job)
        job->abandoned = 1;
}
int gstd_log_read_poll(sqlite3 *db, gstd_log_read *job) {
    if (!db || !job || (!job->worker && !job->reaped && !job->complete))
        return -1;
    if (job->complete)
        return 1;
    if (!job->reaped && (job->abandoned || milliseconds() >= job->deadline_ms)) {
        /* This unreaped direct child cannot have its PID reused. */
        if (kill(job->worker, SIGKILL) && errno != ESRCH)
            return 0;
        job->abandoned = 1;
    }
    while (job->channel >= 0 && !job->abandoned) {
        if (job->received >= sizeof(job->header) &&
            job->received == sizeof(job->header) + job->expected)
            break;
        size_t header_bytes = sizeof(job->header);
        unsigned char *target = job->received < header_bytes
                                    ? job->header + job->received
                                    : job->bytes + job->received - header_bytes;
        size_t remaining = job->received < header_bytes
                               ? header_bytes - job->received
                               : job->expected + header_bytes - job->received;
        if (!remaining)
            break;
        ssize_t n = read(job->channel, target, remaining);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n <= 0) {
            close(job->channel);
            job->channel = -1;
            break;
        }
        job->received += (size_t)n;
        if (job->received == header_bytes) {
            job->expected = ((size_t)job->header[4] << 24) | ((size_t)job->header[5] << 16) |
                            ((size_t)job->header[6] << 8) | job->header[7];
            if (job->header[0] > GSTD_LOG_LIMIT || job->header[1] || job->header[2] ||
                job->header[3] || job->expected > GSTD_STREAM_BYTES) {
                job->abandoned = 1;
                break;
            }
            if (job->expected && !(job->bytes = malloc(job->expected))) {
                job->abandoned = 1;
                break;
            }
        }
    }
    int outcome = 0;
    if (!job->reaped) {
        pid_t got = waitpid(job->worker, &outcome, WNOHANG);
        if (got == job->worker) {
            job->reaped = 1;
            job->exit_status = outcome;
            job->exit_ok = WIFEXITED(outcome) && !WEXITSTATUS(outcome);
            /* Drain any bytes written between the nonblocking read and waitpid. */
            return 0;
        } else if (got < 0 && errno != EINTR)
            return -1;
    }
    if (!job->reaped)
        return 0;
    if (!job->abandoned && job->exit_ok && !job->records &&
        job->received == sizeof(job->header) + job->expected) {
        job->status = (gstd_log_status)job->header[0];
        if (job->status == GSTD_LOG_OK) {
            json_error_t error;
            job->records = job->bytes ? json_loadb((const char *)job->bytes, job->expected,
                                                   JSON_REJECT_DUPLICATES, &error)
                                      : NULL;
            if (!json_is_array(job->records) || json_array_size(job->records) > 256) {
                json_decref(job->records);
                job->records = NULL;
                job->status = GSTD_LOG_IO;
            }
        }
    }
    if (job->channel >= 0) {
        close(job->channel);
        job->channel = -1;
    }
    if (gstd_schedule_read_finish(db, job->subscription))
        return -1;
    job->complete = 1;
    return 1;
}
void gstd_log_read_close(gstd_log_read *job) {
    if (!job || !job->complete)
        return;
    free(job->bytes);
    json_decref(job->records);
    memset(job, 0, sizeof(*job));
    job->channel = -1;
}
