#define _GNU_SOURCE 1
/* Explicit developer-only raw libcrun probe. The owned config is produced by the
 * saved Scylla runner, never accepted by a production daemon or bundled tool. */
#include "bootstrap.h"
#include "bootstrap_http.h"
#include "execution_plan.h"
#include "gitstored/assets.h"
#include "gitstored/execution.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "helper.h"
#include "namespace_owner.h"
#include "native_abi.h"
#include "native_exit.h"
#include "native_job.h"
#include "native_population.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int scylla_probe_create(const gstd_crun_request *, const char *, int *);
static const gstd_native_context *registered_context;
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void pause_ms(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static gstd_native_status production_apply(const gstd_native_context *context,
                                           gstd_native_action action,
                                           gstd_native_observation *observed) {
    gstd_native_job job = {0};
    if (gstd_native_launch(context, action, 10000, &job))
        return GSTD_NATIVE_FAILED;
    for (;;) {
        gstd_native_status status;
        int ready = gstd_native_poll(&job, -1, &status, observed);
        if (ready)
            return ready == 1 ? status : GSTD_NATIVE_FAILED;
        pause_ms();
    }
}
static int private(const char *path, int dir) {
    struct stat s;
    return !lstat(path, &s) && s.st_uid == geteuid() &&
           (s.st_mode & 07777) == (dir ? 0700 : 0600) &&
           (dir ? S_ISDIR(s.st_mode) : S_ISREG(s.st_mode) && s.st_nlink == 1);
}
static int apply(const gstd_crun_request *request, const char *bytes, int action, int output) {
    if (registered_context) {
        gstd_native_action selected = action == 0   ? GSTD_NATIVE_CREATE
                                      : action == 1 ? GSTD_NATIVE_START
                                                    : GSTD_NATIVE_DELETE;
        gstd_native_observation observed = {0};
        if (!action && registered_context->writer_lease_fd >= 3) {
            gstd_native_context denied = *registered_context;
            denied.writer_lease_fd = -1;
            if (production_apply(&denied, selected, &observed) != GSTD_NATIVE_INVALID ||
                observed.effect_possible)
                return -1;
        }
        return production_apply(registered_context, selected, &observed) == GSTD_NATIVE_OK ? 0 : -1;
    }
    pid_t worker = fork();
    if (worker < 0)
        return -1;
    if (!worker) {
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || null < 0 || dup2(null, 0) < 0 ||
            dup2(output, 1) < 0 || dup2(output, 2) < 0)
            _exit(126);
        if (null >= 3)
            close(null);
        int number = 0, result = -1;
        if (action == 0)
            result = scylla_probe_create(request, bytes, &number);
        else if (action == 1)
            result = gstd_crun_start(request, &number);
        else if (action == 2)
            result = gstd_crun_delete(request, &number);
        if (result < 0)
            dprintf(2, "native action=%d errno=%d result=%d\n", action, number, result);
        _exit(result >= 0 ? 0 : 1);
    }
    int64_t deadline = now_ms() + 10000;
    int outcome = 0;
    for (;;) {
        pid_t got = waitpid(worker, &outcome, WNOHANG);
        if (got == worker)
            return WIFEXITED(outcome) && !WEXITSTATUS(outcome) ? 0 : -1;
        if (got < 0 && errno != EINTR)
            return -1;
        if (now_ms() >= deadline) {
            /* Unreaped direct child identity, never a caller-selected PID. */
            (void)kill(worker, SIGKILL);
            (void)waitpid(worker, &outcome, 0);
            return -1;
        }
        pause_ms();
    }
}
static json_t *load_json(const char *path) {
    if (!private(path, 0))
        return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    char bytes[65537];
    ssize_t n = fd < 0 ? -1 : read(fd, bytes, sizeof(bytes));
    if (fd >= 0)
        close(fd);
    return n > 0 && n <= 65536 ? gstd_json_parse((unsigned char *)bytes, (size_t)n) : NULL;
}
static int namespace_record(const char *root, const gstd_network_namespace *n) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/namespace.json", root);
    json_t *v = json_pack("{s:i,s:I,s:I,s:I,s:I,s:I,s:s}", "pid", (int)n->keeper_pid, "start",
                          (json_int_t)n->keeper_start, "userDevice", (json_int_t)n->user_device,
                          "userInode", (json_int_t)n->user_inode, "networkDevice",
                          (json_int_t)n->network_device, "networkInode",
                          (json_int_t)n->network_inode, "boot", n->boot_id);
    char *bytes = v ? gstd_json_dump(v) : NULL;
    json_decref(v);
    int fd = bytes ? open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    int ok = fd >= 3 && write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes) && !fsync(fd);
    if (fd >= 0)
        close(fd);
    free(bytes);
    return ok ? 0 : -1;
}
static int namespace_borrow(const char *root, gstd_network_namespace *n) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/shared-namespace.json", root);
    json_t *v = load_json(path);
    const char *boot = json_string_value(json_object_get(v, "boot"));
    if (!v || json_object_size(v) != 7 || !boot || strlen(boot) != 36) {
        json_decref(v);
        return -1;
    }
    const char *names[] = {"pid",       "start",         "userDevice",
                           "userInode", "networkDevice", "networkInode"};
    for (unsigned i = 0; i < 6; i++)
        if (!json_is_integer(json_object_get(v, names[i])) ||
            json_integer_value(json_object_get(v, names[i])) <= 0) {
            json_decref(v);
            return -1;
        }
    json_int_t pid = json_integer_value(json_object_get(v, "pid"));
    if (pid > 2147483647) {
        json_decref(v);
        return -1;
    }
    n->keeper_pid = (pid_t)pid;
    n->keeper_start = (uint64_t)json_integer_value(json_object_get(v, "start"));
    n->user_device = (uint64_t)json_integer_value(json_object_get(v, "userDevice"));
    n->user_inode = (uint64_t)json_integer_value(json_object_get(v, "userInode"));
    n->network_device = (uint64_t)json_integer_value(json_object_get(v, "networkDevice"));
    n->network_inode = (uint64_t)json_integer_value(json_object_get(v, "networkInode"));
    memcpy(n->boot_id, boot, 37);
    json_decref(v);
    snprintf(path, sizeof(path), "/proc/%ld/ns/user", (long)n->keeper_pid);
    n->user_fd = open(path, O_RDONLY | O_CLOEXEC);
    snprintf(path, sizeof(path), "/proc/%ld/ns/net", (long)n->keeper_pid);
    n->network_fd = open(path, O_RDONLY | O_CLOEXEC);
    return gstd_network_namespace_check(n);
}
static int arguments(const json_t *args, const char *mode) {
    const char *command = json_string_value(json_array_get(args, 0));
    if (!command)
        return -1;
    if (!strcmp(mode, "api-check"))
        return json_array_size(args) == 1 && !strcmp(command, "/app/api") ? 0 : -1;
    if (!strcmp(mode, "git-server"))
        return json_array_size(args) == 1 && !strcmp(command, "/app/git-service") ? 0 : -1;
    if (!strcmp(mode, "version")) {
        const char *flag = json_string_value(json_array_get(args, 1));
        return json_array_size(args) == 2 && flag &&
                       (!strcmp(flag, "--version") || !strcmp(flag, "--help") ||
                        !strcmp(flag, "--help-seastar")) &&
                       (!strcmp(command, "/usr/bin/scylla") || !strcmp(command, "/usr/bin/cqlsh"))
                   ? 0
                   : -1;
    }
    static const char *server[] = {"/usr/bin/scylla",
                                   "--developer-mode=1",
                                   "--options-file=/etc/scylla/scylla.yaml",
                                   "--overprovisioned",
                                   "--smp=1",
                                   "--memory=1536M",
                                   "--listen-address=127.0.0.1",
                                   "--rpc-address=127.0.0.1",
                                   "--broadcast-address=127.0.0.1",
                                   "--broadcast-rpc-address=127.0.0.1",
                                   "--workdir=/var/lib/scylla",
                                   "--api-address=127.0.0.1",
                                   "--log-to-stdout=1",
                                   "--log-to-syslog=0"};
    if (!strcmp(mode, "server")) {
        if (json_array_size(args) != sizeof(server) / sizeof(*server))
            return -1;
        for (unsigned i = 0; i < sizeof(server) / sizeof(*server); i++) {
            const char *part = json_string_value(json_array_get(args, i));
            if (!part || strcmp(part, server[i]))
                return -1;
        }
        return 0;
    }
    if (!strcmp(mode, "migration") || !strcmp(mode, "migration-cancel")) {
        const char *expected[] = {"/app/gitctl",
                                  "migrate",
                                  "--hosts=127.0.0.1:9042",
                                  "--keyspace=gitstore",
                                  "--disable-shard-aware-port",
                                  "--timeout=60s"};
        if (json_array_size(args) != sizeof(expected) / sizeof(*expected))
            return -1;
        for (unsigned i = 0; i < sizeof(expected) / sizeof(*expected); i++) {
            const char *part = json_string_value(json_array_get(args, i));
            if (!part || strcmp(part, expected[i]))
                return -1;
        }
        return 0;
    }
    const char *statement =
        !strcmp(mode, "readiness") ? "SELECT release_version FROM system.local;"
        : !strcmp(mode, "keyspace")
            ? "CREATE KEYSPACE IF NOT EXISTS gitstore WITH replication = {'class': "
              "'NetworkTopologyStrategy', 'replication_factor': '1'} AND durable_writes = true;"
        : !strcmp(mode, "ledger")
            ? "SELECT JSON name, checksum, done, end_time FROM gitstore.gocqlx_migrate;"
        : !strcmp(mode, "lock-hold")
            ? "INSERT INTO gitstore.schema_migrations_lock (lock_key,holder,acquired_at) VALUES "
              "('migration','gitstore-native-fixture',toTimestamp(now())) IF NOT EXISTS USING TTL "
              "120;"
        : !strcmp(mode, "lock-release")
            ? "DELETE FROM gitstore.schema_migrations_lock WHERE lock_key='migration' IF "
              "holder='gitstore-native-fixture';"
            : NULL;
    if (!statement)
        return -1;
    const char *expected[] = {"/usr/bin/cqlsh",
                              "127.0.0.1",
                              "9042",
                              "--connect-timeout",
                              "5",
                              "--request-timeout",
                              "10",
                              "-e",
                              statement};
    if (json_array_size(args) != 9)
        return -1;
    for (unsigned i = 0; i < 9; i++) {
        const char *part = json_string_value(json_array_get(args, i));
        if (!part || strcmp(part, expected[i]))
            return -1;
    }
    return 0;
}
static int prepare_bootstrap(const char *fixture, const json_t *manifest) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/bootstrap", fixture);
    if (n < 0 || n >= (int)sizeof(path) || mkdir(path, 0700))
        return -1;
    int scope = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    gstd_bootstrap_request request = {.manifest = manifest,
                                      .store_id = "0123456789abcdef0123456789abcdef",
                                      .deployment_id = "scylla-api-fixture",
                                      .operation_id = "scylla-api-check",
                                      .datastore = "scylladb",
                                      .generation = 1,
                                      .control_owner = getppid(),
                                      .scope_fd = scope};
    gstd_credential material = {0};
    int ok = scope >= 3 && !gstd_bootstrap_material_generate(getppid(), &material) &&
             !gstd_bootstrap_prepare(&request, &material);
    gstd_credential_clear(&material);
    if (scope >= 0)
        close(scope);
    return ok ? 0 : -1;
}
static json_t *registered_plan(const json_t *config, const gstd_network_namespace *network,
                               const char *fixture, const char *mode, int held[8],
                               gstd_execution_inputs *retained) {
    int server = !strcmp(mode, "server"), api = !strcmp(mode, "api-check"),
        git = !strcmp(mode, "git-server"),
        migration = !strcmp(mode, "migration") || !strcmp(mode, "migration-cancel");
    const char *root = json_string_value(json_object_get(json_object_get(config, "root"), "path"));
    const char *data = NULL;
    json_t *mounts = json_object_get(config, "mounts");
    for (size_t i = 0; i < json_array_size(mounts); i++) {
        json_t *mount = json_array_get(mounts, i);
        const char *destination = json_string_value(json_object_get(mount, "destination"));
        if (destination && (!strcmp(destination, "/var/lib/scylla") ||
                            !strcmp(destination, "/var/lib/gitstore/repositories"))) {
            if (data)
                return NULL;
            data = json_string_value(json_object_get(mount, "source"));
        }
    }
    if (!root || ((server || git) && (!data || !private(data, 1))))
        return NULL;
    held[0] = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (server || git)
        held[1] = open(data, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/resolver.conf", fixture);
    if (held[0] < 3 || ((server || git) && held[1] < 3) || length < 0 ||
        length >= (int)sizeof(path))
        return NULL;
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    const char bytes[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
    int valid = output >= 3 && write(output, bytes, sizeof(bytes) - 1) == sizeof(bytes) - 1 &&
                !fsync(output);
    if (output >= 0)
        close(output);
    if (!valid)
        return NULL;
    held[2] = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    json_t *components = json_pack("[s,s]", "api", "git");
    json_t *manifest = gstd_asset_manifest_for_datastore(components, "arm64", "scylladb");
    json_decref(components);
    gstd_execution_inputs input = {.manifest = manifest,
                                   .component = git                ? "git"
                                                : api || migration ? "api"
                                                                   : "scylladb",
                                   .purpose = server || api || git ? GSTD_EXEC_SERVICE
                                              : migration          ? GSTD_EXEC_SCHEMA_MIGRATE
                                              : !strcmp(mode, "keyspace")
                                                  ? GSTD_EXEC_SCYLLA_KEYSPACE
                                                  : GSTD_EXEC_SCYLLA_READY,
                                   .rootfs_fd = held[0],
                                   .network = network};
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++)
        input.role_fds[i] = -1;
    input.role_fds[GSTD_MOUNT_DATASTORE] = server ? held[1] : -1;
    input.role_fds[GSTD_MOUNT_RESOLVER] = held[2];
    if (api || git) {
        length = snprintf(path, sizeof(path), "%s/shared-bootstrap.json", fixture);
        json_t *shared = length > 0 && length < (int)sizeof(path) ? load_json(path) : NULL;
        const char *source = json_string_value(json_object_get(shared, "path"));
        if (!source || !private(source, 1)) {
            json_decref(shared);
            goto api_fail;
        }
        held[7] = open(source, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        json_decref(shared);
        if (held[7] < 3)
            goto api_fail;
        const char *files[] = {"api-base.toml", "api-owned.toml", "policy.yaml", "users.yaml"};
        const unsigned roles[] = {GSTD_MOUNT_BASE, GSTD_MOUNT_OVERLAY, GSTD_MOUNT_POLICY,
                                  GSTD_MOUNT_USERS};
        for (unsigned i = 0; i < (git ? 1u : 4u); i++) {
            held[3 + i] =
                openat(held[7], git ? "git.toml" : files[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (held[3 + i] < 3)
                goto api_fail;
            input.role_fds[roles[i]] = held[3 + i];
        }
        if (git)
            input.role_fds[GSTD_MOUNT_REPOSITORIES] = held[1];
    }
    json_t *spec = held[2] >= 3 ? gstd_execution_build(&input) : NULL;
    if (spec) {
        *retained = input;
        json_incref(manifest);
    }
    json_decref(manifest);
    return spec;
api_fail:
    json_decref(manifest);
    return NULL;
}
typedef struct {
    char bytes[8192];
    size_t length;
} health_diagnostic;
static size_t health_receive(char *bytes, size_t size, size_t count, void *opaque) {
    health_diagnostic *response = opaque;
    if (size && count > (sizeof(response->bytes) - response->length) / size)
        return 0;
    size_t length = size * count;
    memcpy(response->bytes + response->length, bytes, length);
    response->length += length;
    return length;
}
static void health_failure(void) {
    health_diagnostic response = {0};
    CURL *curl = curl_easy_init();
    if (!curl)
        return;
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:4000/ready");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, health_receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    long code = 0;
    CURLcode status = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    json_t *value = gstd_json_parse((unsigned char *)response.bytes, response.length);
    const char *version = json_string_value(json_object_get(value, "version"));
    const char *health = json_string_value(json_object_get(value, "status"));
    dprintf(2,
            "registered readiness rejected: transport=%d http=%ld version=%.128s status=%.128s\n",
            (int)status, code, version ? version : "absent", health ? health : "absent");
    json_decref(value);
    curl_easy_cleanup(curl);
}
static int api_ready(const gstd_network_namespace *network) {
    pid_t parent = getpid(), child = fork();
    if (!child) {
        gstd_bootstrap_http_context context = {.control_owner = parent, .network = network};
        gstd_bootstrap_http_status status =
            gstd_bootstrap_http(&context, GSTD_BOOT_API_READY, NULL, NULL);
        if (status == GSTD_BOOT_HTTP_REJECTED)
            health_failure();
        _exit(status == GSTD_BOOT_HTTP_READY ? 0 : status == GSTD_BOOT_HTTP_PENDING ? 10 : 1);
    }
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status) == 0 ? 1 : WEXITSTATUS(status) == 10 ? 0 : -1;
}
static int git_listening(const gstd_network_namespace *network) {
    pid_t child = fork();
    if (!child) {
        if (gstd_network_namespace_check(network) || setns(network->user_fd, CLONE_NEWUSER) ||
            setns(network->network_fd, CLONE_NEWNET))
            _exit(1);
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        struct sockaddr_in address = {.sin_family = AF_INET,
                                      .sin_port = htons(50051),
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        int connected = fd >= 0 && !connect(fd, (struct sockaddr *)&address, sizeof(address));
        if (!connected && fd >= 0 && errno == EINPROGRESS) {
            struct pollfd pending = {.fd = fd, .events = POLLOUT};
            int error = 0;
            socklen_t size = sizeof(error);
            connected = poll(&pending, 1, 200) > 0 &&
                        !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) && !error;
        }
        _exit(connected ? 0 : 1);
    }
    int status;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           !WEXITSTATUS(status);
}
static int writer_held(int data, pid_t pid, uint64_t expected_start) {
    int fd = openat(data, ".fixture-writer.lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd/3", (int)pid);
    struct stat lock = {0}, native = {0};
    uint64_t before = 0, after = 0;
    int valid = fd >= 3 && !fstat(fd, &lock) && S_ISREG(lock.st_mode) && lock.st_uid == geteuid() &&
                lock.st_nlink == 1 && !lock.st_size && (lock.st_mode & 07777) == 0600 &&
                !gstd_native_process_start(pid, &before) && before == expected_start;
    if (valid && !stat(path, &native))
        valid = lock.st_dev == native.st_dev && lock.st_ino == native.st_ino;
    else if (valid) {
        /* User-namespace exec can deny ptrace-style /proc/fd access. This is
         * not an FD receipt: the independent kernel flock remains observable
         * after the controller and reaped native worker close their copies. */
        valid = errno == EACCES || errno == EPERM;
        if (valid)
            dprintf(2, "native FD link inaccessible; kernel lock observation only\n");
    }
    if (valid) {
        int locked = flock(fd, LOCK_EX | LOCK_NB);
        valid = locked < 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
    }
    valid = valid && !gstd_native_process_start(pid, &after) && after == expected_start;
    if (!valid)
        dprintf(2, "native writer check pid=%d fd=%d errno=%d lock=%llu:%llu native=%llu:%llu\n",
                (int)pid, fd, errno, (unsigned long long)lock.st_dev,
                (unsigned long long)lock.st_ino, (unsigned long long)native.st_dev,
                (unsigned long long)native.st_ino);
    if (fd >= 0)
        close(fd);
    return valid;
}
int main(int argc, char **argv) {
    if ((argc != 2 && argc != 3) || argv[1][0] != '/' || !geteuid() || !private(argv[1], 1))
        return 2;
    const char *mode = argc == 3 ? argv[2] : "version";
    int owned_network =
        strcmp(mode, "readiness") && strcmp(mode, "keyspace") && strcmp(mode, "migration") &&
        strcmp(mode, "ledger") && strcmp(mode, "lock-hold") && strcmp(mode, "lock-release") &&
        strcmp(mode, "api-check") && strcmp(mode, "git-server") && strcmp(mode, "migration-cancel");
    int server = !strcmp(mode, "server"), git = !strcmp(mode, "git-server");
    int result = 1, output = -1, death = -1, collected = 0, created = 0, stage = 1,
        unsafe_helper_retained = 0, cancellation_proven = 0;
    int held_plan[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    gstd_execution_inputs plan_inputs = {0};
    gstd_native_context dispatch = {.state_fd = -1, .bundle_fd = -1, .writer_lease_fd = -1};
    gstd_network_status ns_status = GSTD_NETWORK_OK;
    json_t *config = NULL, *namespaces = NULL;
    char bundle[4096], state[4096], file[4096], rootfs[4096], boot[37], identifier[64];
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    gstd_native_population population = GSTD_NATIVE_POPULATION_INITIAL;
    gstd_crun_state actual = {0};
    gstd_native_exit exited = {0};
    gstd_crun_request request = {.state_root = state, .id = identifier, .bundle = bundle};
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) || !gstd_crun_host_ready() ||
        gstd_native_boot_id(boot))
        return 77;
    struct rlimit core = {0, 0}, address = {UINT64_C(512) * 1024 * 1024, UINT64_C(1) << 46},
                  files = {UINT64_C(64) * 1024 * 1024, UINT64_C(8) * 1024 * 1024 * 1024};
#ifdef GITSTORED_INSTRUMENTED
    address.rlim_cur = address.rlim_max;
#endif
    if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_AS, &address) ||
        setrlimit(RLIMIT_FSIZE, &files))
        return 1;
    gstd_json_initialize();
    if (!strcmp(mode, "prepare")) {
        json_t *components = json_pack("[s,s]", "api", "git");
        json_t *manifest = gstd_asset_manifest_for_datastore(components, "arm64", "scylladb");
        int ready = manifest && !prepare_bootstrap(argv[1], manifest);
        json_decref(manifest);
        json_decref(components);
        return ready ? 0 : 1;
    }
    json_t *identity = json_pack("{s:s}", "fixture", argv[1]);
    char hash[65];
    int identified = identity && !gstd_fingerprint(identity, hash);
    json_decref(identity);
    if (!identified)
        return 1;
    snprintf(identifier, sizeof(identifier), "scylla-%.48s", hash);
    if (snprintf(bundle, sizeof(bundle), "%s/bundle", argv[1]) < 0 ||
        snprintf(state, sizeof(state), "%s/state", argv[1]) < 0 ||
        snprintf(file, sizeof(file), "%s/bundle/config.json", argv[1]) < 0 || !private(bundle, 1) ||
        !private(state, 1) || !private(file, 0))
        goto done;
    int input = open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char buffer[65537];
    ssize_t count = input < 0 ? -1 : read(input, buffer, sizeof(buffer));
    if (input >= 0)
        close(input);
    config = count > 0 && count <= 65536 ? gstd_json_parse((unsigned char *)buffer, (size_t)count)
                                         : NULL;
    const char *root = json_string_value(json_object_get(json_object_get(config, "root"), "path"));
    if (!root || strlen(root) >= sizeof(rootfs))
        goto done;
    memcpy(rootfs, root, strlen(root) + 1);
    json_t *args = json_object_get(json_object_get(config, "process"), "args");
    if (arguments(args, mode))
        goto done;
    stage = 2;
    if (owned_network)
        ns_status = gstd_network_namespace_open(&network);
    else
        ns_status = namespace_borrow(argv[1], &network) ? GSTD_NETWORK_FAILED : GSTD_NETWORK_OK;
    if (ns_status != GSTD_NETWORK_OK)
        goto done;
    stage = 3;
    if (gstd_network_namespace_check(&network))
        goto done;
    stage = 4;
    if (server && namespace_record(argv[1], &network))
        goto done;
    files.rlim_cur = files.rlim_max;
    if (setrlimit(RLIMIT_FSIZE, &files))
        goto done;
    if (server || !strcmp(mode, "readiness") || !strcmp(mode, "keyspace") ||
        !strcmp(mode, "migration") || !strcmp(mode, "migration-cancel") ||
        !strcmp(mode, "api-check") || git) {
        json_t *registered =
            registered_plan(config, &network, argv[1], mode, held_plan, &plan_inputs);
        if (!registered)
            goto done;
        json_decref(config);
        config = registered;
    } else {
        namespaces = json_object_get(json_object_get(config, "linux"), "namespaces");
        for (size_t i = 0; i < json_array_size(namespaces); i++) {
            json_t *entry = json_array_get(namespaces, i);
            const char *kind = json_string_value(json_object_get(entry, "type"));
            if (kind && (!strcmp(kind, "user") || !strcmp(kind, "network"))) {
                char name[64];
                snprintf(name, sizeof(name), "/proc/self/fd/%d",
                         !strcmp(kind, "user") ? network.user_fd : network.network_fd);
                if (json_object_set_new(entry, "path", json_string(name)))
                    goto done;
            }
        }
        if (json_object_del(json_object_get(config, "linux"), "uidMappings") ||
            json_object_del(json_object_get(config, "linux"), "gidMappings"))
            goto done;
    }
    snprintf(file, sizeof(file), "%s/output.log", argv[1]);
    stage = 5;
    output = open(file, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 3)
        goto done;
    if (plan_inputs.manifest) {
        dispatch =
            (gstd_native_context){.backend_id = identifier,
                                  .component = plan_inputs.component,
                                  .boot_id = boot,
                                  .state_fd = open(state, O_RDONLY | O_DIRECTORY | O_CLOEXEC),
                                  .bundle_fd = open(bundle, O_RDONLY | O_DIRECTORY | O_CLOEXEC),
                                  .rootfs_fd = held_plan[0],
                                  .stdout_fd = output,
                                  .stderr_fd = output,
                                  .writer_lease_fd = -1,
                                  .control_owner = getpid(),
                                  .configuration = config,
                                  .registered_inputs = &plan_inputs};
        if (dispatch.state_fd < 3 || dispatch.bundle_fd < 3)
            goto done;
        if (server || git) {
            /* This disposable fixture lock is not daemon ownership authority.
             * It exercises the production worker's inherited writer FD rule. */
            dispatch.writer_lease_fd = openat(held_plan[1], ".fixture-writer.lock",
                                              O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (dispatch.writer_lease_fd < 3 || flock(dispatch.writer_lease_fd, LOCK_EX | LOCK_NB))
                goto done;
        }
        registered_context = &dispatch;
    }
    if (plan_inputs.manifest && plan_inputs.purpose != GSTD_EXEC_SERVICE) {
        gstd_helper helper = {0};
        int finished = gstd_helper_begin(&dispatch, &helper) ? -1 : 0;
        while (!finished) {
            int event = gstd_helper_poll(&helper, -1);
            if (event == 1) {
                /* This is a developer receipt, not daemon/SQLite authority.
                 * Exercise the production pause/commit/permit boundary. */
                gstd_crun_state paused = {0};
                if (gstd_helper_poll(&helper, -1) != 1 || helper.worker.worker ||
                    gstd_crun_observe(&request, bundle, rootfs, &paused) || !paused.created ||
                    paused.running || paused.pid != helper.context.expected_pid ||
                    paused.start_time != helper.context.expected_start_time) {
                    finished = -1;
                    break;
                }
                snprintf(file, sizeof(file), "%s/helper-observation.json", argv[1]);
                json_t *receipt = json_pack(
                    "{s:s,s:s,s:i,s:I,s:s,s:i}", "kind", "developer-helper-observation", "id",
                    identifier, "pid", (int)paused.pid, "startTime", (json_int_t)paused.start_time,
                    "bootId", boot, "purpose", (int)plan_inputs.purpose);
                char *record = receipt ? gstd_json_dump(receipt) : NULL;
                json_decref(receipt);
                int saved = open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
                int committed = record && saved >= 3 &&
                                write(saved, record, strlen(record)) == (ssize_t)strlen(record) &&
                                !fsync(saved);
                if (saved >= 0)
                    close(saved);
                free(record);
                if (!committed || gstd_helper_permit_start(&helper)) {
                    finished = -1;
                    break;
                }
            } else if (event == 2)
                finished = 1;
            else if (event < 0)
                finished = -1;
            else if (!strcmp(mode, "migration-cancel") && helper.deadline_ms &&
                     !helper.worker.worker) {
                int notification[2];
                if (pipe2(notification, O_CLOEXEC | O_NONBLOCK)) {
                    finished = -1;
                    break;
                }
                int alive = gstd_native_watch_poll(helper.death_fd) == 0 &&
                            gstd_native_population_empty(&helper.population) == 0;
                int notified = write(notification[1], "x", 1) == 1;
                int cancelled = notified ? gstd_helper_poll(&helper, notification[0]) : -1;
                close(notification[0]);
                close(notification[1]);
                gstd_crun_state running = {0};
                cancellation_proven =
                    alive && cancelled == 2 && helper.outcome == -2 && helper.may_exist &&
                    !helper.deleted && gstd_native_watch_poll(helper.death_fd) == 0 &&
                    !gstd_crun_observe(&request, bundle, rootfs, &running) && running.running &&
                    running.pid == helper.context.expected_pid &&
                    running.start_time == helper.context.expected_start_time;
                finished = cancellation_proven ? 1 : -1;
            } else
                pause_ms();
        }
        if (!helper.stage)
            goto done;
        dispatch = helper.context;
        actual.pid = dispatch.expected_pid;
        actual.start_time = dispatch.expected_start_time;
        created = helper.may_exist;
        exited = helper.exit;
        collected = exited.exited || exited.signalled;
        death = helper.death_fd;
        helper.death_fd = -1;
        population = helper.population;
        helper.population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
        unsafe_helper_retained = helper.may_exist && helper.deadline_ms &&
                                 (plan_inputs.purpose == GSTD_EXEC_SCYLLA_KEYSPACE ||
                                  plan_inputs.purpose == GSTD_EXEC_SCHEMA_MIGRATE);
        snprintf(file, sizeof(file), "%s/helper-lifecycle.json", argv[1]);
        json_t *lifecycle = json_pack("{s:b,s:b,s:b,s:i,s:i,s:b,s:b}", "productionHelperActor", 1,
                                      "deleted", helper.deleted, "mayExist", helper.may_exist,
                                      "outcome", helper.outcome, "exitCode", helper.exit.code,
                                      "cancellationPreservedNative", cancellation_proven,
                                      "exitObserved", helper.exit.exited || helper.exit.signalled);
        char *lifecycle_bytes = lifecycle ? gstd_json_dump(lifecycle) : NULL;
        json_decref(lifecycle);
        int lifecycle_fd = open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        int lifecycle_saved = lifecycle_bytes && lifecycle_fd >= 3 &&
                              write(lifecycle_fd, lifecycle_bytes, strlen(lifecycle_bytes)) ==
                                  (ssize_t)strlen(lifecycle_bytes) &&
                              !fsync(lifecycle_fd);
        if (lifecycle_fd >= 0)
            close(lifecycle_fd);
        free(lifecycle_bytes);
        if (finished == 1 && lifecycle_saved && !helper.outcome && helper.deleted &&
            !helper.may_exist) {
            result = 0;
            puts("PASS: production helper actor observed normal exit, empty cgroup and DELETE");
        }
        if (finished == 1 && lifecycle_saved && cancellation_proven)
            result = 0;
        if (finished == 1)
            (void)gstd_helper_close(&helper);
        goto done;
    }
    char *encoded = gstd_json_dump(config);
    if (!encoded)
        goto done;
    stage = 6;
    int made = apply(&request, encoded, 0, output);
    free(encoded);
    /* CREATE can leave a paused native init even after a failed worker reply. */
    stage = 7;
    if (gstd_crun_observe(&request, bundle, rootfs, &actual))
        goto done;
    created = 1;
    dispatch.expected_pid = actual.pid;
    dispatch.expected_start_time = actual.start_time;
    stage = 8;
    if (gstd_native_watch_open(actual.pid, actual.start_time, boot, &death) ||
        gstd_native_population_open(request.id, actual.pid, actual.start_time, boot, &population) ||
        made || apply(&request, NULL, 1, output))
        goto done;
    if (dispatch.writer_lease_fd >= 3) {
        close(dispatch.writer_lease_fd);
        dispatch.writer_lease_fd = -1;
        if (!writer_held(held_plan[1], actual.pid, actual.start_time))
            goto done;
    }
    stage = 9;
    if (!strcmp(mode, "api-check")) {
        int healthy = 0;
        int64_t deadline = now_ms() + 60000;
        while (now_ms() < deadline &&
               !(collected = gstd_native_exit_poll(actual.pid, actual.start_time, &exited))) {
            healthy = api_ready(&network);
            if (healthy)
                break;
            /* Match the daemon actor's 250 ms health cadence. A millisecond
             * experiment loop exhausts the API's request-rate budget. */
            struct timespec interval = {0, 250000000};
            nanosleep(&interval, NULL);
        }
        if (healthy != 1 || collected || syscall(SYS_pidfd_send_signal, death, SIGTERM, NULL, 0))
            goto done;
        deadline = now_ms() + 30000;
        while (now_ms() < deadline &&
               !(collected = gstd_native_exit_poll(actual.pid, actual.start_time, &exited)))
            pause_ms();
    } else if (server || git) {
        if (git) {
            int listening = 0;
            int64_t deadline = now_ms() + 20000;
            while (now_ms() < deadline &&
                   !(collected = gstd_native_exit_poll(actual.pid, actual.start_time, &exited))) {
                if ((listening = git_listening(&network)))
                    break;
                pause_ms();
            }
            if (!listening || collected)
                goto done;
            if (!writer_held(held_plan[1], actual.pid, actual.start_time))
                goto done;
        }
        printf(
            "{\"nativeStarted\":true,\"registeredPlan\":true,\"registeredWorker\":%s,\"pid\":%d}\n",
            registered_context ? "true" : "false", (int)actual.pid);
        fflush(stdout);
        int stop = 0;
        while (!stop &&
               !(collected = gstd_native_exit_poll(actual.pid, actual.start_time, &exited))) {
            struct pollfd control = {STDIN_FILENO, POLLIN, 0};
            int ready = poll(&control, 1, 100);
            if (ready > 0 && (control.revents & (POLLIN | POLLHUP | POLLERR))) {
                char command[32];
                ssize_t n = read(0, command, sizeof(command));
                if (n == 6 && !memcmp(command, "lease\n", 6)) {
                    int held = writer_held(held_plan[1], actual.pid, actual.start_time);
                    printf("{\"writerLockHeldAfterParentClose\":%s}\n", held ? "true" : "false");
                    fflush(stdout);
                    if (!held)
                        goto done;
                } else if (n == 0 || (n == 5 && !memcmp(command, "stop\n", 5)))
                    stop = 1;
                else if (n < 0 && errno == EINTR)
                    continue;
                else {
                    stage = 10;
                    goto done;
                }
            } else if (ready < 0 && errno != EINTR)
                goto done;
        }
        if (!collected && stop) {
            if (syscall(SYS_pidfd_send_signal, death, SIGTERM, NULL, 0) && errno != ESRCH)
                goto done;
            int64_t deadline = now_ms() + 30000;
            while (now_ms() < deadline &&
                   !(collected = gstd_native_exit_poll(actual.pid, actual.start_time, &exited)))
                pause_ms();
        }
    } else {
        int64_t deadline = now_ms() + (!strcmp(mode, "migration") ? 70000 : 20000);
        while (now_ms() < deadline &&
               !(collected = gstd_native_exit_poll(actual.pid, actual.start_time, &exited)))
            pause_ms();
    }
    if (collected != 1 || !exited.exited || exited.code || gstd_native_watch_poll(death) != 1 ||
        gstd_native_population_empty(&population) != 1 || apply(&request, NULL, 2, output))
        goto done;
    created = 0;
    result = 0;
    puts("PASS: pinned image helper executed as namespace UID 0/GID 0; observed exit and "
         "empty cgroup before non-forced delete");
done:
    if (result)
        fprintf(stderr,
                "scylla helper stage=%d namespaceStatus=%d collected=%d exited=%d signalled=%d "
                "exit=%d\n",
                stage, (int)ns_status, collected, exited.exited, exited.signalled, exited.code);
    if (created && unsafe_helper_retained && cancellation_proven) {
        /* Reconcile natural exit without changing the original uncertain
         * cancellation decision. Never signal the dispatched mutator. */
        int64_t deadline = now_ms() + 70000;
        while (now_ms() < deadline && !collected) {
            collected = gstd_native_exit_poll(actual.pid, actual.start_time, &exited);
            if (!collected)
                pause_ms();
        }
        int cleaned = collected == 1 && gstd_native_watch_poll(death) == 1 &&
                      gstd_native_population_empty(&population) == 1 &&
                      !apply(&request, NULL, 2, output);
        if (cleaned) {
            const char receipt[] = "{\"observedExit\":true,\"emptyCgroup\":true,\"deleted\":true,"
                                   "\"cancelOutcome\":-2}\n";
            snprintf(file, sizeof(file), "%s/helper-cleanup.json", argv[1]);
            int saved = open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            cleaned = saved >= 3 &&
                      write(saved, receipt, sizeof(receipt) - 1) == sizeof(receipt) - 1 &&
                      !fsync(saved);
            if (saved >= 0)
                close(saved);
            created = 0;
        }
        if (!cleaned)
            result = 1;
    }
    if (created && unsafe_helper_retained)
        fprintf(stderr, "Mutating helper ownership retained; no forced signal or delete\n");
    if (created && !unsafe_helper_retained) {
        if (death >= 3)
            (void)syscall(SYS_pidfd_send_signal, death, SIGKILL, NULL, 0);
        else {
            int number = 0;
            (void)gstd_crun_signal(&request, 1, &number);
        }
        int64_t deadline = now_ms() + 5000;
        while (now_ms() < deadline && !collected) {
            collected = gstd_native_exit_poll(actual.pid, actual.start_time, &exited);
            if (!collected)
                pause_ms();
        }
        if (collected == 1 && gstd_native_population_empty(&population) == 1)
            (void)apply(&request, NULL, 2, output);
    }
    if (death >= 0)
        close(death);
    gstd_native_population_close(&population);
    if (owned_network) {
        if (gstd_network_namespace_close(&network) != GSTD_NETWORK_OK)
            result = 1;
    } else {
        if (network.user_fd >= 0)
            close(network.user_fd);
        if (network.network_fd >= 0)
            close(network.network_fd);
    }
    if (output >= 0)
        close(output);
    if (dispatch.state_fd >= 0)
        close(dispatch.state_fd);
    if (dispatch.bundle_fd >= 0)
        close(dispatch.bundle_fd);
    if (dispatch.writer_lease_fd >= 0)
        close(dispatch.writer_lease_fd);
    json_decref((json_t *)plan_inputs.manifest);
    for (unsigned i = 0; i < 8; i++)
        if (held_plan[i] >= 0)
            close(held_plan[i]);
    json_decref(config);
    return result;
}
