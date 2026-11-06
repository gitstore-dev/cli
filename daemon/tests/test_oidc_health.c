#define _GNU_SOURCE 1
#include "bootstrap_http.h"
#include "gitstored/protocol.h"
#include "oidc_discovery.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC health line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
typedef struct {
    gstd_oidc_health_role role;
    uint16_t port;
    const char *path;
} target;
static int send_all(int fd, const char *bytes, size_t length) {
    while (length) {
        ssize_t sent = send(fd, bytes, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        bytes += sent;
        length -= (size_t)sent;
    }
    return 0;
}
/* Bind before fork: occupied ports are a fixture failure, never a false pass. */
static int serve(const target *role, int code, const char *body, int fault) {
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int reuse = 1;
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_port = htons(role->port),
                                  .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    if (listener < 0)
        return -1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 2)) {
        close(listener);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        close(listener);
        return -1;
    }
    if (!child) {
        alarm(5);
        int peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        char request[4096] = {0}, expected[128];
        if (peer < 0)
            _exit(1);
        size_t used = 0;
        while (!strstr(request, "\r\n\r\n")) {
            ssize_t n = read(peer, request + used, sizeof(request) - 1 - used);
            if (n <= 0)
                _exit(1);
            used += (size_t)n;
            if (used == sizeof(request) - 1)
                _exit(1);
        }
        snprintf(expected, sizeof(expected), "GET %s HTTP/1.1\r\n", role->path);
        if (strncmp(request, expected, strlen(expected)))
            _exit(1);
        if (fault == 1) { /* total HTTP deadline, without a worker timeout */
            struct timespec delay = {2, 200000000};
            nanosleep(&delay, NULL);
        } else {
            char headers[512];
            size_t length = fault == 2 ? GSTD_FRAME_MAX + 1 : strlen(body);
            int n = snprintf(headers, sizeof(headers),
                             "HTTP/1.1 %d Fixture\r\nContent-Type: application/json\r\n"
                             "Location: http://127.0.0.1:%u/redirect-trap\r\n"
                             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                             code, role->port, length);
            if (n <= 0 || (size_t)n >= sizeof(headers) || send_all(peer, headers, (size_t)n))
                _exit(1);
            if (fault == 2) {
                char opaque[4096];
                memset(opaque, 'x', sizeof(opaque));
                while (length) {
                    size_t chunk = length > sizeof(opaque) ? sizeof(opaque) : length;
                    if (send_all(peer, opaque, chunk))
                        break; /* consumer may reject before all bytes are sent */
                    length -= chunk;
                }
            } else if (fault != 3 && send_all(peer, body, length))
                _exit(1);
        }
        close(peer);
        /* Any redirected request would reach the same still-listening socket. */
        struct pollfd trap = {.fd = listener, .events = POLLIN};
        int redirected = code == 302 && poll(&trap, 1, 100) != 0;
        close(listener);
        _exit(redirected ? 1 : 0);
    }
    close(listener);
    return child;
}
static int issuer_case(const gstd_oidc_ports *ports, const gstd_bootstrap_http_context *context,
                       int mode) {
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), reuse = 1;
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_port = htons(ports->issuer),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(listener >= 3 && !setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) &&
          !bind(listener, (struct sockaddr *)&address, sizeof(address)) && !listen(listener, 2));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(5);
        unsigned requests = (mode == 0 || mode == 3 || mode == 4 || mode == 6) ? 2 : 1;
        for (unsigned i = 0; i < requests; i++) {
            int peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            char request[4096] = {0}, body[2048], header[512], expected[128], modulus[343];
            memset(modulus, 'x', 342);
            modulus[342] = 0;
            if (peer < 0 || read(peer, request, sizeof(request) - 1) <= 0)
                _exit(1);
            snprintf(expected, sizeof(expected), "GET /.well-known/%s HTTP/1.1\r\n",
                     i ? "jwks.json" : "openid-configuration");
            if (strncmp(request, expected, strlen(expected)))
                _exit(1);
            if (!i) {
                snprintf(body, sizeof(body),
                         "{\"issuer\":\"http://127.0.0.1:%u/\","
                         "\"jwks_uri\":\"http://%s:%u/.well-known/jwks.json\"}",
                         mode == 1 ? ports->bridge : ports->issuer,
                         mode == 2 ? "foreign.invalid" : "127.0.0.1", ports->issuer);
                if (mode == 8)
                    strcpy(body, "invalid json");
            } else if (mode == 3)
                strcpy(body, "{\"keys\":[]}");
            else
                snprintf(body, sizeof(body),
                         "{\"keys\":[{\"kid\":\"fixture\",\"kty\":\"RSA\","
                         "\"alg\":\"RS256\",\"use\":\"sig\",\"n\":\"%s\",\"e\":\"AQAB\"%s}]}",
                         modulus, mode == 4 ? ",\"d\":\"synthetic-private-field\"" : "");
            int code = (mode == 5 || (mode == 6 && i)) ? 302 : mode == 9 ? 503 : 200;
            int length = snprintf(
                header, sizeof(header),
                "HTTP/1.1 %d Fixture\r\n"
                "Content-Type: %s\r\nLocation: http://127.0.0.1:%u/redirect-trap\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                code, mode == 7 ? "text/plain" : "application/json", ports->issuer, strlen(body));
            if (length < 0 || send_all(peer, header, (size_t)length) ||
                send_all(peer, body, strlen(body)))
                _exit(1);
            close(peer);
        }
        struct pollfd trap = {.fd = listener, .events = POLLIN};
        int extra = poll(&trap, 1, 50);
        close(listener);
        _exit(extra ? 1 : 0);
    }
    close(listener);
    gstd_oidc_health_status actual = gstd_oidc_health(context, GSTD_OIDC_HEALTH_ISSUER, ports);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    gstd_oidc_health_status expected = !mode ? GSTD_OIDC_HEALTH_READY
                                       : mode == 5 || mode == 6 || mode == 9
                                           ? GSTD_OIDC_HEALTH_PENDING
                                           : GSTD_OIDC_HEALTH_REJECTED;
    CHECK(actual == expected);
    return 0;
}
static int check(const target *role, int code, const char *body, int fault,
                 gstd_oidc_health_status expected, const gstd_oidc_ports *ports,
                 const gstd_bootstrap_http_context *context) {
    int child = serve(role, code, body, fault);
    if (child < 0) {
        perror("owned OIDC health listener");
        return 1;
    }
    gstd_oidc_health_status status = gstd_oidc_health(context, role->role, ports);
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) || status != expected) {
        fprintf(stderr, "OIDC health role %d returned %d, expected %d for HTTP %d fault %d\n",
                (int)role->role, (int)status, (int)expected, code, fault);
        return 1;
    }
    return 0;
}
#endif
int main(void) {
#if !defined(__linux__) || !defined(GITSTORED_BOOTSTRAP)
    return 77;
#else
    alarm(20);
    struct rlimit core = {0, 0};
    CHECK(!setrlimit(RLIMIT_CORE, &core));
    gstd_json_initialize();
    int sockets[5];
    uint16_t selected[5];
    for (unsigned i = 0; i < 5; i++) {
        sockets[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in address = {.sin_family = AF_INET,
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t size = sizeof(address);
        CHECK(sockets[i] >= 3 && !bind(sockets[i], (struct sockaddr *)&address, sizeof(address)) &&
              !getsockname(sockets[i], (struct sockaddr *)&address, &size));
        selected[i] = ntohs(address.sin_port);
    }
    gstd_oidc_ports ports = {selected[0], selected[1], selected[2], selected[3], selected[4]};
    for (unsigned i = 0; i < 5; i++)
        close(sockets[i]);
    gstd_bootstrap_http_context context = {.control_owner = getppid(), .test_fixture = 1};
    CHECK(gstd_oidc_health(&context, GSTD_OIDC_HEALTH_HYDRA, NULL) == GSTD_OIDC_HEALTH_REJECTED);
    CHECK(gstd_oidc_health(&context, (gstd_oidc_health_role)99, &ports) ==
          GSTD_OIDC_HEALTH_REJECTED);
    CHECK(gstd_oidc_health(NULL, GSTD_OIDC_HEALTH_SMTP, &ports) == GSTD_OIDC_HEALTH_REJECTED);
    ports.mail = ports.ui;
    CHECK(gstd_oidc_health(&context, GSTD_OIDC_HEALTH_SMTP, &ports) == GSTD_OIDC_HEALTH_REJECTED);
    ports.mail = selected[4];
    gstd_bootstrap_http_context invalid = context;
    invalid.control_owner = getpid();
    CHECK(gstd_oidc_health(&invalid, GSTD_OIDC_HEALTH_SMTP, &ports) == GSTD_OIDC_HEALTH_REJECTED);
    CHECK(!setenv("http_proxy", "http://127.0.0.1:1", 1));
    CHECK(!setenv("ALL_PROXY", "http://127.0.0.1:1", 1));
    CHECK(!setenv("NO_PROXY", "", 1));
    const target roles[] = {{GSTD_OIDC_HEALTH_HYDRA, 4445, "/health/ready"},
                            {GSTD_OIDC_HEALTH_KRATOS, 4434, "/admin/health/ready"},
                            {GSTD_OIDC_HEALTH_BRIDGE, ports.bridge, "/health"},
                            {GSTD_OIDC_HEALTH_UI, ports.ui, "/health/ready"},
                            {GSTD_OIDC_HEALTH_SMTP, ports.mail, "/readyz"}};
    for (size_t i = 0; i < sizeof(roles) / sizeof(*roles); i++) {
        CHECK(!check(&roles[i], 200, i ? "opaque upstream bytes" : "{\"status\":\"ok\"}", 0,
                     GSTD_OIDC_HEALTH_READY, &ports, &context));
        CHECK(!check(&roles[i], 503, "fixture failure", 0, GSTD_OIDC_HEALTH_PENDING, &ports,
                     &context));
        CHECK(!check(&roles[i], 302, "", 0,
                     i ? GSTD_OIDC_HEALTH_PENDING : GSTD_OIDC_HEALTH_REJECTED, &ports, &context));
        CHECK(!check(&roles[i], 204, "", 0,
                     i ? GSTD_OIDC_HEALTH_PENDING : GSTD_OIDC_HEALTH_REJECTED, &ports, &context));
        CHECK(
            !check(&roles[i], 200, "partial body", 3, GSTD_OIDC_HEALTH_PENDING, &ports, &context));
        CHECK(!check(&roles[i], 200, "", 2,
                     i ? GSTD_OIDC_HEALTH_PENDING : GSTD_OIDC_HEALTH_REJECTED, &ports, &context));
    }
    CHECK(!check(&roles[0], 200, "{\"status\":\"pending\"}", 0, GSTD_OIDC_HEALTH_REJECTED, &ports,
                 &context));
    CHECK(!check(&roles[0], 200, "not json", 0, GSTD_OIDC_HEALTH_REJECTED, &ports, &context));
    CHECK(!check(&roles[1], 200, "", 1, GSTD_OIDC_HEALTH_PENDING, &ports, &context));
    for (int mode = 0; mode < 10; mode++)
        CHECK(!issuer_case(&ports, &context, mode));
    /* Listener is now closed: no service and no reply cannot satisfy readiness. */
    CHECK(gstd_oidc_health(&context, GSTD_OIDC_HEALTH_KRATOS, &ports) == GSTD_OIDC_HEALTH_PENDING);
    return 0;
#endif
}
