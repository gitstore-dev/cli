#define _GNU_SOURCE 1
#include "oidc_async_probe.h"
#include "bootstrap_job.h"
#include "namespace_owner.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int last_status;
static int observe(gstd_bootstrap_job_input *input, int expected) {
    gstd_bootstrap_job job = {0};
    if (gstd_bootstrap_job_launch(input, 7000, &job))
        return -1;
    gstd_bootstrap_job_reply reply = {.status = -99};
    int done = 0;
    while (!(done = gstd_bootstrap_job_poll(&job, -1, &reply)))
        usleep(1000);
    last_status = reply.status;
    return done == 1 && reply.status == expected && !reply.uid[0] ? 0 : -1;
}
static int soft_limit(int resource, rlim_t maximum) {
    struct rlimit bound;
    if (getrlimit(resource, &bound))
        return -1;
    if (bound.rlim_cur > maximum)
        bound.rlim_cur = maximum;
    return setrlimit(resource, &bound);
}
int gstd_test_oidc_async(const char *path, int root, int configuration, int broker,
                         const json_t *source, const json_t *manifest, const char *postman,
                         const char *mock) {
    int result = -1, log = -1;
    pid_t server = -1;
    struct stat executable, directory;
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    int stage = 1;
    if (!path || !postman || !mock || postman[0] != '/' || mock[0] != '/' ||
        lstat(postman, &executable) || !S_ISREG(executable.st_mode) ||
        (executable.st_uid != geteuid() && executable.st_uid) || (executable.st_mode & 0022) ||
        !(executable.st_mode & 0111) || lstat(mock, &directory) || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != geteuid() || (directory.st_mode & 0022) ||
        mkdirat(root, "async-postman-home", 0700))
        goto done;
    stage = 2;
    if (soft_limit(RLIMIT_CORE, 0) || soft_limit(RLIMIT_FSIZE, 67108864))
        goto done;
#ifndef GITSTORED_INSTRUMENTED
    if (soft_limit(RLIMIT_AS, 536870912))
        goto done;
#endif
    /* The mock can echo synthetic client secrets. Suppress its raw console
     * output entirely; only typed results from the application worker return. */
    log = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (log < 3 || gstd_network_namespace_open_user(&network, 10000) != GSTD_NETWORK_OK)
        goto done;
    char home[4096], xdg[4096];
    int n = snprintf(home, sizeof(home), "HOME=%s/async-postman-home", path);
    if (n < 1 || n >= (int)sizeof(home))
        goto done;
    n = snprintf(xdg, sizeof(xdg), "XDG_CONFIG_HOME=%s/async-postman-home", path);
    if (n < 1 || n >= (int)sizeof(xdg))
        goto done;
    pid_t parent = getpid();
    server = fork();
    if (server < 0)
        goto done;
    if (!server) {
        struct rlimit core = {0, 0}, files = {67108864, 67108864};
        struct rlimit memory = {UINT64_C(1) << 40, UINT64_C(1) << 40};
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || setpgid(0, 0) ||
            prctl(PR_SET_DUMPABLE, 0) || setrlimit(RLIMIT_CORE, &core) ||
            setrlimit(RLIMIT_FSIZE, &files) || setrlimit(RLIMIT_AS, &memory) ||
            setns(network.user_fd, CLONE_NEWUSER) || setns(network.network_fd, CLONE_NEWNET))
            _exit(20);
        struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
        struct __user_cap_data_struct caps[2] = {{0}};
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (syscall(SYS_capset, &header, caps) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
            null < 3 || dup2(null, STDIN_FILENO) != STDIN_FILENO ||
            dup2(log, STDOUT_FILENO) != STDOUT_FILENO ||
            dup2(log, STDERR_FILENO) != STDERR_FILENO || syscall(SYS_close_range, 3U, ~0U, 0))
            _exit(21);
        alarm(90);
        char *args[] = {(char *)postman, "mock",     "run",    (char *)mock,   "--port",
                        "4445",          "--output", "ndjson", "--no-history", NULL};
        char *env[] = {home, xdg, "PATH=/usr/bin:/bin", "LANG=C", "NO_COLOR=1", NULL};
        execve(postman, args, env);
        _exit(22);
    }
    close(log);
    log = -1;
    gstd_bootstrap_job_input input = {
        .preparation = {.manifest = manifest,
                        .store_id = "0123456789abcdef0123456789abcdef",
                        .deployment_id = "oidc-material-fixture",
                        .operation_id = "first",
                        .generation = 1,
                        .control_owner = parent,
                        .scope_fd = configuration},
        .kind = GSTD_BOOT_JOB_OIDC_HTTP,
        .broker_fd = broker,
        .source = source,
        .network = &network,
        .oidc_action = GSTD_OIDC_HTTP_READY,
        .oidc_ports = {24001, 24002, 24003, 24004, 24005},
        .oidc_client = {"gitstore-oidc-fixture-async", "gitstore",
                        "http://127.0.0.1:3000/callback"}};
    int ready = 0;
    stage = 3;
    for (unsigned attempt = 0; attempt < 100 && !ready; attempt++) {
        int status;
        if (waitpid(server, &status, WNOHANG) == server) {
            server = -1;
            goto done;
        }
        ready = !observe(&input, GSTD_OIDC_PROVIDER_READY);
        if (!ready)
            usleep(250000);
    }
    if (!ready)
        goto done;
    input.oidc_action = GSTD_OIDC_HTTP_CREATE;
    stage = 4;
    if (observe(&input, GSTD_OIDC_CLIENT_CREATED))
        goto done;
    input.oidc_action = GSTD_OIDC_HTTP_INSPECT;
    stage = 5;
    if (observe(&input, GSTD_OIDC_PUBLIC_MATCH))
        goto done;
    input.oidc_client.client_id = "gitstore-oidc-fixture-async-disconnect";
    input.oidc_action = GSTD_OIDC_HTTP_CREATE;
    stage = 6;
    if (observe(&input, GSTD_OIDC_UNCERTAIN))
        goto done;
    /* A public match verifies that the mock accepted the lost-reply write. It
     * remains distinct from acknowledged creation; no second POST is sent. */
    input.oidc_action = GSTD_OIDC_HTTP_INSPECT;
    stage = 7;
    if (observe(&input, GSTD_OIDC_PUBLIC_MATCH))
        goto done;
    result = 0;
done:
    if (result)
        fprintf(stderr, "OIDC async fixture stage %d status %d\n", stage, last_status);
    if (log >= 3)
        close(log);
    if (server > 0) {
        /* This fixture created the process group. Never address a borrowed PID. */
        if (kill(-server, SIGTERM) && errno != ESRCH)
            result = -1;
        int status = 0, waited = 0;
        for (unsigned i = 0; i < 3000 && !waited; i++) {
            waited = waitpid(server, &status, WNOHANG);
            if (!waited)
                usleep(1000);
        }
        if (!waited) {
            if (kill(-server, SIGKILL) && errno != ESRCH)
                result = -1;
            if (waitpid(server, &status, 0) != server)
                result = -1;
        } else if (waited != server)
            result = -1;
    }
    if (network.keeper_pid > 0 && gstd_network_namespace_close(&network) != GSTD_NETWORK_OK)
        result = -1;
    return result;
}
