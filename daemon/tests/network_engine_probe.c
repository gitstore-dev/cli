#define _GNU_SOURCE 1
#include "namespace_owner.h"
#include "networking.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <resolv.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Developer-only namespace/TAP adapter. All packet, TCP, UDP and DNS forwarding
 * semantics belong to libslirp, never a bundled network helper executable. */
static const char *payload = "fixture";
static void check(bool value, const char *message) {
    if (!value) {
        perror(message);
        exit(1);
    }
}
static int64_t now_ns(void) {
    struct timespec value;
    check(!clock_gettime(CLOCK_MONOTONIC, &value), "clock");
    return (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static void namespace_workload(const gstd_network_namespace *owner) {
    check(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0, "guest parent death");
    alarm(12);
    check(setns(owner->user_fd, CLONE_NEWUSER) == 0 && setns(owner->network_fd, CLONE_NEWNET) == 0,
          "join owned namespaces");
    check(syscall(SYS_close_range, 3u, ~0u, 0u) == 0, "guest drops unrelated descriptors");
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(listener >= 0, "guest listener");
    struct sockaddr_in service = {.sin_family = AF_INET, .sin_port = htons(4000)};
    inet_pton(AF_INET, "10.0.2.15", &service.sin_addr);
    check(bind(listener, (struct sockaddr *)&service, sizeof(service)) == 0 &&
              listen(listener, 1) == 0,
          "private service bind");
    // Use libc's maintained DNS resolver, explicitly through libslirp's DNS IP.
    struct __res_state resolver = {0};
    check(res_ninit(&resolver) == 0, "resolver init");
    resolver.nscount = 1;
    resolver.retrans = 2;
    resolver.retry = 1;
    resolver.nsaddr_list[0] = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(53)};
    inet_pton(AF_INET, "10.0.2.3", &resolver.nsaddr_list[0].sin_addr);
    unsigned char answer[4096];
    int result = res_nquery(&resolver, "example.org", ns_c_in, ns_t_a, answer, sizeof(answer));
    res_nclose(&resolver);
    check(result > 0, "DNS through libslirp");
    int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    check(client >= 0, "forwarded connection");
    check(write(client, payload, 7) == 7, "fixture response");
    close(client);
    close(listener);
    _exit(0);
}
int main(int argc, char **argv) {
    check(argc == 1 || (argc == 2 && strlen(argv[1]) == 7), "seven-byte fixture identity");
    if (argc == 2)
        payload = argv[1];
    check(getuid() != 0, "run experiment as ordinary fixture user");
    alarm(15);
    struct rlimit core = {0, 0}, file = {67108864, 67108864};
    check(!setrlimit(RLIMIT_CORE, &core) && !setrlimit(RLIMIT_FSIZE, &file), "worker limits");
#ifndef GITSTORED_INSTRUMENTED
    struct rlimit address_space = {536870912, 536870912};
    check(!setrlimit(RLIMIT_AS, &address_space), "worker address limit");
#endif
    gstd_network_namespace owner = GSTD_NETWORK_NAMESPACE_INITIAL;
    unsigned namespace_uid = 0;
    const char *selected_uid = getenv("GITSTORED_TEST_NAMESPACE_UID");
    check(!selected_uid || !strcmp(selected_uid, "10000"), "registered fixture UID selection");
    if (selected_uid)
        namespace_uid = 10000;
    check(gstd_network_namespace_open_user(&owner, 10001) == GSTD_NETWORK_INVALID &&
              !owner.keeper_pid,
          "unregistered mapping refused before effects");
    check(gstd_network_namespace_open_user(&owner, namespace_uid) == GSTD_NETWORK_OK,
          "owned namespace create");
    int tap = owner.tap_fd;
    check(!gstd_network_namespace_check(&owner), "owned namespace identity");
    gstd_network_namespace wrong = owner;
    wrong.namespace_uid = namespace_uid ? 0 : 10000;
    check(gstd_network_namespace_check(&wrong) == -1, "wrong mapping receipt refused");
    wrong = owner;
    wrong.keeper_start++;
    check(gstd_network_namespace_check(&wrong) == -1, "stale keeper identity refused");
    wrong = owner;
    wrong.network_inode++;
    check(gstd_network_namespace_check(&wrong) == -1, "foreign namespace receipt refused");
    wrong = owner;
    wrong.user_fd = owner.tap_fd;
    check(gstd_network_namespace_check(&wrong) == -1, "non-namespace descriptor refused");
    pid_t workload = fork();
    check(workload >= 0, "guest fork");
    if (!workload)
        namespace_workload(&owner);
    gstd_network_engine *engine = NULL;
    check(gstd_network_open(tap, &engine) == GSTD_NETWORK_OK, "engine open");
    uint16_t port = 0, again = 0, git_port = 0;
    check(gstd_network_forward(engine, GSTD_FORWARD_API, &port) == GSTD_NETWORK_OK && port >= 1024,
          "API loopback forward");
    check(gstd_network_forward(engine, GSTD_FORWARD_API, &again) == GSTD_NETWORK_OK &&
              again == port,
          "idempotent role");
    check(gstd_network_forward(engine, 99, &again) == GSTD_NETWORK_INVALID, "unregistered role");
    check(gstd_network_forward(engine, GSTD_FORWARD_GIT, &git_port) == GSTD_NETWORK_OK &&
              git_port >= 1024 && git_port != port,
          "separate fixed Git role");
    int control[2];
    check(!pipe2(control, O_CLOEXEC), "control pipe");
    check(write(control[1], "x", 1) == 1, "control marker");
    check(gstd_network_step(engine, control[0], 0) == GSTD_NETWORK_CONTROL, "independent control");
    char marker;
    check(read(control[0], &marker, 1) == 1 && marker == 'x', "unconsumed control");
    close(control[0]);
    close(control[1]);
    struct sockaddr_in bound = {.sin_family = AF_INET, .sin_port = htons(port)};
    check(inet_pton(AF_INET, "127.0.0.1", &bound.sin_addr) == 1, "loopback literal");
    pid_t requester = fork();
    check(requester >= 0, "request fork");
    if (!requester) {
        check(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0, "requester parent death signal");
        // Close inherited network descriptors before entering the test client.
        check(syscall(SYS_close_range, 3u, ~0u, 0u) == 0, "requester drops inherited descriptors");
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(fd >= 0, "request socket");
        check(connect(fd, (struct sockaddr *)&bound, sizeof(bound)) == 0, "forward connect");
        char bytes[7];
        size_t count = 0;
        while (count < sizeof(bytes)) {
            ssize_t n = read(fd, bytes + count, sizeof(bytes) - count);
            check(n > 0, "forward reply");
            count += (size_t)n;
        }
        check(!memcmp(bytes, payload, 7), "forwarded payload identity");
        close(fd);
        _exit(0);
    }
    bool child_done = false, requester_done = false;
    int status;
    int64_t deadline = now_ns() + 10000000000LL;
    while (!(child_done && requester_done) && now_ns() < deadline) {
        check(gstd_network_step(engine, -1, 20) == GSTD_NETWORK_OK, "engine step");
        if (!child_done && waitpid(workload, &status, WNOHANG) == workload) {
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "namespace workload result");
            child_done = true;
        }
        if (!requester_done && waitpid(requester, &status, WNOHANG) == requester) {
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "requester result");
            requester_done = true;
        }
    }
    check(child_done && requester_done, "network experiment deadline");
    check(gstd_network_close(engine) == GSTD_NETWORK_OK, "engine paired cleanup");
    check(fcntl(tap, F_GETFD) >= 0, "borrowed TAP preserved");
    check(gstd_network_namespace_close(&owner) == GSTD_NETWORK_OK,
          "owned namespace/keeper cleanup");
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd >= 0, "post-cleanup socket");
    check(connect(fd, (struct sockaddr *)&bound, sizeof(bound)) != 0 && errno == ECONNREFUSED,
          "listener cleanup");
    close(fd);
    bound.sin_port = htons(git_port);
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd >= 0 && connect(fd, (struct sockaddr *)&bound, sizeof(bound)) != 0 &&
              errno == ECONNREFUSED,
          "Git forward cleanup");
    close(fd);
    puts("daemon network engine: owned TAP, DNS, fixed loopback role, independent control and "
         "cleanup pass");
    return 0;
}
