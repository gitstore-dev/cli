#define _GNU_SOURCE 1
#include "bootstrap_http.h"
#include "gitstored/protocol.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
/* Explicit controlled fixture only; never built into a production command. */
static int run(pid_t parent, gstd_oidc_http_action action, int fd) {
    alarm(8);
    struct rlimit core = {0, 0};
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || setrlimit(RLIMIT_CORE, &core))
        return 1;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0600 || info.st_nlink != 1 || info.st_size <= 0 ||
        info.st_size > 4096)
        return 1;
    gstd_credential input = {.bytes = malloc((size_t)info.st_size), .length = (size_t)info.st_size};
    if (!input.bytes || pread(fd, input.bytes, input.length, 0) != (ssize_t)input.length)
        return 1;
    close(fd);
    json_t *value = gstd_json_parse(input.bytes, input.length);
    gstd_oidc_client client = {json_string_value(json_object_get(value, "clientId")),
                               json_string_value(json_object_get(value, "audience")),
                               json_string_value(json_object_get(value, "redirectUri"))};
    json_t *material = json_object_get(value, "clientSecret");
    gstd_credential secret = {.bytes = (unsigned char *)json_string_value(material),
                              .length = json_string_length(material)};
    gstd_bootstrap_http_context context = {.control_owner = parent, .test_fixture = 1};
    gstd_oidc_http_status status =
        gstd_oidc_http(&context, action, action == GSTD_OIDC_HTTP_READY ? NULL : &client,
                       action == GSTD_OIDC_HTTP_CREATE ? &secret : NULL);
    json_decref(value);
    gstd_credential_clear(&input);
    /* No client secret, registration token, response or input is returned. */
    printf("{\"status\":%d}\n", (int)status);
    fflush(stdout);
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 3 || !geteuid())
        return 2;
    gstd_json_initialize();
    gstd_oidc_http_action action;
    if (!strcmp(argv[1], "ready"))
        action = GSTD_OIDC_HTTP_READY;
    else if (!strcmp(argv[1], "create"))
        action = GSTD_OIDC_HTTP_CREATE;
    else if (!strcmp(argv[1], "inspect"))
        action = GSTD_OIDC_HTTP_INSPECT;
    else
        return 2;
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 3)
        return 2;
    pid_t parent = getpid(), child = fork();
    if (!child)
        _exit(run(parent, action, fd));
    close(fd);
    if (child < 0)
        return 1;
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : 1;
}
