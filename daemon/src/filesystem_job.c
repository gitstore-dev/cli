#define _GNU_SOURCE 1
#include "filesystem_job.h"
#include "gitstored/asset_tree.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "host_probe.h"
#include "image_backend.h"
#include "network_restore.h"
#include "oidc_preparation.h"
#include "oidc_profile.h"
#include "scope_recovery.h"
#include <string.h>
#include <unistd.h>
void gstd_filesystem_reply_close(gstd_filesystem_reply *reply) {
    if (!reply)
        return;
    for (unsigned i = 0; i < reply->descriptor_count && i < 8; i++)
        if (reply->descriptors[i] >= 3)
            close(reply->descriptors[i]);
    memset(reply, 0, sizeof(*reply));
}
#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int private_directory(int fd) {
    struct stat info;
    return fd >= 3 && !fstat(fd, &info) && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
           (info.st_mode & 07777) == 0700;
}
static int identifier(const char *text) {
    return text && strcmp(text, ".") && strcmp(text, "..") &&
           gstd_identifier(text, strnlen(text, 65));
}
static int store_id(const char *text) {
    if (!text || strnlen(text, 33) != 32)
        return 0;
    for (unsigned i = 0; i < 32; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return 1;
}
static int directory(int root, const char *name, int create) {
    if (create && mkdirat(root, name, 0700) && errno != EEXIST)
        return -1;
    int fd = openat(root, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0 && !private_directory(fd)) {
        close(fd);
        fd = -1;
    }
    return fd;
}
static int initialize(gstd_filesystem_input *input, gstd_filesystem_reply *reply) {
    int existing = input->action == GSTD_FS_REOPEN_ROOTS;
    if (!private_directory(3) || !store_id(input->store_id) ||
        (existing && !input->expected_cache.inode))
        return -1;
    char resources[80], native[80];
    snprintf(resources, sizeof(resources), "resources-%s", input->store_id);
    snprintf(native, sizeof(native), "native-%s", input->store_id);
    int cache = directory(3, "assets", input->expected_cache.inode == 0),
        volumes = directory(3, "volumes", !existing);
    gstd_cache_owner cache_owner = {.lock_fd = -1};
    gstd_volume_root volume_owner = {.lock_fd = -1};
    int resource_fd = -1, native_fd = -1;
    int valid = cache >= 3 && volumes >= 3 &&
                gstd_asset_cache_lock(cache, input->store_id, input->expected_cache.device,
                                      input->expected_cache.inode, input->expected_cache.inode == 0,
                                      &cache_owner) == GSTD_CACHE_OK &&
                (existing ? gstd_volume_root_reopen(volumes, input->store_id, &volume_owner)
                          : gstd_volume_root_lock(volumes, input->store_id, &volume_owner)) ==
                    GSTD_VOLUME_OK;
    if (valid) {
        resource_fd = directory(3, resources, !existing);
        native_fd = directory(3, native, !existing);
        valid = resource_fd >= 3 && native_fd >= 3;
    }
    if (!valid) {
        if (resource_fd >= 0)
            close(resource_fd);
        if (native_fd >= 0)
            close(native_fd);
        gstd_asset_cache_unlock(&cache_owner);
        gstd_volume_root_close(&volume_owner);
        if (cache >= 0)
            close(cache);
        if (volumes >= 0)
            close(volumes);
        return -1;
    }
    reply->cache_owner = cache_owner;
    reply->volume_root = volume_owner;
    int descriptors[] = {cache,    cache_owner.lock_fd, volumes, volume_owner.lock_fd, resource_fd,
                         native_fd};
    memcpy(reply->descriptors, descriptors, sizeof(descriptors));
    reply->descriptor_count = 6;
    return 0;
}
static int owner_file(int root, const json_t *value) {
    char *bytes = gstd_json_dump(value);
    if (!bytes)
        return -1;
    int fd = openat(root, "owner.json", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    size_t length = strlen(bytes), sent = 0;
    int valid = fd >= 3;
    while (valid && sent < length) {
        ssize_t n = write(fd, bytes + sent, length - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            valid = 0;
        else
            sent += (size_t)n;
    }
    free(bytes);
    valid = valid && !fsync(fd);
    if (fd >= 0)
        close(fd);
    return valid && !fsync(root) ? 0 : -1;
}
static int scope(gstd_filesystem_input *input, gstd_filesystem_reply *reply) {
    if (!private_directory(3) || !store_id(input->store_id) || !identifier(input->resource_id) ||
        !identifier(input->operation_id) || !identifier(input->deployment_id) ||
        !input->generation || input->generation > GSTD_SAFE_INTEGER || !input->resource_kind ||
        (strcmp(input->resource_kind, "configuration") &&
         strcmp(input->resource_kind, "workload") && strcmp(input->resource_kind, "network")))
        return -1;
    /* New reserved resource only; no adoption/recreation of a prior allocation.
     * Recovery of a partial/missing ACK is a separately verified owner action. */
    if (mkdirat(3, input->resource_id, 0700))
        return -1;
    int root = directory(3, input->resource_id, 0), data = -1, bundle = -1;
    if (root >= 3)
        data = directory(root, !strcmp(input->resource_kind, "workload") ? "execution" : "data", 1);
    if (root >= 3 && !strcmp(input->resource_kind, "workload"))
        bundle = directory(root, "bundle", 1);
    struct stat info, child;
    int valid = root >= 3 && data >= 3 &&
                (!strcmp(input->resource_kind, "workload") ? bundle >= 3 : 1) &&
                !fstat(root, &info) && !fstat(data, &child);
    char device[32], inode[32], data_device[32], data_inode[32];
    if (valid) {
        snprintf(device, sizeof(device), "%llu", (unsigned long long)info.st_dev);
        snprintf(inode, sizeof(inode), "%llu", (unsigned long long)info.st_ino);
        snprintf(data_device, sizeof(data_device), "%llu", (unsigned long long)child.st_dev);
        snprintf(data_inode, sizeof(data_inode), "%llu", (unsigned long long)child.st_ino);
    }
    json_t *owner =
        valid
            ? json_pack("{s:i,s:s,s:s,s:s,s:s,s:s,s:I,s:s,s:s,s:s,s:s}", "schemaVersion", 1,
                        "storeId", input->store_id, "resourceId", input->resource_id, "operationId",
                        input->operation_id, "deploymentId", input->deployment_id, "kind",
                        input->resource_kind, "generation", (json_int_t)input->generation, "device",
                        device, "inode", inode, "dataDevice", data_device, "dataInode", data_inode)
            : NULL;
    valid = valid && owner && !owner_file(root, owner) && !fsync(data) &&
            (bundle < 0 || !fsync(bundle)) && !fsync(3);
    json_decref(owner);
    if (!valid) {
        if (root >= 0)
            close(root);
        if (data >= 0)
            close(data);
        if (bundle >= 0)
            close(bundle);
        return -1;
    }
    reply->scope_device = (uint64_t)info.st_dev;
    reply->scope_inode = (uint64_t)info.st_ino;
    reply->descriptors[0] = root;
    reply->descriptors[1] = data;
    reply->descriptor_count = 2;
    if (bundle >= 3)
        reply->descriptors[reply->descriptor_count++] = bundle;
    return 0;
}
static int limit(int kind, rlim_t maximum) {
    struct rlimit current;
    if (getrlimit(kind, &current))
        return -1;
    if (current.rlim_cur > maximum)
        current.rlim_cur = maximum;
    if (current.rlim_max > maximum)
        current.rlim_max = maximum;
    return setrlimit(kind, &current);
}
static int verify(int fd, const gstd_provision_result *proof) {
    struct stat info;
    char digest[65];
    uint64_t logical = 0;
    return proof->staging_inode && proof->logical_bytes &&
                   proof->logical_bytes <= UINT64_C(8) * 1024 * 1024 * 1024 &&
                   proof->tree_digest[64] == 0 && strlen(proof->tree_digest) == 64 &&
                   !fstat(fd, &info) && (uint64_t)info.st_dev == proof->staging_device &&
                   (uint64_t)info.st_ino == proof->staging_inode &&
                   !gstd_asset_tree_digest(fd, proof->logical_bytes, 0, digest, &logical) &&
                   logical == proof->logical_bytes && !strcmp(digest, proof->tree_digest)
               ? 0
               : -1;
}
static int send_reply(int channel, gstd_filesystem_reply *reply) {
    if (reply->descriptor_count > 8)
        return -1;
    unsigned char control[CMSG_SPACE(sizeof(int) * 8)] = {0};
    struct iovec vector = {reply, sizeof(*reply)};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    if (reply->descriptor_count) {
        message.msg_control = control;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * reply->descriptor_count);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * reply->descriptor_count);
        memcpy(CMSG_DATA(header), reply->descriptors, sizeof(int) * reply->descriptor_count);
    }
    return sendmsg(channel, &message, MSG_NOSIGNAL) == sizeof(*reply) ? 0 : -1;
}
static void worker(gstd_filesystem_input input, int channel, pid_t parent) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || prctl(PR_SET_DUMPABLE, 0))
        _exit(126);
    int original[] = {input.descriptors[0], input.descriptors[1], input.descriptors[2], channel},
        copied[4];
    for (unsigned i = 0; i < 4; i++) {
        copied[i] = original[i] < 0 ? -1 : fcntl(original[i], F_DUPFD_CLOEXEC, 32);
        if (original[i] >= 0 && copied[i] < 32)
            _exit(126);
    }
    for (unsigned i = 0; i < 4; i++) {
        if (copied[i] < 0)
            close((int)i + 3);
        else if (dup2(copied[i], (int)i + 3) != (int)i + 3 ||
                 fcntl((int)i + 3, F_SETFD, FD_CLOEXEC))
            _exit(126);
    }
    if (syscall(SYS_close_range, 7u, ~0u, 0u))
        _exit(126);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
        _exit(126);
    for (int i = 0; i < 3; i++)
        if (dup2(null, i) != i)
            _exit(126);
    if (null >= 3)
        close(null);
    if (limit(RLIMIT_CORE, 0) || limit(RLIMIT_FSIZE, UINT64_C(8) * 1024 * 1024 * 1024) ||
        limit(RLIMIT_NOFILE, 128))
        _exit(126);
#ifndef GITSTORED_INSTRUMENTED
    if (limit(RLIMIT_AS, UINT64_C(1024) * 1024 * 1024))
        _exit(126);
#endif
    if (clearenv() || setenv("PATH", "/nonexistent", 1) || setenv("LANG", "C", 1))
        _exit(126);
    umask(077);
#ifdef GITSTORED_TESTING
    if (input.test_pause == 1)
        raise(SIGSTOP);
#endif
    gstd_filesystem_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.status = -1;
    input.descriptors[0] = 3;
    input.descriptors[1] = original[1] < 0 ? -1 : 4;
    input.descriptors[2] = original[2] < 0 ? -1 : 5;
    if (input.action == GSTD_FS_INITIALIZE || input.action == GSTD_FS_REOPEN_ROOTS)
        reply.status = initialize(&input, &reply);
    else if (input.action == GSTD_FS_HOST_PROBE) {
        reply.detail = (int)gstd_host_probe(3);
        reply.status = reply.detail == GSTD_HOST_OK ? 0 : -1;
    } else if (input.action == GSTD_FS_SCOPE)
        reply.status = scope(&input, &reply);
    else if (input.action == GSTD_FS_RESOLVER) {
        static const char text[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
        int fd =
            openat(3, "resolver.conf", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 3) {
            int valid =
                write(fd, text, sizeof(text) - 1) == sizeof(text) - 1 && !fsync(fd) && !fsync(3);
            close(fd);
            if (valid) {
                fd = openat(3, "resolver.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (fd >= 3) {
                    reply.status = 0;
                    reply.descriptor_count = 1;
                    reply.descriptors[0] = fd;
                }
            }
        }
    } else if (input.action == GSTD_FS_RECOVER_NETWORK) {
        reply.status = gstd_network_restore_worker(3, &input, &reply);
    } else if (input.action == GSTD_FS_REOPEN_SCOPE) {
        reply.status = gstd_scope_reopen(3, &input, &reply);
    } else if (input.action == GSTD_FS_STAGE_DIRECTORY) {
        char name[138];
        if (input.asset_key && input.operation_id &&
            !gstd_asset_stage_name(input.asset_key, input.operation_id, name) &&
            !mkdirat(3, name, 0700)) {
            int fd = directory(3, name, 0);
            struct stat info;
            if (fd >= 3 && !fstat(fd, &info) && !fsync(fd) && !fsync(3)) {
                reply.status = 0;
                reply.descriptor_count = 1;
                reply.descriptors[0] = fd;
                reply.scope_device = (uint64_t)info.st_dev;
                reply.scope_inode = (uint64_t)info.st_ino;
            } else if (fd >= 0)
                close(fd);
        }
    } else if (input.action == GSTD_FS_VOLUME || input.action == GSTD_FS_OBSERVE_VOLUME ||
               input.action == GSTD_FS_DATASTORE_VOLUME ||
               input.action == GSTD_FS_OBSERVE_DATASTORE) {
        gstd_volume_root owner = input.volume_root;
        owner.root_fd = 3;
        owner.lock_fd = 4;
        gstd_volume_handle handle = {.data_fd = -1, .writer_fd = -1};
        int observed =
            input.action == GSTD_FS_OBSERVE_VOLUME || input.action == GSTD_FS_OBSERVE_DATASTORE;
        int scylla =
            input.action == GSTD_FS_DATASTORE_VOLUME || input.action == GSTD_FS_OBSERVE_DATASTORE;
        reply.detail =
            observed ? (scylla ? gstd_scylla_volume_observe_open : gstd_volume_observe_open)(
                           3, &owner, input.volume_id, input.producer_id, &input.volume_expected,
                           &handle)
                     : (scylla ? gstd_scylla_volume_acquire : gstd_volume_acquire)(
                           3, &owner, input.volume_id, input.producer_id, !input.volume_existing,
                           input.volume_existing ? &input.volume_expected : NULL, &handle);
        if (reply.detail == GSTD_VOLUME_OK) {
            reply.status = 0;
            reply.volume = handle.identity;
            reply.descriptor_count = observed ? 1 : 2;
            reply.descriptors[0] = handle.data_fd;
            if (reply.descriptor_count == 2)
                reply.descriptors[1] = handle.writer_fd;
        }
    } else if (input.action == GSTD_FS_OIDC_VOLUME || input.action == GSTD_FS_OBSERVE_OIDC_VOLUME) {
        gstd_volume_root owner = input.volume_root;
        owner.root_fd = 3;
        owner.lock_fd = 4;
        gstd_volume_handle handle = {.data_fd = -1, .writer_fd = -1};
        int observed = input.action == GSTD_FS_OBSERVE_OIDC_VOLUME;
        reply.detail =
            observed
                ? gstd_oidc_volume_observe_open(3, &owner, input.oidc_volume_role, input.volume_id,
                                                input.producer_id, &input.volume_expected, &handle)
                : gstd_oidc_volume_acquire(3, &owner, input.oidc_volume_role, input.volume_id,
                                           input.producer_id, !input.volume_existing,
                                           input.volume_existing ? &input.volume_expected : NULL,
                                           &handle);
        if (reply.detail == GSTD_VOLUME_OK) {
            reply.status = 0;
            reply.volume = handle.identity;
            reply.descriptor_count = observed ? 1 : 2;
            reply.descriptors[0] = handle.data_fd;
            if (!observed)
                reply.descriptors[1] = handle.writer_fd;
        }
    } else if (input.action == GSTD_FS_OIDC_PREPARE) {
        gstd_bootstrap_request request = {.manifest = input.manifest,
                                          .store_id = input.store_id,
                                          .deployment_id = input.deployment_id,
                                          .operation_id = input.operation_id,
                                          .generation = input.generation,
                                          .control_owner = parent,
                                          .scope_fd = 3};
        int files[4] = {-1, -1, -1, -1};
        reply.status = gstd_oidc_provision(&request, &input.oidc_ports, 4, input.credential_source,
                                           input.credential_initial,
                                           input.credential_prior_required ? 5 : -1, files);
        if (!reply.status) {
            reply.descriptor_count = 4;
            for (size_t i = 0; i < 4; i++)
                reply.descriptors[i] = files[i];
        }
    } else if (input.action == GSTD_FS_OPEN_ACTIVE) {
        char name[138];
        if (input.asset_key && input.operation_id &&
            !gstd_asset_stage_name(input.asset_key, input.operation_id, name)) {
            int fd = directory(3, input.asset_key, 0);
            if (fd >= 3 && !verify(fd, &input.proof)) {
                reply.status = 0;
                reply.descriptor_count = 1;
                reply.descriptors[0] = fd;
            } else if (fd >= 0)
                close(fd);
        }
    } else if (input.action == GSTD_FS_IMAGE_VERIFY) {
        if (!verify(3, &input.proof))
            reply.status = 0;
    } else if (input.action == GSTD_FS_IMAGE_PROMOTE) {
        reply.detail = gstd_asset_promote(3, input.staging_name, input.asset_key, &input.proof, -1);
        reply.status = reply.detail == GSTD_CACHE_OK          ? 0
                       : reply.detail == GSTD_CACHE_UNCERTAIN ? -2
                                                              : -1;
    } else {
        gstd_image_auth auth = {0};
        reply.detail =
            input.action == GSTD_FS_IMAGE_IMPORT
                ? gstd_provision_import(input.manifest, input.component, 4, 3, input.download_limit,
                                        input.merge_limit, input.image_timeout_ms, -1, &reply.proof)
            : input.action == GSTD_FS_IMAGE_MATERIALIZE
                ? gstd_provision_materialize(input.manifest, input.component, 4, 3,
                                             input.download_limit, input.merge_limit,
                                             input.image_timeout_ms, -1, &reply.proof)
                : gstd_provision_stage(input.manifest, input.component, 3, &auth,
                                       input.download_limit, input.merge_limit,
                                       input.image_timeout_ms, -1, &reply.proof);
        reply.status = reply.detail == GSTD_PROVISION_OK ? 0 : -1;
        if (!reply.status && input.action == GSTD_FS_IMAGE_MATERIALIZE) {
            int fd = openat(3, "rootfs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 3) {
                reply.descriptor_count = 1;
                reply.descriptors[0] = fd;
            } else
                reply.status = -1;
        }
    }
#ifdef GITSTORED_TESTING
    if (input.test_pause == 2)
        raise(SIGSTOP);
#endif
    reply.cache_owner.lock_fd = -1;
    reply.volume_root.lock_fd = -1;
    reply.volume_root.root_fd = -1;
    _exit(send_reply(6, &reply) ? 1 : 0);
}
static int receive(gstd_filesystem_job *job) {
    unsigned char control[CMSG_SPACE(sizeof(int) * 8)] = {0};
    gstd_filesystem_reply reply;
    memset(&reply, 0, sizeof(reply));
    struct iovec vector = {&reply, sizeof(reply)};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
    ssize_t count = recvmsg(job->channel, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    if (count <= 0)
        return -1;
    int handles[8];
    unsigned used = 0;
    int valid = count == sizeof(reply) && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0)) {
            valid = 0;
            continue;
        }
        size_t bytes = header->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int))
            valid = 0;
        size_t n = bytes / sizeof(int);
        int *fd = (int *)CMSG_DATA(header);
        for (size_t i = 0; i < n; i++) {
            if (used < 8)
                handles[used++] = fd[i];
            else {
                close(fd[i]);
                valid = 0;
            }
        }
    }
    if (!valid || used != reply.descriptor_count ||
        (reply.status == 0 && used != job->expected_descriptors) || reply.status < -2 ||
        reply.status > 0 || (reply.status && used)) {
        for (unsigned i = 0; i < used; i++)
            close(handles[i]);
        return -1;
    }
    memcpy(reply.descriptors, handles, sizeof(int) * used);
    job->reply = reply;
    job->received = 1;
    return 1;
}
#endif
int gstd_filesystem_launch(const gstd_filesystem_input *input, unsigned timeout,
                           gstd_filesystem_job *job) {
#ifdef __linux__
    if (!input || !job || job->worker || !timeout || timeout > 600000 || !geteuid() ||
        input->action < GSTD_FS_INITIALIZE || input->action > GSTD_FS_OIDC_PREPARE ||
        !private_directory(input->descriptors[0]) ||
        (input->action >= GSTD_FS_IMAGE_STAGE && input->action <= GSTD_FS_IMAGE_MATERIALIZE &&
         (gstd_asset_manifest_check(input->manifest) || !input->component ||
          !input->image_timeout_ms || input->image_timeout_ms > 600000)))
        return -1;
    if (input->action == GSTD_FS_DATASTORE_VOLUME || input->action == GSTD_FS_OBSERVE_DATASTORE) {
        const char *datastore = json_string_value(json_object_get(input->manifest, "datastore"));
        if (!datastore || strcmp(datastore, "scylladb") ||
            gstd_asset_manifest_check(input->manifest))
            return -1;
    }
    if (input->action == GSTD_FS_OIDC_VOLUME || input->action == GSTD_FS_OBSERVE_OIDC_VOLUME) {
        if (!gstd_oidc_volume_compatibility(input->oidc_volume_role) ||
            gstd_oidc_core_manifest_check(input->manifest) ||
            !json_object_get(input->manifest, "oidcProfileDigest"))
            return -1;
    }
    if (input->action == GSTD_FS_OIDC_PREPARE &&
        (gstd_oidc_core_manifest_check(input->manifest) ||
         !json_object_get(input->manifest, "oidcProfileDigest") ||
         gstd_oidc_ports_check(&input->oidc_ports) ||
         (input->credential_initial != 0 && input->credential_initial != 1) ||
         (input->credential_prior_required != 0 && input->credential_prior_required != 1) ||
         (input->credential_prior_required ? input->descriptors[2] < 3
                                           : input->descriptors[2] != -1) ||
         (input->credential_source && !json_is_object(input->credential_source))))
        return -1;
    for (unsigned i = 0; i < 3; i++)
        if (input->descriptors[i] != -1 && input->descriptors[i] < 3)
            return -1;
    if ((input->descriptors[2] != -1 && input->action != GSTD_FS_OIDC_PREPARE) ||
        fcntl(0, F_GETFD) < 0 || fcntl(1, F_GETFD) < 0 || fcntl(2, F_GETFD) < 0)
        return -1;
    int source =
        input->action == GSTD_FS_IMAGE_IMPORT || input->action == GSTD_FS_IMAGE_MATERIALIZE ||
        input->action == GSTD_FS_VOLUME || input->action == GSTD_FS_OBSERVE_VOLUME ||
        input->action == GSTD_FS_DATASTORE_VOLUME || input->action == GSTD_FS_OBSERVE_DATASTORE ||
        input->action == GSTD_FS_OIDC_VOLUME || input->action == GSTD_FS_OBSERVE_OIDC_VOLUME ||
        input->action == GSTD_FS_OIDC_PREPARE;
    if (source ? input->descriptors[1] < 3 : input->descriptors[1] != -1)
        return -1;
    if ((input->action == GSTD_FS_VOLUME || input->action == GSTD_FS_DATASTORE_VOLUME ||
         input->action == GSTD_FS_OIDC_VOLUME) &&
        input->volume_existing != 0 && input->volume_existing != 1)
        return -1;
    if (input->test_pause > 2)
        return -1;
#ifndef GITSTORED_TESTING
    if (input->test_pause)
        return -1;
#endif
    struct sigaction action;
    DIR *tasks = opendir("/proc/self/task");
    if (!tasks)
        return -1;
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(tasks)))
        if (entry->d_name[0] != '.')
            count++;
    closedir(tasks);
    if (count != 1 || sigaction(SIGCHLD, NULL, &action) || action.sa_handler != SIG_DFL ||
        (action.sa_flags & SA_NOCLDWAIT))
        return -1;
    int64_t now = now_ms();
    int channels[2];
    if (now < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, channels))
        return -1;
    pid_t parent = getpid(), child = fork();
    if (!child)
        worker(*input, channels[1], parent);
    close(channels[1]);
    if (child < 0) {
        close(channels[0]);
        return -1;
    }
    *job = (gstd_filesystem_job){
        .worker = child,
        .channel = channels[0],
        .deadline_ms = now + timeout,
        .expected_descriptors =
            (input->action == GSTD_FS_INITIALIZE || input->action == GSTD_FS_REOPEN_ROOTS) ? 6
            : input->action == GSTD_FS_SCOPE
                ? (input->resource_kind && !strcmp(input->resource_kind, "workload") ? 3 : 2)
            : input->action == GSTD_FS_REOPEN_SCOPE
                ? (input->resource_kind && !strcmp(input->resource_kind, "network")    ? 1
                   : input->resource_kind && !strcmp(input->resource_kind, "workload") ? 4
                                                                                       : 2)
            : input->action == GSTD_FS_RECOVER_NETWORK || input->action == GSTD_FS_OIDC_PREPARE ? 4
            : (input->action == GSTD_FS_OBSERVE_VOLUME ||
               input->action == GSTD_FS_OBSERVE_DATASTORE ||
               input->action == GSTD_FS_OBSERVE_OIDC_VOLUME)
                ? 1
            : (input->action == GSTD_FS_VOLUME || input->action == GSTD_FS_DATASTORE_VOLUME ||
               input->action == GSTD_FS_OIDC_VOLUME)
                ? 2
            : input->action == GSTD_FS_IMAGE_MATERIALIZE ? 1
            : input->action == GSTD_FS_STAGE_DIRECTORY || input->action == GSTD_FS_OPEN_ACTIVE ||
                    input->action == GSTD_FS_RESOLVER
                ? 1
                : 0};
    return 0;
#else
    (void)input;
    (void)timeout;
    (void)job;
    return -1;
#endif
}
int gstd_filesystem_poll(gstd_filesystem_job *job, int cancel, gstd_filesystem_reply *reply) {
    if (!job || job->worker <= 0 || !reply || cancel < -1)
        return -1;
#ifdef __linux__
    int64_t now = now_ms();
    struct pollfd cancellation = {.fd = cancel, .events = POLLIN};
    int abort = now < 0 || now >= job->deadline_ms ||
                (cancel >= 0 && poll(&cancellation, 1, 0) > 0 && cancellation.revents);
    if (!job->interrupted && !job->received && !abort && receive(job) < 0)
        abort = 1;
    if (!job->interrupted && abort) {
        if (kill(job->worker, SIGKILL) && errno != ESRCH)
            return -1;
        job->interrupted = 1;
    }
    int status = 0;
    pid_t waited = waitpid(job->worker, &status, WNOHANG);
    if (!waited || (waited < 0 && errno == EINTR))
        return 0;
    if (waited == job->worker && WIFEXITED(status) && !WEXITSTATUS(status) && !job->interrupted) {
        if (!job->received)
            receive(job);
        if (job->received)
            *reply = job->reply;
        else {
            memset(reply, 0, sizeof(*reply));
            reply->status = -2;
        }
    } else {
        gstd_filesystem_reply_close(&job->reply);
        memset(reply, 0, sizeof(*reply));
        reply->status = -2;
    }
    close(job->channel);
    memset(job, 0, sizeof(*job));
    job->channel = -1;
    return 1;
#else
    (void)cancel;
    return -1;
#endif
}
