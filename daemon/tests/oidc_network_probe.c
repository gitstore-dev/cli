#define _GNU_SOURCE 1
#include "namespace_owner.h"
#include "networking.h"
#include "oidc_config.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC network line %d\n", __LINE__);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static struct sockaddr_in address(uint16_t port, int guest) {
    struct sockaddr_in value = {.sin_family = AF_INET, .sin_port = htons(port)};
    CHECK(inet_pton(AF_INET, guest ? "10.0.2.15" : "127.0.0.1", &value.sin_addr) == 1);
    return value;
}
static int listener(uint16_t port, int guest, uint16_t *assigned) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 3);
    struct sockaddr_in value = address(port, guest);
    CHECK(!bind(fd, (struct sockaddr *)&value, sizeof(value)) && !listen(fd, 8));
    socklen_t length = sizeof(value);
    CHECK(!getsockname(fd, (struct sockaddr *)&value, &length));
    if (assigned)
        *assigned = ntohs(value.sin_port);
    return fd;
}
static void guest(const gstd_network_namespace *network, const uint16_t ports[7], int started) {
    CHECK(!prctl(PR_SET_PDEATHSIG, SIGKILL));
    alarm(10);
    CHECK(!setns(network->user_fd, CLONE_NEWUSER) && !setns(network->network_fd, CLONE_NEWNET));
    CHECK(dup2(started, 3) == 3);
    CHECK(!syscall(SYS_close_range, 4u, ~0u, 0u));
    struct pollfd servers[7];
    for (size_t i = 0; i < 7; i++)
        servers[i] = (struct pollfd){.fd = listener(ports[i], 1, NULL), .events = POLLIN};
    CHECK(write(3, "R", 1) == 1);
    close(3);
    unsigned count = 0;
    while (count < 7) {
        CHECK(poll(servers, 7, 2000) > 0);
        for (size_t i = 0; i < 7; i++)
            if (servers[i].revents & POLLIN) {
                int client = accept4(servers[i].fd, NULL, NULL, SOCK_CLOEXEC);
                CHECK(client >= 3);
                unsigned char role = (unsigned char)i;
                CHECK(write(client, &role, 1) == 1);
                close(client);
                close(servers[i].fd);
                servers[i].fd = -1;
                count++;
            }
    }
    _exit(0);
}
static void client(const uint16_t ports[7]) {
    CHECK(!prctl(PR_SET_PDEATHSIG, SIGKILL));
    alarm(10);
    CHECK(!syscall(SYS_close_range, 3u, ~0u, 0u));
    for (size_t i = 0; i < 7; i++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(fd >= 3);
        struct sockaddr_in value = address(ports[i], 0);
        CHECK(!connect(fd, (struct sockaddr *)&value, sizeof(value)));
        unsigned char role = 255;
        CHECK(read(fd, &role, 1) == 1 && role == i);
        close(fd);
    }
    _exit(0);
}
int main(void) {
    CHECK(geteuid() != 0);
    alarm(15);
    struct rlimit core = {0, 0}, file = {67108864, 67108864};
    CHECK(!setrlimit(RLIMIT_CORE, &core) && !setrlimit(RLIMIT_FSIZE, &file));
#ifndef GITSTORED_INSTRUMENTED
    struct rlimit memory = {536870912, 536870912};
    CHECK(!setrlimit(RLIMIT_AS, &memory));
#endif
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    CHECK(gstd_network_namespace_open(&network) == GSTD_NETWORK_OK);
    gstd_network_engine *engine = NULL;
    CHECK(gstd_network_open(network.tap_fd, &engine) == GSTD_NETWORK_OK);
    uint16_t host[7] = {0}, guest_ports[7] = {4000, 9000, 0};
    int reservations[5];
    CHECK(gstd_network_forward(engine, GSTD_FORWARD_API, &host[0]) == GSTD_NETWORK_OK);
    CHECK(gstd_network_forward(engine, GSTD_FORWARD_GIT, &host[1]) == GSTD_NETWORK_OK);
    for (size_t i = 0; i < 5; i++)
        reservations[i] = listener(0, 0, &guest_ports[i + 2]);
    gstd_oidc_ports ports = {guest_ports[2], guest_ports[3], guest_ports[4], guest_ports[5],
                             guest_ports[6]};
    CHECK(!gstd_oidc_ports_check(&ports));
    uint16_t rejected = 0;
    CHECK(gstd_network_forward_bound(engine, GSTD_FORWARD_API, 25000, &rejected) ==
          GSTD_NETWORK_INVALID);
    CHECK(gstd_network_forward_bound(engine, GSTD_FORWARD_OIDC_ISSUER, 4445, &rejected) ==
          GSTD_NETWORK_INVALID);
    CHECK(gstd_network_forward_bound(engine, GSTD_FORWARD_OIDC_IDENTITY, 4434, &rejected) ==
          GSTD_NETWORK_INVALID);
    CHECK(gstd_network_forward_bound(engine, GSTD_FORWARD_MAIL_UI, 1025, &rejected) ==
          GSTD_NETWORK_INVALID);
    CHECK(gstd_network_forward_bound(engine, GSTD_FORWARD_MAIL_UI, 1023, &rejected) ==
          GSTD_NETWORK_INVALID);
    for (size_t i = 0; i < 5; i++) {
        close(reservations[i]);
        gstd_forward_role role = (gstd_forward_role)(i + 2);
        CHECK(gstd_network_forward_bound(engine, role, guest_ports[i + 2], &host[i + 2]) ==
              GSTD_NETWORK_OK);
        CHECK(host[i + 2] == guest_ports[i + 2]);
        uint16_t again = 0;
        CHECK(gstd_network_forward_bound(engine, role, host[i + 2], &again) == GSTD_NETWORK_OK &&
              again == host[i + 2]);
        int fd = -1;
        uint64_t inode = 0;
        CHECK(!gstd_network_listener(engine, role, &fd, &inode) && fd >= 3 && inode);
        struct sockaddr_in actual;
        socklen_t length = sizeof(actual);
        CHECK(!getsockname(fd, (struct sockaddr *)&actual, &length));
        CHECK(actual.sin_family == AF_INET && actual.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
              ntohs(actual.sin_port) == host[i + 2]);
    }
    /* Establish all guest listeners before the client starts, without sleeps. */
    int started[2];
    CHECK(!pipe2(started, O_CLOEXEC));
    pid_t workload = fork();
    CHECK(workload >= 0);
    if (!workload) {
        close(started[0]);
        guest(&network, guest_ports, started[1]);
    }
    close(started[1]);
    char marker = 0;
    CHECK(read(started[0], &marker, 1) == 1 && marker == 'R');
    close(started[0]);
    pid_t requester = fork();
    CHECK(requester >= 0);
    if (!requester)
        client(host);
    int done_guest = 0, done_client = 0, status;
    while (!done_guest || !done_client) {
        CHECK(gstd_network_step(engine, -1, 20) == GSTD_NETWORK_OK);
        if (!done_guest && waitpid(workload, &status, WNOHANG) == workload) {
            CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
            done_guest = 1;
        }
        if (!done_client && waitpid(requester, &status, WNOHANG) == requester) {
            CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
            done_client = 1;
        }
    }
    CHECK(gstd_network_close(engine) == GSTD_NETWORK_OK);
    engine = NULL;
    CHECK(gstd_network_open(network.tap_fd, &engine) == GSTD_NETWORK_OK);
    for (size_t i = 2; i < 7; i++) {
        uint16_t rebound = 0;
        CHECK(gstd_network_forward_bound(engine, (gstd_forward_role)i, host[i], &rebound) ==
                  GSTD_NETWORK_OK &&
              rebound == host[i]);
    }
    CHECK(gstd_network_close(engine) == GSTD_NETWORK_OK);
    CHECK(gstd_network_namespace_close(&network) == GSTD_NETWORK_OK);
    for (size_t i = 0; i < 7; i++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(fd >= 3);
        struct sockaddr_in value = address(host[i], 0);
        CHECK(connect(fd, (struct sockaddr *)&value, sizeof(value)) != 0 && errno == ECONNREFUSED);
        close(fd);
    }
    puts("{\"forwards\":7,\"samePortOidcRoles\":5,\"loopbackOnly\":true,\"privatePortsRejected\":"
         "true,\"payloadRolesMatched\":true,\"listenersReleased\":true,\"stablePortsRebound\":"
         "true}");
    return 0;
}
