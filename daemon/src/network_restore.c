#define _POSIX_C_SOURCE 200809L
#include "network_restore.h"
#include "native_exit.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int gstd_network_restore_worker(int scope, const gstd_filesystem_input *input,
                                gstd_filesystem_reply *reply) {
    if (!input || !reply)
        return -1;
    gstd_network_worker owner = GSTD_NETWORK_WORKER_INITIAL;
    gstd_network_worker_observation actual = GSTD_NETWORK_OBSERVATION_INITIAL;
    gstd_network_worker_request request = {.manifest = input->manifest,
                                           .store_id = input->store_id,
                                           .resource_id = input->resource_id,
                                           .operation_id = input->operation_id,
                                           .scope_fd = scope};
    if (gstd_network_worker_reopen(&request, input->scope_expected, &owner, &actual))
        return -1;
    reply->descriptor_count = 4;
    reply->descriptors[0] = owner.scope_fd;
    reply->descriptors[1] = owner.pid_fd;
    reply->descriptors[2] = actual.network.user_fd;
    reply->descriptors[3] = actual.network.network_fd;
    owner.scope_fd = -1;
    owner.pid_fd = -1;
    actual.network.user_fd = -1;
    actual.network.network_fd = -1;
    reply->network_owner = owner;
    reply->network_observed = actual;
    return 0;
}
int gstd_network_restore_adopt(gstd_filesystem_reply *reply, const json_t *expected,
                               gstd_network_worker *owner,
                               gstd_network_worker_observation *actual) {
    if (!reply || !expected || !owner || !actual || reply->status || reply->descriptor_count != 4 ||
        owner->pid || owner->scope_fd != -1 || owner->pid_fd != -1 || owner->channel != -1 ||
        actual->network.user_fd != -1 || actual->network.network_fd != -1 ||
        reply->network_owner.scope_fd != -1 || reply->network_owner.pid_fd != -1 ||
        reply->network_owner.channel != -1 || reply->network_observed.network.user_fd != -1 ||
        reply->network_observed.network.network_fd != -1 ||
        reply->network_observed.network.tap_fd != -1 ||
        reply->network_observed.network.life_fd != -1 ||
        reply->network_observed.network.pid_fd != -1)
        return -1;
    gstd_network_worker candidate = reply->network_owner;
    gstd_network_worker_observation observed = reply->network_observed;
    candidate.scope_fd = reply->descriptors[0];
    candidate.pid_fd = reply->descriptors[1];
    observed.network.user_fd = reply->descriptors[2];
    observed.network.network_fd = reply->descriptors[3];
    json_t *identity = gstd_network_worker_identity(&candidate);
    int matches = identity && json_equal(identity, expected);
    json_decref(identity);
    if (!matches || strnlen(observed.network.boot_id, 37) != 36)
        return -1;
    struct stat info;
    uint64_t start;
    if (candidate.scope_fd < 3 || candidate.pid_fd < 3 || observed.network.user_fd < 3 ||
        observed.network.network_fd < 3 || !candidate.permitted || !candidate.received ||
        observed.pid != candidate.pid || observed.start != candidate.start ||
        fstat(candidate.scope_fd, &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700 || (uint64_t)info.st_dev != candidate.scope_device ||
        (uint64_t)info.st_ino != candidate.scope_inode ||
        gstd_native_watch_poll(candidate.pid_fd) != 0 ||
        gstd_native_process_start(candidate.pid, &start) || start != candidate.start ||
        strcmp(candidate.boot_id, observed.network.boot_id) ||
        gstd_network_namespace_check(&observed.network))
        return -1;
    *owner = candidate;
    *actual = observed;
    reply->descriptor_count = 0;
    return 0;
}
