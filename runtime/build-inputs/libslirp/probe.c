#define _GNU_SOURCE 1
#include "libslirp.h"
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Developer-only namespace/TAP adapter. All packet, TCP, UDP and DNS forwarding
 * semantics belong to libslirp, never a bundled network helper executable. */
struct timer {
    SlirpTimerCb callback;
    void *argument;
    int64_t deadline;
    struct timer *next;
};
struct network {
    int tap;
    struct timer *timers;
    struct pollfd poll[256];
    int count;
};
static const char *payload = "fixture";
static void check(bool value, const char *message) {
    if (!value) {
        perror(message);
        exit(1);
    }
}
static int64_t now_ns(void) {
    struct timespec t;
    check(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "clock");
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int64_t clock_ns(void *opaque) {
    (void)opaque;
    return now_ns();
}
static ssize_t packet(const void *data, size_t length, void *opaque) {
    struct network *n = opaque;
    ssize_t size = write(n->tap, data, length);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return (ssize_t)length;
    return size;
}
static void guest_error(const char *message, void *opaque) {
    (void)opaque;
    fprintf(stderr, "slirp fixture: %.160s\n", message);
}
static void *timer_new(SlirpTimerCb callback, void *argument, void *opaque) {
    struct network *n = opaque;
    struct timer *t = calloc(1, sizeof(*t));
    check(t != NULL, "timer allocation");
    *t = (struct timer){callback, argument, -1, n->timers};
    n->timers = t;
    return t;
}
static void timer_free(void *timer, void *opaque) {
    struct network *n = opaque;
    struct timer **p = &n->timers;
    while (*p && *p != timer)
        p = &(*p)->next;
    check(*p != NULL, "timer ownership");
    *p = (*p)->next;
    free(timer);
}
static void timer_mod(void *timer, int64_t expiry, void *opaque) {
    (void)opaque;
    ((struct timer *)timer)->deadline = expiry;
}
static void fd_change(int fd, void *opaque) {
    (void)fd;
    (void)opaque;
}
static void notify(void *opaque) { (void)opaque; }
static int add_poll(int fd, int events, void *opaque) {
    struct network *n = opaque;
    check(n->count < 256, "poll bound");
    short flags = 0;
    if (events & SLIRP_POLL_IN)
        flags |= POLLIN;
    if (events & SLIRP_POLL_OUT)
        flags |= POLLOUT;
    if (events & SLIRP_POLL_PRI)
        flags |= POLLPRI;
    n->poll[n->count] = (struct pollfd){fd, flags, 0};
    return n->count++;
}
static int revents(int index, void *opaque) {
    struct network *n = opaque;
    check(index >= 0 && index < n->count, "poll index");
    int flags = n->poll[index].revents, events = 0;
    if (flags & POLLIN)
        events |= SLIRP_POLL_IN;
    if (flags & POLLOUT)
        events |= SLIRP_POLL_OUT;
    if (flags & POLLPRI)
        events |= SLIRP_POLL_PRI;
    if (flags & POLLERR)
        events |= SLIRP_POLL_ERR;
    if (flags & POLLHUP)
        events |= SLIRP_POLL_HUP;
    return events;
}
static void write_mapping(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    check(fd >= 0, path);
    check(write(fd, value, strlen(value)) == (ssize_t)strlen(value), "mapping write");
    close(fd);
}
static struct sockaddr address(const char *value) {
    struct sockaddr_in a = {.sin_family = AF_INET};
    check(inet_pton(AF_INET, value, &a.sin_addr) == 1, "IPv4 literal");
    struct sockaddr result;
    memcpy(&result, &a, sizeof(result));
    return result;
}
static void pass_tap(int channel, int tap) {
    char value = 'T', control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {&value, 1};
    struct msghdr message = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &tap, sizeof(tap));
    check(sendmsg(channel, &message, MSG_NOSIGNAL) == 1, "send TAP");
}
static int receive_tap(int channel) {
    char value, control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {&value, 1};
    struct msghdr message = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
    struct pollfd ready = {channel, POLLIN, 0};
    check(poll(&ready, 1, 3000) > 0, "namespace deadline");
    check(recvmsg(channel, &message, MSG_CMSG_CLOEXEC) == 1 && value == 'T' &&
              !(message.msg_flags & MSG_CTRUNC),
          "receive TAP");
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    check(c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
              c->cmsg_len == CMSG_LEN(sizeof(int)),
          "TAP descriptor");
    int tap;
    memcpy(&tap, CMSG_DATA(c), sizeof(tap));
    return tap;
}
static void namespace_workload(int channel, uid_t uid, gid_t gid) {
    check(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0, "parent death signal");
    alarm(12);
    check(unshare(CLONE_NEWUSER) == 0, "rootless user namespace");
    char map[80];
    snprintf(map, sizeof(map), "0 %lu 1\n", (unsigned long)uid);
    write_mapping("/proc/self/uid_map", map);
    write_mapping("/proc/self/setgroups", "deny\n");
    snprintf(map, sizeof(map), "0 %lu 1\n", (unsigned long)gid);
    write_mapping("/proc/self/gid_map", map);
    check(unshare(CLONE_NEWNET) == 0, "private network namespace");
    int tap = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    check(tap >= 0, "rootless TUN access");
    struct ifreq request = {.ifr_flags = IFF_TAP | IFF_NO_PI};
    strcpy(request.ifr_name, "gsfixture0");
    check(ioctl(tap, TUNSETIFF, &request) == 0, "create owned TAP");
    int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    check(socket_fd >= 0, "interface socket");
    request.ifr_addr = address("10.0.2.15");
    check(ioctl(socket_fd, SIOCSIFADDR, &request) == 0, "interface address");
    request.ifr_netmask = address("255.255.255.0");
    check(ioctl(socket_fd, SIOCSIFNETMASK, &request) == 0, "interface mask");
    request.ifr_flags = IFF_UP;
    check(ioctl(socket_fd, SIOCSIFFLAGS, &request) == 0, "interface up");
    strcpy(request.ifr_name, "lo");
    request.ifr_flags = IFF_UP;
    check(ioctl(socket_fd, SIOCSIFFLAGS, &request) == 0, "loopback up");
    struct rtentry route = {.rt_dst = address("0.0.0.0"),
                            .rt_gateway = address("10.0.2.2"),
                            .rt_genmask = address("0.0.0.0"),
                            .rt_flags = RTF_UP | RTF_GATEWAY};
    check(ioctl(socket_fd, SIOCADDRT, &route) == 0, "namespace default route");
    close(socket_fd);
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(listener >= 0, "guest listener");
    struct sockaddr_in service = {.sin_family = AF_INET, .sin_port = htons(8080)};
    inet_pton(AF_INET, "10.0.2.15", &service.sin_addr);
    check(bind(listener, (struct sockaddr *)&service, sizeof(service)) == 0 &&
              listen(listener, 1) == 0,
          "private service bind");
    pass_tap(channel, tap);
    close(tap);
    close(channel);
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
    int channels[2];
    check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels) == 0, "private channel");
    pid_t workload = fork();
    check(workload >= 0, "namespace fork");
    if (!workload) {
        close(channels[0]);
        namespace_workload(channels[1], getuid(), getgid());
    }
    close(channels[1]);
    struct network network = {.tap = receive_tap(channels[0])};
    close(channels[0]);
    char path[80];
    snprintf(path, sizeof(path), "/proc/%ld/ns/net", (long)workload);
    struct stat theirs, ours;
    check(stat(path, &theirs) == 0 && stat("/proc/self/ns/net", &ours) == 0 &&
              theirs.st_ino != ours.st_ino,
          "network namespace isolation");
    SlirpConfig cfg = {
        .version = 6, .in_enabled = true, .disable_host_loopback = true, .disable_dhcp = true};
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);
    SlirpCb callbacks = {.send_packet = packet,
                         .guest_error = guest_error,
                         .clock_get_ns = clock_ns,
                         .timer_new = timer_new,
                         .timer_free = timer_free,
                         .timer_mod = timer_mod,
                         .register_poll_socket = fd_change,
                         .unregister_poll_socket = fd_change,
                         .notify = notify};
    Slirp *slirp = slirp_new(&cfg, &callbacks, &network);
    check(slirp != NULL, "slirp_new");
    struct in_addr host, guest;
    inet_pton(AF_INET, "127.0.0.1", &host);
    inet_pton(AF_INET, "10.0.2.15", &guest);
    check(slirp_add_hostfwd(slirp, 0, host, 0, guest, 8080) == 0, "loopback forwarding");
    uint32_t timeout = 10;
    slirp_pollfds_fill_socket(slirp, &timeout, add_poll, &network);
    struct sockaddr_in bound = {0};
    bool found = false;
    for (int i = 0; i < network.count; i++) {
        int accepting = 0;
        socklen_t size = sizeof(accepting), len = sizeof(bound);
        if (!getsockopt(network.poll[i].fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &size) &&
            accepting && !getsockname(network.poll[i].fd, (struct sockaddr *)&bound, &len)) {
            check(bound.sin_family == AF_INET && bound.sin_addr.s_addr == host.s_addr &&
                      ntohs(bound.sin_port) >= 1024,
                  "high loopback listener");
            found = true;
            break;
        }
    }
    check(found, "observe allocated port without bind-close race");
    pid_t requester = fork();
    check(requester >= 0, "request fork");
    if (!requester) {
        check(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0, "requester parent death signal");
        // Close inherited network descriptors before entering the test client.
        close(network.tap);
        for (int i = 0; i < network.count; i++)
            close(network.poll[i].fd);
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
        network.count = 1;
        network.poll[0] = (struct pollfd){network.tap, POLLIN, 0};
        timeout = 20;
        slirp_pollfds_fill_socket(slirp, &timeout, add_poll, &network);
        int result = poll(network.poll, (nfds_t)network.count, (int)timeout);
        check(result >= 0 || errno == EINTR, "poll");
        slirp_pollfds_poll(slirp, result < 0, revents, &network);
        if (network.poll[0].revents & POLLIN) {
            unsigned char data[65536];
            ssize_t count = read(network.tap, data, sizeof(data));
            if (count > 0)
                slirp_input(slirp, data, (int)count);
        }
        for (struct timer *t = network.timers, *next; t; t = next) {
            next = t->next;
            if (t->deadline >= 0 && t->deadline <= now_ns() / 1000000) {
                t->deadline = -1;
                t->callback(t->argument);
            }
        }
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
    check(slirp_remove_hostfwd(slirp, 0, host, ntohs(bound.sin_port)) == 0, "remove owned forward");
    slirp_cleanup(slirp);
    close(network.tap);
    check(network.timers == NULL, "timer cleanup");
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd >= 0, "post-cleanup socket");
    check(connect(fd, (struct sockaddr *)&bound, sizeof(bound)) != 0 && errno == ECONNREFUSED,
          "listener cleanup");
    close(fd);
    printf("libslirp %s: rootless TAP, private netns, DNS, high loopback forwarding and cleanup "
           "pass\n",
           slirp_version_string());
    return 0;
}
