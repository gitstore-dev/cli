#define _GNU_SOURCE 1
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "network_worker.h"
#include "oidc_profile.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC network owner line %d\n", __LINE__);                             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static void pause_poll(void) { usleep(1000); }
static int save(int root, const char *name, const json_t *value) {
    char *bytes = gstd_json_dump(value);
    if (!bytes)
        return -1;
    int fd = openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    size_t length = strlen(bytes);
    int ok = fd >= 3 && write(fd, bytes, length) == (ssize_t)length && !fsync(fd) && !fsync(root);
    if (fd >= 3)
        close(fd);
    free(bytes);
    return ok ? 0 : -1;
}
static int choose(gstd_oidc_ports *ports) {
    int sockets[5];
    uint16_t values[5];
    for (size_t i = 0; i < 5; i++) {
        sockets[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(sockets[i] >= 3);
        struct sockaddr_in address = {.sin_family = AF_INET,
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        CHECK(!bind(sockets[i], (struct sockaddr *)&address, sizeof(address)));
        socklen_t length = sizeof(address);
        CHECK(!getsockname(sockets[i], (struct sockaddr *)&address, &length));
        values[i] = ntohs(address.sin_port);
    }
    *ports = (gstd_oidc_ports){values[0], values[1], values[2], values[3], values[4]};
    for (size_t i = 0; i < 5; i++)
        close(sockets[i]);
    return gstd_oidc_ports_check(ports);
}
static int guest(const gstd_network_namespace *network, const gstd_oidc_ports *ports, int report) {
    pid_t parent = getppid();
    CHECK(!setns(network->user_fd, CLONE_NEWUSER) && !setns(network->network_fd, CLONE_NEWNET));
    CHECK(!prctl(PR_SET_PDEATHSIG, SIGKILL) && getppid() == parent);
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2] = {{0}};
    CHECK(!syscall(SYS_capset, &header, caps) && !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
    CHECK(dup2(report, 3) == 3 && !syscall(SYS_close_range, 4U, ~0U, 0));
    alarm(10);
    int listeners[5];
    const uint16_t values[] = {ports->issuer, ports->bridge, ports->identity, ports->ui,
                               ports->mail};
    for (size_t i = 0; i < 5; i++) {
        listeners[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(listeners[i] >= 4);
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(values[i])};
        CHECK(inet_pton(AF_INET, "10.0.2.15", &address.sin_addr) == 1);
        CHECK(!bind(listeners[i], (struct sockaddr *)&address, sizeof(address)) &&
              !listen(listeners[i], 2));
    }
    CHECK(write(3, "r", 1) == 1);
    close(3);
    for (size_t i = 0; i < 5; i++) {
        int client = accept4(listeners[i], NULL, NULL, SOCK_CLOEXEC);
        CHECK(client >= 3);
        unsigned char byte = (unsigned char)i;
        CHECK(write(client, &byte, 1) == 1);
        close(client);
        close(listeners[i]);
    }
    return 0;
}
static int traffic(const gstd_network_worker_observation *actual) {
    int reports[2];
    CHECK(!pipe2(reports, O_CLOEXEC));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(reports[0]);
        _exit(guest(&actual->network, &actual->oidc_ports, reports[1]));
    }
    close(reports[1]);
    struct pollfd ready = {reports[0], POLLIN, 0};
    CHECK(poll(&ready, 1, 5000) == 1);
    char byte;
    CHECK(read(reports[0], &byte, 1) == 1 && byte == 'r');
    close(reports[0]);
    const uint16_t ports[] = {actual->oidc_ports.issuer, actual->oidc_ports.bridge,
                              actual->oidc_ports.identity, actual->oidc_ports.ui,
                              actual->oidc_ports.mail};
    for (size_t i = 0; i < 5; i++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(fd >= 3);
        struct sockaddr_in address = {.sin_family = AF_INET,
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                                      .sin_port = htons(ports[i])};
        CHECK(!connect(fd, (struct sockaddr *)&address, sizeof(address)));
        struct pollfd data = {fd, POLLIN, 0};
        CHECK(poll(&data, 1, 5000) == 1);
        unsigned char response = 255;
        CHECK(read(fd, &response, 1) == 1 && response == i);
        close(fd);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    return 0;
}
static int run(int root, const json_t *manifest, const gstd_oidc_ports *ports, const char *name,
               int lost, int conflict) {
    CHECK(!mkdirat(root, name, 0700));
    int scope = openat(root, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(scope >= 3);
    gstd_network_worker_request request = {.manifest = manifest,
                                           .store_id = "0123456789abcdef0123456789abcdef",
                                           .resource_id = name,
                                           .operation_id = "oidc-network-fixture",
                                           .scope_fd = scope,
                                           .oidc_ports = ports};
    gstd_network_worker owner = GSTD_NETWORK_WORKER_INITIAL;
    gstd_network_worker_observation actual = GSTD_NETWORK_OBSERVATION_INITIAL;
    CHECK(gstd_network_worker_prepare(&request, &owner) == GSTD_NETWORK_OK);
    json_t *identity = gstd_network_worker_identity(&owner);
    CHECK(identity && json_object_size(identity) == 12);
    char receipt[100];
    snprintf(receipt, sizeof(receipt), "%s-parent.json", name);
    CHECK(!save(root, receipt, identity));
    CHECK(!gstd_network_worker_permit(&owner));
    pid_t original = owner.pid;
    if (lost) {
        gstd_network_worker_close(&owner);
        for (int i = 0; i < 5000 && faccessat(scope, "ready.json", F_OK, AT_SYMLINK_NOFOLLOW); i++)
            pause_poll();
        CHECK(!gstd_network_worker_reopen(&request, identity, &owner, &actual));
    } else {
        int status = 0;
        for (int i = 0; i < 5000 && !(status = gstd_network_worker_ready(&owner, &actual)); i++)
            pause_poll();
        CHECK(status == (conflict ? -1 : 1));
    }
    if (!conflict) {
        unsigned roles = 0;
        CHECK(!gstd_network_request_roles(&request, &roles));
        CHECK((roles & 1 ? actual.api_port >= 1024 : !actual.api_port) &&
              (roles & 2 ? actual.git_port >= 1024 : !actual.git_port) &&
              actual.oidc_ports.issuer == ports->issuer);
        CHECK(!traffic(&actual));
        gstd_oidc_ports swapped = *ports;
        swapped.issuer = ports->bridge;
        swapped.bridge = ports->issuer;
        gstd_network_worker_request wrong = request;
        wrong.oidc_ports = &swapped;
        gstd_network_worker rejected = GSTD_NETWORK_WORKER_INITIAL;
        gstd_network_worker_observation rejected_actual = GSTD_NETWORK_OBSERVATION_INITIAL;
        CHECK(gstd_network_worker_reopen(&wrong, identity, &rejected, &rejected_actual) == -1);
        json_t *forged = json_deep_copy(identity), *tuple = gstd_network_oidc_ports_json(&swapped);
        CHECK(forged && tuple && !json_object_set_new(forged, "oidcPorts", tuple));
        CHECK(gstd_network_worker_reopen(&wrong, forged, &rejected, &rejected_actual) == -1);
        json_decref(forged);
        CHECK(!gstd_network_worker_signal(&owner, 0));
    }
    struct pollfd exited = {owner.pid_fd, POLLIN, 0};
    CHECK(poll(&exited, 1, 5000) == 1);
    int status;
    CHECK(waitpid(original, &status, 0) == original && WIFEXITED(status) &&
          WEXITSTATUS(status) == (conflict ? 1 : 0));
    if (conflict)
        CHECK(faccessat(scope, "ready.json", F_OK, AT_SYMLINK_NOFOLLOW) < 0);
    gstd_network_worker_observation_close(&actual);
    gstd_network_worker_close(&owner);
    json_decref(identity);
    close(scope);
    return 0;
}
int main(int argc, char **argv) {
    CHECK((argc == 2 || (argc == 3 && !strcmp(argv[2], "combined"))) && argv[1][0] == '/' &&
          geteuid());
    alarm(45);
    gstd_json_initialize();
    struct rlimit core = {0, 0}, files = {67108864, 67108864};
    CHECK(!setrlimit(RLIMIT_CORE, &core) && !setrlimit(RLIMIT_FSIZE, &files));
#ifndef GITSTORED_INSTRUMENTED
    struct rlimit memory = {536870912, 536870912};
    CHECK(!setrlimit(RLIMIT_AS, &memory));
#endif
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    CHECK(root >= 3 && !fstat(root, &info) && info.st_uid == geteuid() &&
          (info.st_mode & 07777) == 0700);
    json_t *selection =
        argc == 3 ? json_pack("[s,s]", "controller", "oidc") : json_pack("[s]", "oidc");
#if defined(__aarch64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    json_t *manifest = selection ? gstd_oidc_core_manifest(selection, architecture, "memdb") : NULL;
    json_decref(selection);
    CHECK(manifest);
    gstd_oidc_ports ports = {0};
    CHECK(!choose(&ports));
    json_t *tuple = gstd_network_oidc_ports_json(&ports);
    CHECK(tuple && !save(root, "ports.json", tuple));
    json_decref(tuple);
    CHECK(!run(root, manifest, &ports, "first", 0, 0));
    int occupied = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), reuse = 1;
    CHECK(occupied >= 3 && !setsockopt(occupied, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                                  .sin_port = htons(ports.issuer)};
    CHECK(!bind(occupied, (struct sockaddr *)&address, sizeof(address)) && !listen(occupied, 1));
    CHECK(!run(root, manifest, &ports, "conflict", 0, 1));
    close(occupied);
    CHECK(!run(root, manifest, &ports, "lost-ack-restart", 1, 0));
    json_decref(manifest);
    close(root);
    puts("{\"fixedPublicForwards\":5,\"samePortsAfterRestart\":true,\"lostAckRecovered\":true,"
         "\"bindConflictNoFallback\":true,\"forgedPortOwnershipRejected\":true,\"workersReaped\":"
         "3}");
    return 0;
}
