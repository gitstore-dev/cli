#define _GNU_SOURCE 1
#include <errno.h>
#include <grp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
int gstd_test_peer_allowed(int);
static int check(int foreign) {
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
        return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1, "gitstored-peer-%ld-%d",
             (long)getpid(), foreign);
    socklen_t size =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(address.sun_path + 1));
    if (bind(listener, (struct sockaddr *)&address, size) || listen(listener, 1)) {
        close(listener);
        return -1;
    }
    pid_t child = fork();
    if (!child) {
        close(listener);
        /* Disposable fixture only: set credentials BEFORE socket creation, so
         * SO_PEERCRED observes a real foreign UID rather than inherited creds. */
        if (foreign && (setgroups(0, NULL) || setgid(11001) || setuid(11001)))
            _exit(2);
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (client < 0 || connect(client, (struct sockaddr *)&address, size))
            _exit(3);
        char byte;
        if (read(client, &byte, 1) != 1)
            _exit(4);
        close(client);
        _exit(0);
    }
    if (child < 0) {
        close(listener);
        return -1;
    }
    int peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    close(listener);
    int valid = peer >= 0 && gstd_test_peer_allowed(peer) == !foreign;
    if (peer >= 0) {
        if (write(peer, "x", 1) != 1)
            valid = 0;
        close(peer);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
        valid = 0;
    return valid ? 0 : -1;
}
int main(void) {
    if (geteuid() != 0) {
        puts("SKIP: cross-UID peer test needs an explicitly selected disposable root fixture");
        return 77;
    }
    alarm(10);
    if (check(0) || check(1))
        return 1;
    puts("PASS: kernel credentials accept the owner and reject actual foreign UID 11001");
    return 0;
}
