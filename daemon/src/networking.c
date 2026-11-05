#define _GNU_SOURCE 1
#include "networking.h"
#include <stdlib.h>
#if defined(__linux__) && defined(GITSTORED_SLIRP)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libslirp.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define POLL_BOUND 512
#define TIMER_BOUND 64
struct timer {
    SlirpTimerCb callback;
    void *argument;
    int64_t deadline;
    int active;
};
struct gstd_network_engine {
    Slirp *slirp;
    SlirpCb callbacks; /* libslirp borrows this table until slirp_cleanup */
    int tap, failed, count;
    uint16_t ports[GSTD_FORWARD_ROLE_COUNT];
    int listeners[GSTD_FORWARD_ROLE_COUNT];
    uint64_t guest_errors;
    struct timer timers[TIMER_BOUND];
    struct pollfd descriptors[POLL_BOUND];
};
static int64_t now_ns(void) {
    struct timespec value;
    return clock_gettime(CLOCK_MONOTONIC, &value)
               ? -1
               : (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static int64_t clock_ns(void *opaque) {
    gstd_network_engine *engine = opaque;
    int64_t value = now_ns();
    if (value < 0)
        engine->failed = 1;
    return value;
}
static ssize_t packet(const void *data, size_t length, void *opaque) {
    gstd_network_engine *engine = opaque;
    if (length > 65536) {
        engine->failed = 1;
        return -1;
    }
    ssize_t sent;
    do {
        sent = write(engine->tap, data, length);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return (ssize_t)length;
    if (sent != (ssize_t)length)
        engine->failed = 1;
    return sent;
}
static void guest_error(const char *message, void *opaque) {
    (void)message;
    /* The library rejects the offending packet. Keep only a bounded counter;
     * never log untrusted packet contents, names or library strings. */
    gstd_network_engine *engine = opaque;
    if (engine->guest_errors < UINT64_MAX)
        engine->guest_errors++;
}
static struct timer *valid_timer(gstd_network_engine *engine, void *pointer) {
    uintptr_t begin = (uintptr_t)&engine->timers[0], end = (uintptr_t)&engine->timers[TIMER_BOUND],
              value = (uintptr_t)pointer;
    if (value < begin || value >= end || (value - begin) % sizeof(struct timer)) {
        engine->failed = 1;
        return NULL;
    }
    struct timer *timer = pointer;
    if (!timer->active) {
        engine->failed = 1;
        return NULL;
    }
    return timer;
}
static void *timer_new(SlirpTimerCb callback, void *argument, void *opaque) {
    gstd_network_engine *engine = opaque;
    if (callback)
        for (size_t i = 0; i < TIMER_BOUND; i++)
            if (!engine->timers[i].active) {
                engine->timers[i] = (struct timer){callback, argument, -1, 1};
                return &engine->timers[i];
            }
    engine->failed = 1;
    return NULL;
}
static void timer_free(void *pointer, void *opaque) {
    struct timer *timer = valid_timer(opaque, pointer);
    if (timer)
        memset(timer, 0, sizeof(*timer));
}
static void timer_mod(void *pointer, int64_t expiry, void *opaque) {
    struct timer *timer = valid_timer(opaque, pointer);
    if (timer)
        timer->deadline = expiry;
}
static void fd_change(int fd, void *opaque) {
    (void)fd;
    (void)opaque;
}
static void notify(void *opaque) { (void)opaque; }
static int add_poll(int fd, int events, void *opaque) {
    gstd_network_engine *engine = opaque;
    if (engine->count >= POLL_BOUND) {
        engine->failed = 1;
        return -1;
    }
    short flags = 0;
    if (events & SLIRP_POLL_IN)
        flags |= POLLIN;
    if (events & SLIRP_POLL_OUT)
        flags |= POLLOUT;
    if (events & SLIRP_POLL_PRI)
        flags |= POLLPRI;
    engine->descriptors[engine->count] = (struct pollfd){fd, flags, 0};
    return engine->count++;
}
static int revents(int index, void *opaque) {
    gstd_network_engine *engine = opaque;
    if (index < 0 || index >= engine->count) {
        engine->failed = 1;
        return SLIRP_POLL_ERR;
    }
    int flags = engine->descriptors[index].revents, events = 0;
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
    if (flags & POLLNVAL) {
        engine->failed = 1;
        events |= SLIRP_POLL_ERR;
    }
    return events;
}
static int bounded(void) {
    struct rlimit value;
    if (!geteuid() || getrlimit(RLIMIT_CORE, &value) || value.rlim_cur)
        return 0;
#ifndef GITSTORED_INSTRUMENTED
    if (getrlimit(RLIMIT_AS, &value) || value.rlim_cur == RLIM_INFINITY ||
        value.rlim_cur > UINT64_C(512) * 1024 * 1024)
        return 0;
#endif
    return !getrlimit(RLIMIT_FSIZE, &value) && value.rlim_cur != RLIM_INFINITY &&
           value.rlim_cur <= UINT64_C(64) * 1024 * 1024;
}
#endif
int gstd_network_engine_available(void) {
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    return 1;
#else
    return 0;
#endif
}
gstd_network_status gstd_network_open(int tap, gstd_network_engine **output) {
    if (!output || *output || tap < 3)
        return GSTD_NETWORK_INVALID;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (!bounded())
        return GSTD_NETWORK_HOST_UNMET;
    struct ifreq info = {0};
    int flags = fcntl(tap, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDWR || !(flags & O_NONBLOCK) ||
        ioctl(tap, TUNGETIFF, &info) || strcmp(info.ifr_name, "gs0") ||
        !(info.ifr_flags & IFF_TAP) || !(info.ifr_flags & IFF_NO_PI) ||
        (info.ifr_flags & (IFF_TUN | IFF_PERSIST | IFF_MULTI_QUEUE | IFF_VNET_HDR)))
        return GSTD_NETWORK_INVALID;
    gstd_network_engine *engine = calloc(1, sizeof(*engine));
    if (!engine)
        return GSTD_NETWORK_FAILED;
    engine->tap = fcntl(tap, F_DUPFD_CLOEXEC, 3);
    if (engine->tap < 0) {
        free(engine);
        return GSTD_NETWORK_FAILED;
    }
    SlirpConfig config = {
        .version = 6, .in_enabled = true, .disable_host_loopback = true, .disable_dhcp = true};
    inet_pton(AF_INET, "10.0.2.0", &config.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &config.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &config.vhost);
    inet_pton(AF_INET, "10.0.2.15", &config.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3", &config.vnameserver);
    engine->callbacks = (SlirpCb){.send_packet = packet,
                                  .guest_error = guest_error,
                                  .clock_get_ns = clock_ns,
                                  .timer_new = timer_new,
                                  .timer_free = timer_free,
                                  .timer_mod = timer_mod,
                                  .register_poll_socket = fd_change,
                                  .unregister_poll_socket = fd_change,
                                  .notify = notify};
    engine->slirp = slirp_new(&config, &engine->callbacks, engine);
    if (!engine->slirp || engine->failed) {
        gstd_network_close(engine);
        return GSTD_NETWORK_FAILED;
    }
    *output = engine;
    return GSTD_NETWORK_OK;
#else
    return GSTD_NETWORK_UNAVAILABLE;
#endif
}
static gstd_network_status forward(gstd_network_engine *engine, gstd_forward_role role,
                                   uint16_t selected_port, uint16_t *port) {
    (void)selected_port;
    if (!engine || !port || role < GSTD_FORWARD_API || role >= GSTD_FORWARD_ROLE_COUNT)
        return GSTD_NETWORK_INVALID;
    *port = 0;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (engine->failed)
        return GSTD_NETWORK_FAILED;
    if (engine->ports[role]) {
        if (selected_port && selected_port != engine->ports[role])
            return GSTD_NETWORK_INVALID;
        *port = engine->ports[role];
        return GSTD_NETWORK_OK;
    }
    struct in_addr host, guest;
    inet_pton(AF_INET, "127.0.0.1", &host);
    inet_pton(AF_INET, "10.0.2.15", &guest);
    if (slirp_add_hostfwd(engine->slirp, 0, host, selected_port, guest,
                          role == GSTD_FORWARD_API   ? 4000
                          : role == GSTD_FORWARD_GIT ? 9000
                                                     : selected_port)) {
        engine->failed = 1;
        return GSTD_NETWORK_FAILED;
    }
    engine->count = 0;
    uint32_t timeout = 0;
    slirp_pollfds_fill_socket(engine->slirp, &timeout, add_poll, engine);
    unsigned found = 0;
    uint16_t candidate = 0;
    int listener = -1;
    for (int i = 0; i < engine->count; i++) {
        int accepting = 0;
        socklen_t length = sizeof(accepting);
        struct sockaddr_in address = {0};
        socklen_t size = sizeof(address);
        if (!getsockopt(engine->descriptors[i].fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting,
                        &length) &&
            accepting &&
            !getsockname(engine->descriptors[i].fd, (struct sockaddr *)&address, &size) &&
            size == sizeof(address) && address.sin_family == AF_INET &&
            address.sin_addr.s_addr == host.s_addr) {
            uint16_t value = ntohs(address.sin_port);
            int existing = 0;
            for (size_t n = 0; n < GSTD_FORWARD_ROLE_COUNT; n++)
                existing |= value == engine->ports[n];
            if (value >= 1024 && !existing && (!selected_port || value == selected_port)) {
                found++;
                candidate = value;
                listener = engine->descriptors[i].fd;
            }
        }
    }
    if (engine->failed || found != 1) {
        engine->failed = 1;
        return GSTD_NETWORK_FAILED;
    }
    engine->ports[role] = candidate;
    engine->listeners[role] = listener;
    *port = candidate;
    return GSTD_NETWORK_OK;
#else
    return GSTD_NETWORK_UNAVAILABLE;
#endif
}
gstd_network_status gstd_network_forward(gstd_network_engine *engine, gstd_forward_role role,
                                         uint16_t *port) {
    if (role != GSTD_FORWARD_API && role != GSTD_FORWARD_GIT)
        return GSTD_NETWORK_INVALID;
    return forward(engine, role, 0, port);
}
gstd_network_status gstd_network_forward_bound(gstd_network_engine *engine, gstd_forward_role role,
                                               uint16_t selected, uint16_t *port) {
    if (role < GSTD_FORWARD_OIDC_ISSUER || role >= GSTD_FORWARD_ROLE_COUNT || selected < 1024)
        return GSTD_NETWORK_INVALID;
    const uint16_t reserved[] = {1025, 4000, 9000, 6000, 50051, 5001, 4445, 4434};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(*reserved); i++)
        if (selected == reserved[i])
            return GSTD_NETWORK_INVALID;
    return forward(engine, role, selected, port);
}
int gstd_network_listener(gstd_network_engine *engine, gstd_forward_role role, int *descriptor,
                          uint64_t *inode) {
    if (!engine || !descriptor || !inode || role < GSTD_FORWARD_API ||
        role >= GSTD_FORWARD_ROLE_COUNT)
        return -1;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    struct stat info;
    if (engine->failed || !engine->ports[role] || engine->listeners[role] < 3 ||
        fstat(engine->listeners[role], &info) || !S_ISSOCK(info.st_mode))
        return -1;
    *descriptor = engine->listeners[role];
    *inode = (uint64_t)info.st_ino;
    return 0;
#else
    return -1;
#endif
}
gstd_network_status gstd_network_step(gstd_network_engine *engine, int control,
                                      unsigned maximum_ms) {
    if (!engine || (control != -1 && control < 3) || maximum_ms > 50)
        return GSTD_NETWORK_INVALID;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (engine->failed)
        return GSTD_NETWORK_FAILED;
    engine->count = 1;
    engine->descriptors[0] = (struct pollfd){engine->tap, POLLIN, 0};
    int control_index = -1;
    if (control >= 0) {
        control_index = engine->count;
        engine->descriptors[engine->count++] = (struct pollfd){control, POLLIN, 0};
    }
    uint32_t timeout = maximum_ms;
    slirp_pollfds_fill_socket(engine->slirp, &timeout, add_poll, engine);
    int64_t now = now_ns();
    if (now < 0)
        engine->failed = 1;
    for (size_t i = 0; i < TIMER_BOUND; i++)
        if (engine->timers[i].active && engine->timers[i].deadline >= 0) {
            int64_t delta = engine->timers[i].deadline - now / 1000000;
            if (delta <= (int64_t)timeout)
                timeout = delta > 0 ? (uint32_t)delta : 0;
        }
    if (engine->failed)
        return GSTD_NETWORK_FAILED;
    if (timeout > maximum_ms)
        timeout = maximum_ms;
    int ready = poll(engine->descriptors, (nfds_t)engine->count, (int)timeout);
    if (ready < 0 && errno != EINTR) {
        engine->failed = 1;
        return GSTD_NETWORK_FAILED;
    }
    if (control_index >= 0 && engine->descriptors[control_index].revents)
        return GSTD_NETWORK_CONTROL;
    slirp_pollfds_poll(engine->slirp, ready < 0, revents, engine);
    if (engine->descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        engine->failed = 1;
    if (engine->descriptors[0].revents & POLLIN) {
        unsigned char bytes[65536];
        ssize_t count = read(engine->tap, bytes, sizeof(bytes));
        if (count > 0)
            slirp_input(engine->slirp, bytes, (int)count);
        else if (!count || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            engine->failed = 1;
    }
    /* A callback can free/rearm any timer. Rescan after each invocation; never
     * retain another timer pointer across a library callback. */
    for (unsigned fired = 0; fired < TIMER_BOUND; fired++) {
        now = now_ns();
        if (now < 0) {
            engine->failed = 1;
            break;
        }
        size_t next = TIMER_BOUND;
        for (size_t i = 0; i < TIMER_BOUND; i++)
            if (engine->timers[i].active && engine->timers[i].deadline >= 0 &&
                engine->timers[i].deadline <= now / 1000000) {
                next = i;
                break;
            }
        if (next == TIMER_BOUND)
            break;
        SlirpTimerCb callback = engine->timers[next].callback;
        void *argument = engine->timers[next].argument;
        engine->timers[next].deadline = -1;
        callback(argument);
        if (engine->failed)
            break;
    }
    return engine->failed ? GSTD_NETWORK_FAILED : GSTD_NETWORK_OK;
#else
    return GSTD_NETWORK_UNAVAILABLE;
#endif
}
gstd_network_status gstd_network_close(gstd_network_engine *engine) {
    if (!engine)
        return GSTD_NETWORK_OK;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    struct in_addr host;
    inet_pton(AF_INET, "127.0.0.1", &host);
    if (engine->slirp) {
        for (size_t i = 0; i < GSTD_FORWARD_ROLE_COUNT; i++)
            if (engine->ports[i] && slirp_remove_hostfwd(engine->slirp, 0, host, engine->ports[i]))
                engine->failed = 1;
        slirp_cleanup(engine->slirp);
    }
    for (size_t i = 0; i < TIMER_BOUND; i++)
        if (engine->timers[i].active)
            engine->failed = 1;
    int failed = engine->failed;
    close(engine->tap);
    free(engine);
    return failed ? GSTD_NETWORK_FAILED : GSTD_NETWORK_OK;
#else
    return GSTD_NETWORK_UNAVAILABLE;
#endif
}
