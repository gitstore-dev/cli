#define _GNU_SOURCE 1
#include "filesystem_job.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "native_exit.h"
#include "network_records.h"
#include "network_restore.h"
#include "network_sandbox.h"
#include "network_worker.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <resolv.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static const char *payload = "worker1";
static void check(bool value, const char *message) {
    if (!value) {
        perror(message);
        _exit(1);
    }
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
static int fixture_records(const char *root, const json_t *manifest, sqlite3 **database,
                           char operation[65], char store[33]) {
    char path[4096], platform[64];
    snprintf(path, sizeof(path), "%s/probe.db", root);
    if (gstd_state_open(path, database))
        return -1;
    sqlite3 *db = *database;
    const char *architecture =
        json_string_value(json_object_get(json_object_get(manifest, "platform"), "architecture"));
    snprintf(platform, sizeof(platform), "linux-%s", architecture);
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,?)", -1, &statement,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(statement, 1, json_string_value(json_object_get(manifest, "serverRevision")),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, json_string_value(json_object_get(manifest, "profileDigest")),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, platform, -1, SQLITE_TRANSIENT);
    int inserted = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (inserted != SQLITE_DONE)
        return -1;
    const char text[] =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"probe\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"native-probe\",\"params\":{\"deploymentId\":\"one\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\"}}";
    json_t *message = gstd_json_parse((const unsigned char *)text, sizeof(text) - 1),
           *components = json_pack("[s]", "git");
    gstd_request request;
    int accepted =
        message && components && !gstd_request_parse(message, &request) &&
        gstd_accept(db, &request, components, "repositories", operation) == GSTD_ACCEPT_NEW;
    json_decref(message);
    json_decref(components);
    if (!accepted || sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &statement,
                                        NULL) != SQLITE_OK)
        return -1;
    int found = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_bytes(statement, 0) == 32;
    if (found)
        snprintf(store, 33, "%s", sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    if (!found ||
        sqlite3_prepare_v2(
            db,
            "INSERT INTO resource_ownership "
            "VALUES(?,'network','one',?,'reserved',?,'held',strftime('%Y-%m-%dT%H:%M:%SZ','now'))",
            -1, &statement, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(statement, 1, "network-fixture", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, operation, -1, SQLITE_TRANSIENT);
    sqlite3_bind_null(statement, 3);
    inserted = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return inserted == SQLITE_DONE ? 0 : -1;
}
static int sandbox_checks(int root) {
    if (mkdirat(root, "sandbox-syscalls", 0700))
        return -1;
    int scope = openat(root, "sandbox-syscalls", O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (scope < 3)
        return -1;
    pid_t child = fork();
    if (!child) {
        if (gstd_network_sandbox(scope))
            _exit(1);
        close(scope);
        close(root);
        errno = 0;
        if (socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) != -1 || errno != EPERM)
            _exit(2);
        errno = 0;
        if (unshare(CLONE_NEWUSER) != -1 || errno != EPERM)
            _exit(3);
        char *arguments[] = {"/nonexistent", NULL}, *environment[] = {NULL};
        errno = 0;
        if (execve(arguments[0], arguments, environment) != -1 || errno != EPERM)
            _exit(4);
        errno = 0;
        if (kill(getppid(), 0) != -1 || errno != EPERM)
            _exit(6);
        errno = 0;
        if (syscall(SYS_pidfd_open, getppid(), 0) != -1 || errno != EPERM)
            _exit(7);
        errno = 0;
        if (open("/write-probe", O_CREAT | O_WRONLY | O_CLOEXEC, 0600) != -1 || errno != EROFS)
            _exit(5);
        _exit(0);
    }
    close(scope);
    int status = 0;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                   !WEXITSTATUS(status)
               ? 0
               : -1;
}
int main(int argc, char **argv) {
    if ((argc != 2 && (argc != 3 || strcmp(argv[2], "lost-ack"))) || !geteuid())
        return 2;
    alarm(25);
    gstd_json_initialize();
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    check(root >= 3 && !mkdirat(root, "scope", 0700), "private scope");
    check(!sandbox_checks(root), "real sandbox syscall denials");
    int scope = openat(root, "scope", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    json_t *components = json_pack("[s,s]", "api", "git");
#if defined(__aarch64__)
    json_t *manifest = gstd_asset_manifest(components, "arm64");
#else
    json_t *manifest = gstd_asset_manifest(components, "amd64");
#endif
    json_decref(components);
    sqlite3 *database = NULL;
    char operation[65], store[33];
    check(!fixture_records(argv[1], manifest, &database, operation, store),
          "accepted start reservation");
    gstd_network_worker_request request = {.manifest = manifest,
                                           .store_id = store,
                                           .resource_id = "network-fixture",
                                           .operation_id = operation,
                                           .scope_fd = scope};
    gstd_network_worker owner = GSTD_NETWORK_WORKER_INITIAL;
    gstd_network_worker_observation observation = GSTD_NETWORK_OBSERVATION_INITIAL;
    json_t *identity = NULL;
    gstd_filesystem_reply recover_reply = {0};
    int result = 1;
    pid_t workload = -1;
    int status;
#define REQUIRE(value, message)                                                                    \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            perror(message);                                                                       \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
    REQUIRE(gstd_network_worker_prepare(&request, &owner) == GSTD_NETWORK_OK, "prepare");
    struct stat info;
    REQUIRE(fstatat(scope, "owner.json", &info, AT_SYMLINK_NOFOLLOW) && errno == ENOENT,
            "no side effect before permit");
    gstd_network_worker rival = GSTD_NETWORK_WORKER_INITIAL;
    REQUIRE(gstd_network_worker_prepare(&request, &rival) != GSTD_NETWORK_OK, "rival lock");
    identity = gstd_network_worker_identity(&owner);
    REQUIRE(identity, "prepared identity");
    REQUIRE(gstd_network_record_bind(database, &request, &owner) == -1, "no autocommit bind");
    REQUIRE(sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK, "begin bind");
    gstd_network_worker foreign = owner;
    foreign.scope_inode++;
    REQUIRE(gstd_network_record_bind(database, &request, &foreign) == -1, "changed scope refused");
    REQUIRE(!gstd_network_record_bind(database, &request, &owner), "bind accepted ownership");
    REQUIRE(!gstd_network_record_bind(database, &request, &owner), "idempotent identity bind");
    REQUIRE(sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) == SQLITE_OK,
            "commit before permit");
    REQUIRE(sqlite3_exec(database,
                         "UPDATE resource_ownership SET backend_identity='{}' WHERE "
                         "resource_id='network-fixture'",
                         NULL, NULL, NULL) != SQLITE_OK,
            "immutable worker receipt");
    REQUIRE(!gstd_network_worker_permit(&owner), "permit");
    int ready = 0;
    if (argc == 3)
        gstd_network_worker_close(&owner);
    for (unsigned n = 0; n < 1000 && !ready; n++) {
        ready =
            argc == 3
                ? (!gstd_network_worker_reopen(&request, identity, &owner, &observation) ? 1 : 0)
                : gstd_network_worker_ready(&owner, &observation);
        if (!ready) {
            struct timespec wait = {0, 10000000};
            nanosleep(&wait, NULL);
        }
    }
    REQUIRE(ready == 1, "ready");
    REQUIRE(sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK,
            "begin running");
    REQUIRE(!gstd_network_record_running(database, &owner, &observation),
            "record observed running");
    REQUIRE(sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) == SQLITE_OK,
            "commit observed running");
    char path[4096], bytes[8192];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)owner.pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0, "worker status");
    ssize_t count = read(fd, bytes, sizeof(bytes) - 1);
    close(fd);
    REQUIRE(count > 0, "status contents");
    bytes[count] = 0;
    REQUIRE(strstr(bytes, "NoNewPrivs:\t1\n") && strstr(bytes, "Seccomp:\t2\n") &&
                strstr(bytes, "CapEff:\t0000000000000000\n"),
            "sandbox kernel policy");
    snprintf(path, sizeof(path), "/proc/%ld/root/home", (long)owner.pid);
    REQUIRE(lstat(path, &info) && errno == ENOENT, "host home excluded");
    snprintf(path, sizeof(path), "/proc/%ld/root/proc", (long)owner.pid);
    REQUIRE(lstat(path, &info) && errno == ENOENT, "no proc escape");
    snprintf(path, sizeof(path), "/proc/%ld/fd/4", (long)owner.pid);
    REQUIRE(stat(path, &info) && errno == ENOENT, "scope directory closed");
    workload = fork();
    REQUIRE(workload >= 0, "workload fork");
    if (!workload)
        namespace_workload(&observation.network);
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(fd >= 0, "client socket");
    struct timeval timeout = {8, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(observation.api_port)};
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    REQUIRE(!connect(fd, (struct sockaddr *)&address, sizeof(address)), "connect forward");
    REQUIRE(read(fd, bytes, 7) == 7 && !memcmp(bytes, payload, 7), "DNS and forwarded reply");
    close(fd);
    REQUIRE(waitpid(workload, &status, 0) == workload && WIFEXITED(status) && !WEXITSTATUS(status),
            "guest success");
    workload = -1;
    pid_t pid = owner.pid;
    uint64_t stamp = owner.start;
    gstd_network_worker_close(&owner);
    struct timespec pause = {0, 100000000};
    nanosleep(&pause, NULL);
    uint64_t actual = 0;
    REQUIRE(!gstd_native_process_start(pid, &actual) && actual == stamp,
            "detached worker survives");
    REQUIRE(sqlite3_close(database) == SQLITE_OK, "close original ledger");
    database = NULL;
    snprintf(path, sizeof(path), "%s/probe.db", argv[1]);
    REQUIRE(!gstd_state_open(path, &database), "reopen parent ledger");
    sqlite3_stmt *saved = NULL;
    REQUIRE(sqlite3_prepare_v2(database,
                               "SELECT backend_identity FROM resource_ownership "
                               "WHERE resource_id='network-fixture'",
                               -1, &saved, NULL) == SQLITE_OK &&
                sqlite3_step(saved) == SQLITE_ROW,
            "durable identity query");
    json_t *persisted =
        gstd_json_parse(sqlite3_column_text(saved, 0), (size_t)sqlite3_column_bytes(saved, 0));
    REQUIRE(persisted && json_equal(persisted, identity), "committed identity survived reopen");
    sqlite3_finalize(saved);
    json_decref(identity);
    identity = persisted;
    gstd_network_worker_observation_close(&observation);
    gstd_network_worker_observation recovered = GSTD_NETWORK_OBSERVATION_INITIAL;
    json_t *wrong_identity = json_deep_copy(identity);
    json_object_set_new(wrong_identity, "startTime", json_integer((json_int_t)stamp + 1));
    REQUIRE(gstd_network_worker_reopen(&request, wrong_identity, &owner, &recovered) == -1 &&
                !owner.pid,
            "changed durable PID stamp refused");
    json_decref(wrong_identity);
    wrong_identity = json_deep_copy(identity);
    json_object_set_new(wrong_identity, "bootId",
                        json_string("00000000-0000-0000-0000-000000000000"));
    REQUIRE(gstd_network_worker_reopen(&request, wrong_identity, &owner, &recovered) == -1 &&
                !owner.pid,
            "foreign boot refused");
    json_decref(wrong_identity);
    gstd_filesystem_input recover_input = {.action = GSTD_FS_RECOVER_NETWORK,
                                           .manifest = manifest,
                                           .store_id = store,
                                           .resource_id = "network-fixture",
                                           .operation_id = operation,
                                           .scope_expected = identity,
                                           .descriptors = {scope, -1, -1}};
    gstd_filesystem_job recover_job = {0};
    REQUIRE(!gstd_filesystem_launch(&recover_input, 5000, &recover_job),
            "launch network recovery worker");
    int recovered_reply = 0;
    for (unsigned n = 0; n < 6000 && !recovered_reply; n++) {
        recovered_reply = gstd_filesystem_poll(&recover_job, -1, &recover_reply);
        if (!recovered_reply) {
            struct timespec delay = {0, 1000000};
            nanosleep(&delay, NULL);
        }
    }
    REQUIRE(recovered_reply == 1 && !recover_reply.status && recover_reply.descriptor_count == 4,
            "receive exact network recovery capability set");
    wrong_identity = json_deep_copy(identity);
    json_object_set_new(wrong_identity, "startTime", json_integer((json_int_t)stamp + 1));
    REQUIRE(gstd_network_restore_adopt(&recover_reply, wrong_identity, &owner, &recovered) == -1 &&
                !owner.pid && recover_reply.descriptor_count == 4,
            "changed parent identity preserves reply ownership");
    json_decref(wrong_identity);
    REQUIRE(!gstd_network_restore_adopt(&recover_reply, identity, &owner, &recovered) &&
                !recover_reply.descriptor_count,
            "adopt recovered owned worker");
    REQUIRE(recovered.pid == pid && recovered.api_port == observation.api_port &&
                !gstd_network_namespace_check(&recovered.network),
            "live recovered namespaces and ports");
    observation = recovered;
    gstd_network_worker wrong = owner;
    wrong.start++;
    REQUIRE(gstd_network_worker_signal(&wrong, 0) == -1, "stale PID refused");
    wrong = owner;
    wrong.boot_id[0] = wrong.boot_id[0] == '0' ? '1' : '0';
    REQUIRE(gstd_network_worker_signal(&wrong, 0) == -1, "wrong boot refused");
    REQUIRE(!gstd_network_worker_signal(&owner, 0), "stop known worker");
    for (unsigned n = 0; n < 500; n++) {
        pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            owner.pid = 0;
            break;
        }
        struct timespec wait = {0, 10000000};
        nanosleep(&wait, NULL);
    }
    REQUIRE(!owner.pid && WIFEXITED(status) && !WEXITSTATUS(status),
            "worker and keeper clean exit");
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof(address)) &&
                errno == ECONNREFUSED,
            "listener closed");
    close(fd);
    result = 0;
    puts("persistent network worker: durable permit, isolation, DNS, forwarding, reopen, fixed "
         "stop pass");
done:
    gstd_filesystem_reply_close(&recover_reply);
    if (!owner.pid && identity) {
        gstd_network_worker_observation cleanup_view = GSTD_NETWORK_OBSERVATION_INITIAL;
        if (!gstd_network_worker_reopen(&request, identity, &owner, &cleanup_view))
            gstd_network_worker_observation_close(&cleanup_view);
    }
    if (workload > 0) {
        kill(workload, SIGKILL);
        waitpid(workload, NULL, 0);
    }
    if (owner.pid > 0) {
        (void)gstd_network_worker_signal(&owner, 0);
        struct pollfd gone = {owner.pid_fd, POLLIN, 0};
        if (poll(&gone, 1, 2000) > 0)
            waitpid(owner.pid, NULL, WNOHANG);
    }
    gstd_network_worker_observation_close(&observation);
    gstd_network_worker_close(&owner);
    json_decref(identity);
    if (database)
        sqlite3_close(database);
    if (root >= 0)
        close(root);
    json_decref(manifest);
    if (scope >= 0)
        close(scope);
    return result;
}
