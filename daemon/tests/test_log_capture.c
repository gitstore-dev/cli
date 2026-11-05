#define _GNU_SOURCE 1
#include "gitstored/protocol.h"
#include "log_capture.h"
#include "logs.h"
#include "native_exit.h"
#include "support.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "capture line %d: %s\n", __LINE__, #x);                                \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static void delay(void) {
    struct timespec value = {0, 1000000};
    nanosleep(&value, NULL);
}
static int await_ready(gstd_log_capture *capture) {
    for (unsigned i = 0; i < 12000; i++) {
        int ready = gstd_log_capture_poll(capture, -1);
        if (ready) {
            if (ready != 1 || capture->status)
                fprintf(stderr, "collector readiness: stage=%d status=%d interrupted=%d\n",
                        capture->stage, capture->status, capture->interrupted);
            return ready == 1 && !capture->status ? 0 : -1;
        }
        delay();
    }
    return -1;
}
static int await_child(pid_t child) {
    for (unsigned i = 0; i < 5000; i++) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        if (result < 0 && errno != EINTR)
            return -1;
        delay();
    }
    return -1;
}
typedef struct {
    pid_t pid;
    uint64_t start;
} child_identity;
static int transfer(int socket, int descriptor, child_identity *identity, int sending) {
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct iovec vector = {identity, sizeof(*identity)};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
    if (sending) {
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
        return sendmsg(socket, &message, MSG_NOSIGNAL) == sizeof(*identity) ? 0 : -1;
    }
    if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != sizeof(*identity) ||
        message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
        return -1;
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int)))
        return -1;
    memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
    return descriptor;
}
int main(void) {
    gstd_json_initialize();
    signal(SIGPIPE, SIG_IGN);
    int result = 1, root_fd = -1, configuration = -1, orphan_configuration = -1, writer = -1,
        channel[2] = {-1, -1};
    pid_t owner = -1, orphan = -1;
    char *root = gstd_test_dir(), path[512], config_path[512], orphan_path[512];
    gstd_log_capture capture = GSTD_LOG_CAPTURE_INITIAL;
    gstd_logs *logs = NULL;
    json_t *page = NULL, *identity = NULL;
    int datastore_configuration = -1;
    CHECK(root && geteuid() && !prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    snprintf(config_path, sizeof(config_path), "%s/configuration", root);
    snprintf(orphan_path, sizeof(orphan_path), "%s/orphan-configuration", root);
    CHECK(!mkdir(config_path, 0700) && !mkdir(orphan_path, 0700));
    root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    configuration = open(config_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    orphan_configuration = open(orphan_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(root_fd >= 3 && configuration >= 3 && orphan_configuration >= 3);
    const char *secrets[] = {"fixture-key-never-persisted-123456789"};
    gstd_log_capture_input input = {.state_fd = root_fd,
                                    .configuration_fd = configuration,
                                    .broker_fd = -1,
                                    .store_id = "0123456789abcdef0123456789abcdef",
                                    .deployment_id = "captured",
                                    .operation_id = "capture-one",
                                    .generation = 1,
                                    .components = 1,
                                    .test_fixture = 1,
                                    .test_secrets = secrets,
                                    .test_secret_count = 1};
    CHECK(!gstd_log_capture_launch(&input, &capture));
    CHECK(!await_ready(&capture));
    /* File quota signals must not kill the pipe drainer and propagate EPIPE
     * into workloads. SQLite reports the failed write to the redacted sink. */
    CHECK(!syscall(SYS_pidfd_send_signal, capture.death_fd, SIGXFSZ, NULL, 0));
    identity = gstd_log_capture_identity(&capture);
    CHECK(identity);
    json_decref(identity);
    identity = NULL;
    const char *prefix = "before fixture-key-";
    CHECK(write(capture.output[0][0], prefix, strlen(prefix)) == (ssize_t)strlen(prefix));
    const char *rest = "never-persisted-123456789 after\n";
    CHECK(write(capture.output[0][0], rest, strlen(rest)) == (ssize_t)strlen(rest));
    const char *sensitive = "Authorization: Bearer opaque-value\n";
    CHECK(write(capture.output[0][1], sensitive, strlen(sensitive)) == (ssize_t)strlen(sensitive));
    gstd_log_capture_outputs_close(&capture);
    CHECK(!await_child(capture.worker));
    CHECK(!gstd_log_capture_close(&capture));
    snprintf(path, sizeof(path), "%s/logs/segments.db", root);
    CHECK(gstd_logs_open(path, NULL, &logs) == GSTD_LOG_OK);
    CHECK(gstd_logs_replay(logs, "captured", NULL, GSTD_STREAM_LOGS, NULL, 10, (int64_t)time(NULL),
                           &page) == GSTD_LOG_OK &&
          json_array_size(page) == 2);
    unsigned known = 0, field = 0;
    for (size_t i = 0; i < json_array_size(page); i++) {
        const char *text = json_string_value(json_object_get(json_array_get(page, i), "text"));
        CHECK(text && !strstr(text, "fixture-key") && !strstr(text, "opaque-value"));
        known += !strcmp(text, "before [REDACTED] after");
        field += strstr(text, "sensitive log line") != NULL;
    }
    CHECK(known == 1 && field == 1);
    json_decref(page);
    page = NULL;
    for (unsigned count = 3; count <= 4; count++) {
        char name[32];
        snprintf(name, sizeof(name), "scylla-capture-%u", count);
        CHECK(!mkdirat(root_fd, name, 0700));
        datastore_configuration =
            openat(root_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        CHECK(datastore_configuration >= 3);
        gstd_log_capture_input selected = input;
        selected.configuration_fd = datastore_configuration;
        selected.deployment_id = count == 3 ? "scylla-three" : "scylla-four";
        selected.operation_id = selected.deployment_id;
        selected.components = count;
        selected.scylladb = 2;
        CHECK(gstd_log_capture_launch(&selected, &capture) == -1);
        selected.scylladb = 1;
        CHECK(!gstd_log_capture_launch(&selected, &capture));
        CHECK(!await_ready(&capture));
        const char line[] = "datastore fixture-key-never-persisted-123456789 ready\n";
        CHECK(write(capture.output[count - 1][0], line, sizeof(line) - 1) == sizeof(line) - 1);
        gstd_log_capture_outputs_close(&capture);
        CHECK(!await_child(capture.worker));
        CHECK(!gstd_log_capture_close(&capture));
        CHECK(gstd_logs_replay(logs, selected.deployment_id, "scylladb", GSTD_STREAM_LOGS, NULL, 10,
                               (int64_t)time(NULL), &page) == GSTD_LOG_OK &&
              json_array_size(page) == 1);
        CHECK(!strcmp(json_string_value(json_object_get(json_array_get(page, 0), "text")),
                      "datastore [REDACTED] ready"));
        json_decref(page);
        page = NULL;
        close(datastore_configuration);
        datastore_configuration = -1;
    }
    /* The workload writer is a transferred synthetic pipe, never a host command.
     * Kill its owner process after ready; capture must continue independently. */
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel));
    struct timeval timeout = {15, 0};
    CHECK(!setsockopt(channel[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    owner = fork();
    CHECK(owner >= 0);
    if (!owner) {
        close(channel[0]);
        input.configuration_fd = orphan_configuration;
        input.deployment_id = "orphan";
        input.operation_id = "capture-orphan";
        gstd_log_capture child = GSTD_LOG_CAPTURE_INITIAL;
        if (gstd_log_capture_launch(&input, &child) || await_ready(&child))
            _exit(91);
        child_identity observed = {child.worker, child.start};
        if (transfer(channel[1], child.output[0][0], &observed, 1))
            _exit(92);
        gstd_log_capture_outputs_close(&child);
        for (;;)
            pause();
    }
    close(channel[1]);
    channel[1] = -1;
    child_identity observed = {0};
    writer = transfer(channel[0], -1, &observed, 0);
    CHECK(writer >= 3 && observed.pid > 0);
    orphan = observed.pid;
    CHECK(!kill(owner, SIGKILL));
    int status;
    CHECK(waitpid(owner, &status, 0) == owner && WIFSIGNALED(status));
    owner = -1;
    uint64_t birth;
    CHECK(!gstd_native_process_start(orphan, &birth) && birth == observed.start);
    const char *survived = "after manager loss\n";
    CHECK(write(writer, survived, strlen(survived)) == (ssize_t)strlen(survived));
    close(writer);
    writer = -1;
    CHECK(!await_child(orphan));
    orphan = -1;
    CHECK(gstd_logs_replay(logs, "orphan", NULL, GSTD_STREAM_LOGS, NULL, 10, (int64_t)time(NULL),
                           &page) == GSTD_LOG_OK &&
          json_array_size(page) == 1);
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(page, 0), "text")),
                  "after manager loss"));
    result = 0;
done:
    if (datastore_configuration >= 3)
        close(datastore_configuration);
    if (writer >= 3)
        close(writer);
    if (owner > 0) {
        kill(owner, SIGKILL);
        waitpid(owner, NULL, 0);
    }
    if (orphan > 0) {
        kill(orphan, SIGKILL);
        waitpid(orphan, NULL, 0);
    }
    if (capture.worker && capture.stage == 1) {
        kill(capture.worker, SIGKILL);
        waitpid(capture.worker, NULL, 0);
        capture.worker = 0;
        capture.stage = 3;
    }
    (void)gstd_log_capture_close(&capture);
    gstd_log_capture_reap();
    for (unsigned i = 0; i < 2; i++)
        if (channel[i] >= 3)
            close(channel[i]);
    if (root_fd >= 3)
        close(root_fd);
    if (configuration >= 3)
        close(configuration);
    if (orphan_configuration >= 3)
        close(orphan_configuration);
    gstd_logs_close(logs);
    json_decref(page);
    json_decref(identity);
    if (root)
        gstd_test_remove(root);
    return result;
}
