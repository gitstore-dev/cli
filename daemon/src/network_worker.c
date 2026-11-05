#define _GNU_SOURCE 1
#include "network_worker.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "network_sandbox.h"
#include "oidc_profile.h"
#include <stdio.h>
#include <string.h>
json_t *gstd_network_oidc_ports_json(const gstd_oidc_ports *ports) {
    return ports && !gstd_oidc_ports_check(ports)
               ? json_pack("[i,i,i,i,i]", (int)ports->issuer, (int)ports->bridge,
                           (int)ports->identity, (int)ports->ui, (int)ports->mail)
               : NULL;
}
int gstd_network_oidc_ports_parse(const json_t *value, gstd_oidc_ports *output) {
    if (!output || !json_is_array(value) || json_array_size(value) != 5)
        return -1;
    uint16_t ports[5];
    for (size_t i = 0; i < 5; i++) {
        const json_t *port = json_array_get(value, i);
        if (!json_is_integer(port) || json_integer_value(port) < 1024 ||
            json_integer_value(port) > UINT16_MAX)
            return -1;
        ports[i] = (uint16_t)json_integer_value(port);
    }
    gstd_oidc_ports selected = {ports[0], ports[1], ports[2], ports[3], ports[4]};
    if (gstd_oidc_ports_check(&selected))
        return -1;
    *output = selected;
    return 0;
}
int gstd_network_request_roles(const gstd_network_worker_request *request, unsigned *roles) {
    if (!request || !roles || gstd_asset_manifest_check(request->manifest))
        return -1;
    unsigned selected = 0, oidc = 0;
    json_t *images = json_object_get(request->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *name =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (!name)
            return -1;
        if (!strcmp(name, "api"))
            selected |= 1;
        if (!strcmp(name, "git"))
            selected |= 2;
        if (gstd_oidc_component(name))
            oidc++;
    }
    if (oidc) {
        if (oidc != 5 || !request->oidc_ports || gstd_oidc_ports_check(request->oidc_ports))
            return -1;
        selected |= 124;
    } else if (request->oidc_ports)
        return -1;
    *roles = selected;
    return 0;
}
json_t *gstd_network_worker_identity(const gstd_network_worker *owner) {
    if (!owner || owner->pid <= 0 || !owner->start || owner->start > INT64_MAX ||
        !owner->scope_inode || strnlen(owner->store_id, 33) != 32 ||
        strnlen(owner->boot_id, 37) != 36 ||
        !gstd_identifier(owner->resource_id, strnlen(owner->resource_id, 65)) ||
        !gstd_identifier(owner->operation_id, strnlen(owner->operation_id, 65)) ||
        strnlen(owner->profile_digest, 72) != 71 || owner->roles > 127 ||
        (owner->roles > 3 && (owner->roles & 124) != 124))
        return NULL;
    json_t *value = json_pack("{s:i,s:s,s:s,s:s,s:s,s:i,s:I,s:s,s:s}", "schemaVersion", 1,
                              "storeId", owner->store_id, "resourceId", owner->resource_id,
                              "operationId", owner->operation_id, "profileDigest",
                              owner->profile_digest, "pid", (int)owner->pid, "startTime",
                              (json_int_t)owner->start, "scopeDevice", "", "scopeInode", "");
    if (!value)
        return NULL;
    if (json_object_set_new(value, "bootId", json_string(owner->boot_id)) ||
        json_object_set_new(value, "roles", json_integer(owner->roles))) {
        json_decref(value);
        return NULL;
    }
    if (owner->roles & 124) {
        json_t *ports = gstd_network_oidc_ports_json(&owner->oidc_ports);
        if (!ports || json_object_set_new(value, "oidcPorts", ports)) {
            json_decref(value);
            return NULL;
        }
    }
    char device[32], inode[32];
    snprintf(device, sizeof(device), "%llu", (unsigned long long)owner->scope_device);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)owner->scope_inode);
    if (json_object_set_new(value, "scopeDevice", json_string(device)) ||
        json_object_set_new(value, "scopeInode", json_string(inode))) {
        json_decref(value);
        return NULL;
    }
    return value;
}
#if defined(__linux__) && defined(GITSTORED_SLIRP)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define WORKER_MAGIC UINT32_C(0x47535731)
typedef struct {
    uint32_t magic, status;
    char boot_id[37];
    pid_t pid, keeper;
    uint64_t start, keeper_start, user_device, user_inode, network_device, network_inode;
    uint16_t api_port, git_port;
    int api_descriptor, git_descriptor;
    uint64_t api_inode, git_inode;
    gstd_oidc_ports oidc_ports;
    int oidc_descriptors[5];
    uint64_t oidc_inodes[5];
} ready_report;
static volatile sig_atomic_t stopping;
static void stop(int number) {
    (void)number;
    stopping = 1;
}
static int identifier(const char *text) { return text && gstd_identifier(text, strnlen(text, 65)); }
static int store_id(const char *text) {
    if (!text || strnlen(text, 33) != 32)
        return 0;
    for (size_t i = 0; i < 32; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return 1;
}
static int directory(int fd, struct stat *info) {
    return fd >= 3 && !fstat(fd, info) && S_ISDIR(info->st_mode) && info->st_uid == geteuid() &&
           (info->st_mode & 07777) == 0700;
}
static int scope_empty(int fd) {
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (copy < 0)
        return 0;
    DIR *directory = fdopendir(copy);
    if (!directory) {
        close(copy);
        return 0;
    }
    rewinddir(directory);
    struct dirent *entry;
    int valid = 1;
    errno = 0;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
            strcmp(entry->d_name, "worker.lock")) {
            valid = 0;
            break;
        }
    if (errno)
        valid = 0;
    closedir(directory);
    return valid;
}
static int single_thread(void) {
    struct sigaction action;
    if (sigaction(SIGCHLD, NULL, &action) || action.sa_handler != SIG_DFL ||
        (action.sa_flags & SA_NOCLDWAIT))
        return 0;
    DIR *directory = opendir("/proc/self/task");
    if (!directory)
        return 0;
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.')
            count++;
    closedir(directory);
    return count == 1;
}
static int bounded(int kind, rlim_t maximum) {
    struct rlimit value;
    if (getrlimit(kind, &value))
        return -1;
    if (value.rlim_cur > maximum)
        value.rlim_cur = maximum;
    if (value.rlim_max > maximum)
        value.rlim_max = maximum;
    return setrlimit(kind, &value);
}
static int write_bytes(int directory, const char *name, const void *bytes, size_t length) {
    if (!length || length > 4096)
        return -1;
    int fd = openat(directory, name,
                    O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
        return -1;
    struct stat info;
    int valid = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                info.st_nlink == 1 && (info.st_mode & 07777) == 0600;
    size_t sent = 0;
    while (valid && sent < length) {
        ssize_t count = write(fd, (const char *)bytes + sent, length - sent);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            valid = 0;
            break;
        }
        sent += (size_t)count;
    }
    valid = valid && !fsync(fd);
    close(fd);
    return valid && !fsync(directory) ? 0 : -1;
}
static int record(int directory, const char *name, const gstd_network_worker *owner,
                  const ready_report *proof) {
    json_t *value = gstd_network_worker_identity(owner);
    if (!value)
        return -1;
    int failed = 0;
    if (proof) {
        json_t *actual = json_pack(
            "{s:i,s:I,s:I,s:I,s:I,s:I,s:i,s:i,s:i,s:i,s:I,s:I}", "keeperPid", (int)proof->keeper,
            "keeperStart", (json_int_t)proof->keeper_start, "userDevice",
            (json_int_t)proof->user_device, "userInode", (json_int_t)proof->user_inode,
            "networkDevice", (json_int_t)proof->network_device, "networkInode",
            (json_int_t)proof->network_inode, "apiPort", (int)proof->api_port, "gitPort",
            (int)proof->git_port, "apiDescriptor", proof->api_descriptor, "gitDescriptor",
            proof->git_descriptor, "apiInode", (json_int_t)proof->api_inode, "gitInode",
            (json_int_t)proof->git_inode);
        if (actual && (owner->roles & 124)) {
            const uint16_t ports[] = {proof->oidc_ports.issuer, proof->oidc_ports.bridge,
                                      proof->oidc_ports.identity, proof->oidc_ports.ui,
                                      proof->oidc_ports.mail};
            json_t *listeners = json_array();
            for (size_t i = 0; listeners && i < 5; i++) {
                json_t *listener = json_pack("{s:i,s:i,s:I}", "port", (int)ports[i], "descriptor",
                                             proof->oidc_descriptors[i], "inode",
                                             (json_int_t)proof->oidc_inodes[i]);
                if (!listener || json_array_append_new(listeners, listener)) {
                    json_decref(listeners);
                    listeners = NULL;
                    break;
                }
            }
            if (!listeners || json_object_set_new(actual, "oidcListeners", listeners))
                failed = 1;
        }
        if (!actual || json_object_set_new(value, "observed", actual))
            failed = 1;
    }
    char *bytes = failed ? NULL : gstd_json_dump(value);
    json_decref(value);
    int result = bytes ? write_bytes(directory, name, bytes, strlen(bytes)) : -1;
    free(bytes);
    return result;
}
static int send_report(int channel, const ready_report *report,
                       const gstd_network_namespace *network) {
    int fds[2] = {network ? network->user_fd : -1, network ? network->network_fd : -1};
    char control[CMSG_SPACE(sizeof(fds))] = {0};
    struct iovec vector = {(void *)report, sizeof(*report)};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    if (network) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(fds));
        memcpy(CMSG_DATA(header), fds, sizeof(fds));
    }
    return sendmsg(channel, &message, MSG_NOSIGNAL) == (ssize_t)sizeof(*report) ? 0 : -1;
}
static void run(const gstd_network_worker *expected, int channel, int scope, int lock,
                unsigned roles, pid_t parent, long maximum) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
        _exit(126);
    int original[3] = {channel, scope, lock}, copied[3];
    for (size_t i = 0; i < 3; i++)
        if ((copied[i] = fcntl(original[i], F_DUPFD_CLOEXEC, 32)) < 0)
            _exit(126);
    for (size_t i = 0; i < 3; i++)
        if (dup2(copied[i], 3 + (int)i) < 0 || fcntl(3 + (int)i, F_SETFD, FD_CLOEXEC))
            _exit(126);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
        _exit(126);
    for (int i = 0; i < 3; i++)
        if (dup2(null, i) < 0 || fcntl(i, F_SETFD, 0))
            _exit(126);
    if (null >= 3)
        close(null);
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 6u, UINT_MAX, 0u))
#endif
        for (int fd = 6; fd < maximum; fd++)
            close(fd);
    struct pollfd grant = {3, POLLIN, 0};
    char permit[2];
    if (poll(&grant, 1, 30000) <= 0 || recv(3, permit, sizeof(permit), MSG_TRUNC) != 1 ||
        permit[0] != 'G')
        _exit(125);
    if (prctl(PR_SET_PDEATHSIG, 0) || clearenv() || setenv("PATH", "/nonexistent", 1) ||
        setenv("LANG", "C", 1) || setenv("HOME", "/nonexistent", 1) || bounded(RLIMIT_CORE, 0) ||
        bounded(RLIMIT_FSIZE, 67108864) || bounded(RLIMIT_NOFILE, 1024))
        _exit(126);
#ifndef GITSTORED_INSTRUMENTED
    if (bounded(RLIMIT_AS, 536870912))
        _exit(126);
#endif
    umask(077);
    stopping = 0;
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL))
        _exit(126);
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, NULL))
        _exit(126);
    gstd_network_worker owner = *expected;
    owner.pid = getpid();
    if (gstd_native_process_start(getpid(), &owner.start) || record(4, "owner.json", &owner, NULL))
        _exit(126);
    ready_report reply = {.magic = WORKER_MAGIC,
                          .status = GSTD_NETWORK_FAILED,
                          .pid = owner.pid,
                          .start = owner.start,
                          .api_descriptor = -1,
                          .git_descriptor = -1};
    for (size_t i = 0; i < 5; i++)
        reply.oidc_descriptors[i] = -1;
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    gstd_network_engine *engine = NULL;
    memcpy(reply.boot_id, owner.boot_id, 37);
    int success = 0, channel_active = 1;
    if (stopping || gstd_network_namespace_open(&network) != GSTD_NETWORK_OK)
        goto done;
    reply.keeper = network.keeper_pid;
    reply.keeper_start = network.keeper_start;
    reply.user_device = network.user_device;
    reply.user_inode = network.user_inode;
    reply.network_device = network.network_device;
    reply.network_inode = network.network_inode;
    if (record(4, "namespace.json", &owner, &reply))
        goto done;
    static const char resolver[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
    if (write_bytes(4, "resolver.conf", resolver, sizeof(resolver) - 1) || stopping ||
        gstd_network_open(network.tap_fd, &engine) != GSTD_NETWORK_OK)
        goto done;
    if (roles & 124) {
        const uint16_t ports[] = {owner.oidc_ports.issuer, owner.oidc_ports.bridge,
                                  owner.oidc_ports.identity, owner.oidc_ports.ui,
                                  owner.oidc_ports.mail};
        for (size_t i = 0; i < 5; i++) {
            uint16_t actual = 0;
            gstd_forward_role role = (gstd_forward_role)(GSTD_FORWARD_OIDC_ISSUER + i);
            if (gstd_network_forward_bound(engine, role, ports[i], &actual) != GSTD_NETWORK_OK ||
                actual != ports[i] ||
                gstd_network_listener(engine, role, &reply.oidc_descriptors[i],
                                      &reply.oidc_inodes[i]))
                goto done;
        }
        reply.oidc_ports = owner.oidc_ports;
    }
    if ((roles & 1) &&
        gstd_network_forward(engine, GSTD_FORWARD_API, &reply.api_port) != GSTD_NETWORK_OK)
        goto done;
    if ((roles & 2) &&
        gstd_network_forward(engine, GSTD_FORWARD_GIT, &reply.git_port) != GSTD_NETWORK_OK)
        goto done;
    if ((roles & 1) &&
        gstd_network_listener(engine, GSTD_FORWARD_API, &reply.api_descriptor, &reply.api_inode))
        goto done;
    if ((roles & 2) &&
        gstd_network_listener(engine, GSTD_FORWARD_GIT, &reply.git_descriptor, &reply.git_inode))
        goto done;
    int confined = gstd_network_sandbox(4);
    if (confined) {
        char failure[96];
        int length = snprintf(failure, sizeof(failure), "{\"sandboxStage\":%d,\"errno\":%d}",
                              -confined, errno);
        if (length > 0 && (size_t)length < sizeof(failure))
            (void)write_bytes(4, "failure.json", failure, (size_t)length);
        goto done;
    }
    reply.status = GSTD_NETWORK_OK;
    if (record(4, "ready.json", &owner, &reply))
        goto done;
    /* A lost acknowledgement leaves a recorded owned detached worker, never an
     * inferred rollback. No private packet/TAP/lifeline FD crosses this channel. */
    (void)send_report(3, &reply, &network);
    close(3);
    channel_active = 0;
    close(4); /* no directory descriptor outside the private read-only root */
    success = 1;
    while (!stopping)
        if (gstd_network_step(engine, -1, 50) != GSTD_NETWORK_OK) {
            success = 0;
            break;
        }
done:
    if (!success && channel_active) {
        reply.status = GSTD_NETWORK_FAILED;
        (void)send_report(3, &reply, NULL);
        close(3);
    }
    if (gstd_network_close(engine) != GSTD_NETWORK_OK)
        success = 0;
    if (gstd_network_namespace_close(&network) != GSTD_NETWORK_OK)
        success = 0;
    if (channel_active)
        close(4);
    close(5);
    _exit(success ? 0 : 1);
}
#endif
gstd_network_status gstd_network_worker_prepare(const gstd_network_worker_request *request,
                                                gstd_network_worker *output) {
    if (!request || !output || output->pid || output->channel != -1 || output->pid_fd != -1 ||
        output->scope_fd != -1)
        return GSTD_NETWORK_INVALID;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    struct stat scope, info;
    long maximum = sysconf(_SC_OPEN_MAX);
    if (!store_id(request->store_id) || !identifier(request->resource_id) ||
        !identifier(request->operation_id) || gstd_asset_manifest_check(request->manifest) ||
        !directory(request->scope_fd, &scope) || !scope_empty(request->scope_fd))
        return GSTD_NETWORK_INVALID;
    char boot[37];
    if (!geteuid() || maximum < 0 || prctl(PR_GET_DUMPABLE) != 1 || !single_thread() ||
        gstd_native_boot_id(boot))
        return GSTD_NETWORK_HOST_UNMET;
    unsigned roles = 0;
    if (gstd_network_request_roles(request, &roles))
        return GSTD_NETWORK_INVALID;
    int lock = openat(request->scope_fd, "worker.lock",
                      O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (lock < 0)
        return GSTD_NETWORK_FAILED;
    if (fstat(lock, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        info.st_nlink != 1 || info.st_size || (info.st_mode & 07777) != 0600 ||
        flock(lock, LOCK_EX | LOCK_NB)) {
        close(lock);
        return GSTD_NETWORK_FAILED;
    }
    int held_scope = fcntl(request->scope_fd, F_DUPFD_CLOEXEC, 3), channels[2];
    if (held_scope < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels)) {
        if (held_scope >= 0)
            close(held_scope);
        close(lock);
        return GSTD_NETWORK_FAILED;
    }
    *output = (gstd_network_worker)GSTD_NETWORK_WORKER_INITIAL;
    output->roles = roles;
    if (roles & 124)
        output->oidc_ports = *request->oidc_ports;
    memcpy(output->boot_id, boot, 37);
    output->scope_fd = held_scope;
    output->scope_device = (uint64_t)scope.st_dev;
    output->scope_inode = (uint64_t)scope.st_ino;
    snprintf(output->store_id, sizeof(output->store_id), "%s", request->store_id);
    snprintf(output->resource_id, sizeof(output->resource_id), "%s", request->resource_id);
    snprintf(output->operation_id, sizeof(output->operation_id), "%s", request->operation_id);
    snprintf(output->profile_digest, sizeof(output->profile_digest), "%s",
             json_string_value(json_object_get(request->manifest, "profileDigest")));
    pid_t parent = getpid(), child = fork();
    if (!child)
        run(output, channels[1], held_scope, lock, roles, parent, maximum);
    close(channels[1]);
    close(lock);
    if (child < 0) {
        close(channels[0]);
        gstd_network_worker_close(output);
        return GSTD_NETWORK_FAILED;
    }
    output->pid = child;
    output->channel = channels[0];
    output->pid_fd = (int)syscall(SYS_pidfd_open, child, 0);
    if (output->pid_fd < 3 || gstd_native_process_start(child, &output->start)) {
        kill(child, SIGKILL);
        close(output->channel);
        output->channel = -1;
        for (unsigned attempt = 0; attempt < 500; attempt++) {
            int status;
            pid_t observed = waitpid(child, &status, WNOHANG);
            if (observed == child) {
                gstd_network_worker_close(output);
                break;
            }
            if (observed < 0 && errno != EINTR)
                break;
            struct timespec delay = {0, 1000000};
            nanosleep(&delay, NULL);
        }
        return GSTD_NETWORK_FAILED;
    }
    return GSTD_NETWORK_OK;
#else
    return GSTD_NETWORK_UNAVAILABLE;
#endif
}
int gstd_network_worker_permit(gstd_network_worker *worker) {
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (!worker || worker->pid <= 0 || worker->channel < 3 || worker->permitted)
        return -1;
    if (send(worker->channel, "G", 1, MSG_NOSIGNAL | MSG_DONTWAIT) != 1)
        return -1;
    worker->permitted = 1;
    return 0;
#else
    (void)worker;
    return -1;
#endif
}
int gstd_network_worker_ready(gstd_network_worker *worker,
                              gstd_network_worker_observation *output) {
    if (!worker || !output || worker->pid <= 0 || worker->channel < 3 || !worker->permitted ||
        worker->received || output->network.user_fd != -1 || output->network.network_fd != -1)
        return -1;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    ready_report reply = {0};
    int fds[2] = {-1, -1};
    char ancillary[CMSG_SPACE(sizeof(fds))] = {0};
    struct iovec vector = {&reply, sizeof(reply)};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = ancillary,
                             .msg_controllen = sizeof(ancillary)};
    ssize_t count = recvmsg(worker->channel, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    worker->received = 1;
    int descriptors = 1;
    size_t received = 0, headers = 0;
    if (count >= 0)
        for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
             header = CMSG_NXTHDR(&message, header)) {
            headers++;
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
                header->cmsg_len < CMSG_LEN(0) || (header->cmsg_len - CMSG_LEN(0)) % sizeof(int)) {
                descriptors = 0;
                continue;
            }
            size_t number = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < number; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(fd));
                if (received < 2)
                    fds[received++] = fd;
                else {
                    close(fd);
                    descriptors = 0;
                }
            }
        }
    int valid = count == (ssize_t)sizeof(reply) &&
                !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) && descriptors && headers == 1 &&
                received == 2 && reply.magic == WORKER_MAGIC && reply.status == GSTD_NETWORK_OK &&
                reply.pid == worker->pid && reply.start == worker->start &&
                !memcmp(reply.boot_id, worker->boot_id, 37);
    if (valid) {
        int api = worker->roles & 1, git = worker->roles & 2;
        valid = (api ? reply.api_port >= 1024 && reply.api_descriptor >= 3 && reply.api_inode
                     : !reply.api_port && reply.api_descriptor == -1 && !reply.api_inode) &&
                (git ? reply.git_port >= 1024 && reply.git_descriptor >= 3 && reply.git_inode
                     : !reply.git_port && reply.git_descriptor == -1 && !reply.git_inode) &&
                (!api || !git || reply.api_port != reply.git_port);
        const uint16_t ports[] = {reply.oidc_ports.issuer, reply.oidc_ports.bridge,
                                  reply.oidc_ports.identity, reply.oidc_ports.ui,
                                  reply.oidc_ports.mail};
        if (worker->roles & 124) {
            json_t *expected = gstd_network_oidc_ports_json(&worker->oidc_ports),
                   *actual = gstd_network_oidc_ports_json(&reply.oidc_ports);
            valid = valid && expected && actual && json_equal(expected, actual);
            json_decref(expected);
            json_decref(actual);
        }
        for (size_t i = 0; i < 5; i++) {
            if (worker->roles & 124) {
                valid = valid && reply.oidc_descriptors[i] >= 3 &&
                        reply.oidc_descriptors[i] < 1024 && reply.oidc_inodes[i] &&
                        ports[i] != reply.api_port && ports[i] != reply.git_port;
                for (size_t j = 0; j < i; j++)
                    valid = valid && reply.oidc_descriptors[i] != reply.oidc_descriptors[j] &&
                            reply.oidc_inodes[i] != reply.oidc_inodes[j];
            } else
                valid =
                    valid && !ports[i] && reply.oidc_descriptors[i] == -1 && !reply.oidc_inodes[i];
        }
        struct pollfd exited = {worker->pid_fd, POLLIN, 0};
        if (poll(&exited, 1, 0) != 0)
            valid = 0;
    }
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    if (valid) {
        memcpy(network.boot_id, worker->boot_id, 37);
        network.keeper_pid = reply.keeper;
        network.keeper_start = reply.keeper_start;
        network.user_fd = fds[0];
        network.network_fd = fds[1];
        network.user_device = reply.user_device;
        network.user_inode = reply.user_inode;
        network.network_device = reply.network_device;
        network.network_inode = reply.network_inode;
        valid = !gstd_network_namespace_check(&network);
    }
    if (valid) {
        *output = (gstd_network_worker_observation){.pid = reply.pid,
                                                    .start = reply.start,
                                                    .network = network,
                                                    .api_port = reply.api_port,
                                                    .git_port = reply.git_port,
                                                    .api_descriptor = reply.api_descriptor,
                                                    .git_descriptor = reply.git_descriptor,
                                                    .api_inode = reply.api_inode,
                                                    .git_inode = reply.git_inode,
                                                    .oidc_ports = reply.oidc_ports};
        memcpy(output->oidc_descriptors, reply.oidc_descriptors, sizeof(reply.oidc_descriptors));
        memcpy(output->oidc_inodes, reply.oidc_inodes, sizeof(reply.oidc_inodes));
    } else
        for (size_t i = 0; i < 2; i++)
            if (fds[i] >= 0)
                close(fds[i]);
    close(worker->channel);
    worker->channel = -1;
    return valid ? 1 : -1;
#else
    return -1;
#endif
}
int gstd_network_worker_signal(gstd_network_worker *worker, int force) {
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (!worker || worker->pid <= 0 || worker->pid_fd < 3 || (force != 0 && force != 1))
        return -1;
    char boot[37];
    if (gstd_native_boot_id(boot) || memcmp(boot, worker->boot_id, 37))
        return -1;
    struct pollfd exited = {worker->pid_fd, POLLIN, 0};
    if (poll(&exited, 1, 0) > 0 && (exited.revents & POLLIN))
        return 0;
    uint64_t start = 0;
    if (gstd_native_process_start(worker->pid, &start) || start != worker->start)
        return -1;
    return syscall(SYS_pidfd_send_signal, worker->pid_fd, force ? SIGKILL : SIGTERM, NULL, 0) ? -1
                                                                                              : 0;
#else
    (void)worker;
    (void)force;
    return -1;
#endif
}
void gstd_network_worker_observation_close(gstd_network_worker_observation *output) {
    if (!output)
        return;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (output->network.user_fd >= 3)
        close(output->network.user_fd);
    if (output->network.network_fd >= 3)
        close(output->network.network_fd);
#endif
    output->network.user_fd = output->network.network_fd = -1;
}
void gstd_network_worker_close(gstd_network_worker *worker) {
    if (!worker)
        return;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (worker->channel >= 3)
        close(worker->channel);
    if (worker->pid_fd >= 3)
        close(worker->pid_fd);
    if (worker->scope_fd >= 3)
        close(worker->scope_fd);
#endif
    *worker = (gstd_network_worker)GSTD_NETWORK_WORKER_INITIAL;
}
