#define _POSIX_C_SOURCE 200809L
#include "helper.h"
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
enum { CREATING = 1, AWAITING_COMMIT, STARTING, WAITING_EXIT, DELETING, FINISHED };
static int64_t now_ms(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now)
               ? -1
               : (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int finish(gstd_helper *a, int outcome) {
    a->outcome = outcome;
    a->stage = FINISHED;
    return 2;
}
static int cancelled(int descriptor) {
    if (descriptor == -1)
        return 0;
    if (descriptor < 3)
        return -1;
    struct pollfd notification = {.fd = descriptor, .events = POLLIN};
    int ready = poll(&notification, 1, 0);
    return ready < 0 ? -1 : ready && notification.revents;
}
int gstd_helper_validate(const gstd_native_context *context) {
    const gstd_execution_inputs *input = context ? context->registered_inputs : NULL;
    int oidc = input && input->purpose == GSTD_EXEC_OIDC_MIGRATE;
    if (!context || !input || input->purpose == GSTD_EXEC_SERVICE ||
        input->purpose < GSTD_EXEC_SCYLLA_READY ||
        (input->purpose > GSTD_EXEC_SCHEMA_HISTORY && !oidc) ||
        context->control_owner != getpid() || !context->component || !input->component ||
        strcmp(context->component, input->component) || context->rootfs_fd != input->rootfs_fd ||
        context->expected_pid || context->expected_start_time || context->test_fixture)
        return -1;
    if (oidc) {
        struct stat lease;
        int flags = fcntl(context->writer_lease_fd, F_GETFL);
        if ((strcmp(context->component, "oidc-hydra") &&
             strcmp(context->component, "oidc-kratos")) ||
            context->writer_lease_fd < 3 || flags < 0 || (flags & O_ACCMODE) != O_RDWR ||
            fstat(context->writer_lease_fd, &lease) || !S_ISREG(lease.st_mode) ||
            lease.st_uid != geteuid() || lease.st_nlink != 1 || lease.st_size ||
            (lease.st_mode & 07777) != 0600)
            return -1;
    } else if (context->writer_lease_fd != -1)
        return -1;
    json_t *registered = gstd_execution_build(input);
    int valid =
        registered && context->configuration && json_equal(registered, context->configuration);
    json_decref(registered);
    return valid ? 0 : -1;
}
int gstd_helper_begin(const gstd_native_context *context, gstd_helper *a) {
    if (!a || a->stage || gstd_helper_validate(context))
        return -1;
    *a = (gstd_helper){.context = *context,
                       .population = GSTD_NATIVE_POPULATION_INITIAL,
                       .death_fd = -1,
                       .stage = CREATING,
                       .may_exist = 1};
    if (gstd_native_launch(&a->context, GSTD_NATIVE_CREATE, 10000, &a->worker)) {
        a->may_exist = 0;
        finish(a, -1);
    }
    return 0;
}
int gstd_helper_withhold_start(gstd_helper *a) {
    if (!a || a->stage != AWAITING_COMMIT || a->worker.worker)
        return -1;
    finish(a, -2);
    return 0;
}
int gstd_helper_identity_ready(const gstd_helper *a) {
    return a && a->stage == AWAITING_COMMIT && !a->worker.worker && a->context.expected_pid > 0 &&
           a->context.expected_start_time && gstd_native_watch_poll(a->death_fd) == 0 &&
           !gstd_native_population_matches(&a->population, a->context.backend_id,
                                           a->context.expected_pid, a->context.expected_start_time,
                                           a->context.boot_id);
}
int gstd_helper_permit_start(gstd_helper *a) {
    if (!gstd_helper_identity_ready(a))
        return -1;
    int64_t now = now_ms();
    if (now < 0)
        return -1;
    a->deadline_ms = now + ((a->context.registered_inputs->purpose == GSTD_EXEC_SCHEMA_MIGRATE ||
                             a->context.registered_inputs->purpose == GSTD_EXEC_OIDC_MIGRATE)
                                ? 70000
                                : 20000);
    a->stage = STARTING;
    if (gstd_native_launch(&a->context, GSTD_NATIVE_START, 10000, &a->worker))
        finish(a, -2);
    return 0;
}
int gstd_helper_poll(gstd_helper *a, int cancellation) {
    if (!a || !a->stage || (cancellation != -1 && cancellation < 3))
        return -1;
    if (a->stage == FINISHED)
        return 2;
    if (a->stage == AWAITING_COMMIT)
        return cancelled(cancellation) ? finish(a, -2) : 1;
    if (a->stage == WAITING_EXIT) {
        int64_t now = now_ms();
        if (now < 0 || cancelled(cancellation) || now >= a->deadline_ms)
            return finish(a, -2);
        int collected = gstd_native_exit_poll(a->context.expected_pid,
                                              a->context.expected_start_time, &a->exit);
        if (!collected)
            return 0;
        if (collected != 1 || gstd_native_watch_poll(a->death_fd) != 1 ||
            gstd_native_population_empty(&a->population) != 1)
            return finish(a, -2);
        a->stage = DELETING;
        if (gstd_native_launch(&a->context, GSTD_NATIVE_DELETE, 10000, &a->worker))
            return finish(a, -2);
        return 0;
    }
    gstd_native_status status;
    int ready = gstd_native_poll(&a->worker, cancellation, &status, &a->observed);
    if (ready != 1)
        return ready;
    if (status != GSTD_NATIVE_OK) {
        if (a->stage == CREATING && status != GSTD_NATIVE_UNCERTAIN &&
            !a->observed.effect_possible) {
            a->may_exist = 0;
            return finish(a, -1);
        }
        return finish(a, -2);
    }
    if (a->stage == CREATING) {
        a->context.expected_pid = a->observed.pid;
        a->context.expected_start_time = a->observed.start_time;
        if (gstd_native_watch_open(a->context.expected_pid, a->context.expected_start_time,
                                   a->context.boot_id, &a->death_fd) ||
            gstd_native_population_open(a->context.backend_id, a->context.expected_pid,
                                        a->context.expected_start_time, a->context.boot_id,
                                        &a->population) ||
            gstd_native_population_empty(&a->population) != 0)
            return finish(a, -2);
        a->stage = AWAITING_COMMIT;
        return 1;
    }
    if (a->stage == STARTING) {
        a->stage = WAITING_EXIT;
        return 0;
    }
    if (a->stage != DELETING || gstd_native_watch_poll(a->death_fd) != 1 ||
        gstd_native_population_empty(&a->population) != 1)
        return finish(a, -2);
    a->deleted = 1;
    a->may_exist = 0;
    return finish(a, a->exit.exited && !a->exit.code ? 0 : -1);
}
int gstd_helper_close(gstd_helper *a) {
    if (!a || a->stage != FINISHED || a->worker.worker)
        return -1;
    return gstd_helper_observers_close(a);
}
int gstd_helper_observers_close(gstd_helper *a) {
    if (!a || a->worker.worker)
        return -1;
    if (!a->stage)
        return 0;
    if (a->death_fd >= 3)
        close(a->death_fd);
    gstd_native_population_close(&a->population);
    memset(a, 0, sizeof(*a));
    return 0;
}
