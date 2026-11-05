#define _GNU_SOURCE 1
#include "backend_libcrun.h"
#include "execution_plan.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "namespace_owner.h"
#include "native_exit.h"
#include "native_job.h"
#include "network_records.h"
#include "network_worker.h"
#include "volume_records.h"
#include "volumes.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int worker_signal;
static pid_t workload_pid;
static gstd_native_context context;
static char identifier[64];
static char bundle[4096], state_root[4096], configuration[4096];
static double monotonic(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        return 0;
    return value.tv_sec + (double)value.tv_nsec / 1000000000;
}
struct reply {
    int result;
    gstd_native_observation observed;
};
static gstd_native_observation latest;
static int raw_worker(const char *stage) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC))
        return -1;
    pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (!child) {
        close(fds[0]);
#ifndef GITSTORED_INSTRUMENTED
        struct rlimit address = {UINT64_C(512) * 1024 * 1024, UINT64_C(1) << 40};
        if (setrlimit(RLIMIT_AS, &address))
            _exit(98);
#endif
        struct rlimit files = {UINT64_C(8) * 1024 * 1024 * 1024, UINT64_C(8) * 1024 * 1024 * 1024};
        if (setrlimit(RLIMIT_FSIZE, &files))
            _exit(98);
        gstd_native_action action = GSTD_NATIVE_OBSERVE;
        if (!strcmp(stage, "create"))
            action = GSTD_NATIVE_CREATE;
        else if (!strcmp(stage, "start") || !strcmp(stage, "start-crash"))
            action = GSTD_NATIVE_START;
        else if (!strcmp(stage, "stop") || !strcmp(stage, "stale-stop"))
            action = GSTD_NATIVE_TERM;
        else if (!strcmp(stage, "delete"))
            action = GSTD_NATIVE_DELETE;
        if (!strcmp(stage, "stale-stop"))
            context.expected_start_time++;
        struct reply reply = {0};
        reply.result = gstd_native_apply(&context, action, &reply.observed);
        if (!strcmp(stage, "stale-stop")) {
            if (reply.result != GSTD_NATIVE_UNCERTAIN || reply.observed.effect_possible)
                _exit(97);
            reply.result = GSTD_NATIVE_OK;
        }
        if (reply.result == GSTD_NATIVE_OK && !strcmp(stage, "start-crash"))
            raise(SIGKILL);
        ssize_t sent = write(fds[1], &reply, sizeof(reply));
        close(fds[1]);
        _exit(sent == (ssize_t)sizeof(reply) && reply.result == GSTD_NATIVE_OK ? 0 : 1);
    }
    close(fds[1]);
    struct pollfd descriptor = {fds[0], POLLIN, 0};
    struct reply reply = {.result = -1};
    int ready = poll(&descriptor, 1, 10000);
    ssize_t received = ready > 0 ? read(fds[0], &reply, sizeof(reply)) : -1;
    close(fds[0]);
    int status = 0;
    double deadline = monotonic() + 5;
    while (waitpid(child, &status, WNOHANG) == 0) {
        if (monotonic() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            return -1;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    worker_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    if (received != (ssize_t)sizeof(reply) || !WIFEXITED(status) || WEXITSTATUS(status) ||
        reply.result != GSTD_NATIVE_OK) {
        fprintf(stderr, "adapter stage %s: status=%d nativeError=%d received=%zd signal=%d\n",
                stage, reply.result, reply.observed.native_error, received, worker_signal);
        return -1;
    }
    latest = reply.observed;
    printf("{\"stage\":\"%s\",\"result\":%d,\"nativeError\":%d}\n", stage, reply.result,
           reply.observed.native_error);
    return 0;
}
static int worker(const char *stage) {
    if (!strcmp(stage, "start-crash"))
        return raw_worker(stage);
    gstd_native_action action = GSTD_NATIVE_OBSERVE;
    if (!strcmp(stage, "create"))
        action = GSTD_NATIVE_CREATE;
    else if (!strcmp(stage, "start"))
        action = GSTD_NATIVE_START;
    else if (!strcmp(stage, "stop") || !strcmp(stage, "stale-stop"))
        action = GSTD_NATIVE_TERM;
    else if (!strcmp(stage, "delete"))
        action = GSTD_NATIVE_DELETE;
    gstd_native_context input = context;
    if (!strcmp(stage, "stale-stop"))
        input.expected_start_time++;
    gstd_native_job job = {0};
    if (gstd_native_launch(&input, action, 10000, &job)) {
        fprintf(stderr, "native worker launch refused stage=%s dumpable=%d\n", stage,
                prctl(PR_GET_DUMPABLE));
        return -1;
    }
    gstd_native_status result;
    int ready;
    while (!(ready = gstd_native_poll(&job, -1, &result, &latest))) {
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    if (ready != 1)
        return -1;
    if (!strcmp(stage, "stale-stop")) {
        if (result != GSTD_NATIVE_UNCERTAIN || latest.effect_possible)
            return -1;
    } else if (result != GSTD_NATIVE_OK) {
        fprintf(stderr, "async stage %s: status=%d nativeError=%d\n", stage, result,
                latest.native_error);
        return -1;
    }
    printf("{\"stage\":\"%s\",\"async\":true,\"result\":%d}\n", stage, result);
    return 0;
}
static int interrupted_observation(void) {
    gstd_native_job job = {0};
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) ||
        gstd_native_launch(&context, GSTD_NATIVE_OBSERVE, 10000, &job))
        return -1;
    kill(job.worker, SIGSTOP);
    if (write(descriptors[1], "x", 1) != 1)
        return -1;
    gstd_native_status status;
    gstd_native_observation observed;
    int ready;
    while (!(ready = gstd_native_poll(&job, descriptors[0], &status, &observed))) {
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    char marker;
    int valid = ready == 1 && status == GSTD_NATIVE_UNCERTAIN && observed.effect_possible &&
                read(descriptors[0], &marker, 1) == 1 && marker == 'x';
    close(descriptors[0]);
    close(descriptors[1]);
    if (valid)
        puts("PASS: cancellation reaped the worker, preserved uncertainty and left its signal "
             "unconsumed");
    return valid ? 0 : -1;
}
static int running(int expected) {
    if (worker("observe"))
        return -1;
    if (latest.running)
        workload_pid = latest.pid;
    return (!!latest.running) == expected ? 0 : -1;
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
            "VALUES(?,'workload','one',?,'reserved',?,'held',strftime('%Y-%m-%dT%H:%M:%SZ','now'))",
            -1, &statement, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(statement, 1, identifier, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, operation, -1, SQLITE_TRANSIENT);
    char boot[37];
    if (gstd_native_boot_id(boot)) {
        sqlite3_finalize(statement);
        return -1;
    }
    json_t *identity =
        json_pack("{s:s,s:s,s:s}", "kind", "libcrun", "id", identifier, "bootId", boot);
    char *encoded = gstd_json_dump(identity);
    json_decref(identity);
    if (!encoded) {
        sqlite3_finalize(statement);
        return -1;
    }
    sqlite3_bind_text(statement, 3, encoded, -1, SQLITE_TRANSIENT);
    free(encoded);
    inserted = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return inserted == SQLITE_DONE ? 0 : -1;
}
int main(int argc, char **argv) {
    int registered = argc >= 3 && !strcmp(argv[2], "registered");
    int joined = registered && argc >= 4 && !strcmp(argv[3], "network");
    int crashed = argc >= 3 && !strcmp(argv[argc - 1], "start-crash");
    if (geteuid() == 0 || argc < 2 || argc > 2 + registered + joined + crashed)
        return 77;
    struct stat metadata;
    if (lstat(argv[1], &metadata) || !S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid() ||
        (metadata.st_mode & 0777) != 0700)
        return 2;
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0))
        return 1;
    struct rlimit no_core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core))
        return 1;
    unsigned char random[16];
    int entropy = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (entropy < 0)
        return 1;
    size_t have = 0;
    while (have < sizeof(random)) {
        ssize_t count = read(entropy, random + have, sizeof(random) - have);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            close(entropy);
            return 1;
        }
        have += (size_t)count;
    }
    close(entropy);
    memcpy(identifier, "gstd-probe-", 11);
    for (size_t i = 0; i < sizeof(random); i++)
        snprintf(identifier + 11 + 2 * i, 3, "%02x", random[i]);
    if (snprintf(bundle, sizeof(bundle), "%s/bundle", argv[1]) < 0 ||
        snprintf(state_root, sizeof(state_root), "%s/state", argv[1]) < 0 ||
        snprintf(configuration, sizeof(configuration), "%s/bundle/config.json", argv[1]) < 0)
        return 2;
    gstd_json_initialize();
    json_error_t error;
    json_t *config = json_load_file(configuration, JSON_REJECT_DUPLICATES, &error);
    char rootfs[4096];
    if (!config || snprintf(rootfs, sizeof(rootfs), "%s/rootfs", bundle) < 0)
        return 2;
    char boot_id[37];
    if (gstd_native_boot_id(boot_id))
        return 2;
    context =
        (gstd_native_context){.state_fd = open(state_root, O_RDONLY | O_CLOEXEC | O_DIRECTORY),
                              .bundle_fd = open(bundle, O_RDONLY | O_CLOEXEC | O_DIRECTORY),
                              .rootfs_fd = open(rootfs, O_RDONLY | O_CLOEXEC | O_DIRECTORY),
                              .backend_id = identifier,
                              .boot_id = boot_id,
                              .component = "fixture",
                              .test_fixture = 1,
                              .control_owner = getpid(),
                              .configuration = config};
    gstd_execution_inputs input = {.component = "git", .rootfs_fd = context.rootfs_fd};
    for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
        input.role_fds[i] = -1;
    json_t *manifest = NULL;
    int volume_root_fd = -1;
    gstd_volume_root volume_owner = {.lock_fd = -1};
    gstd_volume_handle volume_handle = {.data_fd = -1, .writer_fd = -1};
    char fixture_store[33], operation[65], producer[65];
    sqlite3 *database = NULL;
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    gstd_network_worker network_worker = GSTD_NETWORK_WORKER_INITIAL;
    gstd_network_worker_observation network_observed = GSTD_NETWORK_OBSERVATION_INITIAL;
    if (registered) {
        json_t *components = json_pack("[s]", "git");
#if defined(__aarch64__)
        manifest = gstd_asset_manifest(components, "arm64");
#else
        manifest = gstd_asset_manifest(components, "amd64");
#endif
        json_decref(components);
        char source[4096];
        snprintf(source, sizeof(source), "%s/registered-config.toml", argv[1]);
        input.role_fds[GSTD_MOUNT_BASE] = open(source, O_RDONLY | O_CLOEXEC);
        snprintf(source, sizeof(source), "%s/volumes", argv[1]);
        volume_root_fd = open(source, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        gstd_volume_identity expected;
        if (fixture_records(argv[1], manifest, &database, operation, fixture_store) ||
            volume_root_fd < 0 ||
            gstd_volume_root_lock(volume_root_fd, fixture_store, &volume_owner) != GSTD_VOLUME_OK ||
            sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK ||
            gstd_volume_record_begin(database, &volume_owner, "repositories", operation, producer,
                                     &expected) != 0 ||
            sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) != SQLITE_OK ||
            gstd_volume_acquire(volume_root_fd, &volume_owner, "repositories", producer, 1, NULL,
                                &volume_handle) != GSTD_VOLUME_OK ||
            sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK ||
            gstd_volume_record_ready(database, &volume_owner, "repositories", operation,
                                     &volume_handle) ||
            sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
            return 2;
        input.role_fds[GSTD_MOUNT_REPOSITORIES] = volume_handle.data_fd;
        context.writer_lease_fd = volume_handle.writer_fd;
        if (joined) {
            sqlite3_stmt *reservation = NULL;
            if (sqlite3_prepare_v2(database,
                                   "INSERT INTO resource_ownership "
                                   "VALUES('network-probe','network','one',?,'reserved',NULL,'held'"
                                   ",strftime('%Y-%m-%dT%H:%M:%SZ','now'))",
                                   -1, &reservation, NULL) != SQLITE_OK)
                return 2;
            sqlite3_bind_text(reservation, 1, operation, -1, SQLITE_TRANSIENT);
            int result = sqlite3_step(reservation);
            sqlite3_finalize(reservation);
            if (result != SQLITE_DONE)
                return 2;
            snprintf(source, sizeof(source), "%s/network-scope", argv[1]);
            if (mkdir(source, 0700))
                return 2;
            int scope = open(source, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            gstd_network_worker_request network_request = {.manifest = manifest,
                                                           .store_id = fixture_store,
                                                           .resource_id = "network-probe",
                                                           .operation_id = operation,
                                                           .scope_fd = scope};
            if (scope < 3 ||
                gstd_network_worker_prepare(&network_request, &network_worker) != GSTD_NETWORK_OK ||
                sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK ||
                gstd_network_record_bind(database, &network_request, &network_worker) ||
                sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) != SQLITE_OK ||
                gstd_network_worker_permit(&network_worker))
                return 2;
            int ready = 0;
            for (unsigned attempt = 0; attempt < 1000 && !ready; attempt++) {
                ready = gstd_network_worker_ready(&network_worker, &network_observed);
                if (!ready) {
                    struct timespec delay = {0, 10000000};
                    nanosleep(&delay, NULL);
                }
            }
            if (ready != 1 ||
                sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK ||
                gstd_network_record_running(database, &network_worker, &network_observed) ||
                gstd_network_attachment_reserve(database, &network_worker, &network_observed,
                                                identifier) ||
                gstd_network_attachment_matches(database, operation, identifier,
                                                &network_observed.network) ||
                gstd_network_attachment_reserve(database, &network_worker, &network_observed,
                                                identifier) ||
                sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                return 2;
            gstd_network_worker_observation foreign_network = network_observed;
            foreign_network.network.network_inode++;
            if (sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK ||
                gstd_network_attachment_reserve(database, &network_worker, &foreign_network,
                                                identifier) != -1 ||
                gstd_network_attachment_matches(database, operation, identifier,
                                                &foreign_network.network) != -1 ||
                gstd_network_attachment_matches(database, operation, "unattached-workload",
                                                &network_observed.network) != -1 ||
                sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK ||
                sqlite3_exec(database, "UPDATE network_attachments SET network_inode='1'", NULL,
                             NULL, NULL) == SQLITE_OK ||
                sqlite3_exec(database, "DELETE FROM network_attachments", NULL, NULL, NULL) ==
                    SQLITE_OK)
                return 2;
            close(scope);
            network = network_observed.network;
            input.network = &network;
            snprintf(source, sizeof(source), "%s/resolver.conf", argv[1]);
            input.role_fds[GSTD_MOUNT_RESOLVER] = open(source, O_RDONLY | O_CLOEXEC);
        }
        input.manifest = manifest;
        json_decref(config);
        config = gstd_execution_build(&input);
        if (!config) {
            fputs("Registered synthetic launch plan was refused\n", stderr);
            return 2;
        }
        context.component = "git";
        context.test_fixture = 0;
        context.registered_inputs = &input;
        context.configuration = config;
    }
    if (joined) {
        gstd_network_namespace wrong = network;
        wrong.keeper_start++;
        gstd_execution_inputs invalid = input;
        invalid.network = &wrong;
        gstd_native_context refused = context;
        refused.registered_inputs = &invalid;
        gstd_native_job job = {0};
        if (gstd_native_launch(&refused, GSTD_NATIVE_CREATE, 1000, &job) != -1 || job.worker) {
            fputs("Stale namespace rejection boundary failed\n", stderr);
            return 1;
        }
    }
    char sentinel[4096];
    int n = snprintf(sentinel, sizeof(sentinel), "%s/sentinel-inherited", argv[1]);
    if (n < 0 || (size_t)n >= sizeof(sentinel))
        return 2;
    int fd = open(sentinel, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return 1; /* deliberately lacks CLOEXEC */
    int created = worker("create");
    if (!created) {
        context.expected_pid = latest.pid;
        context.expected_start_time = latest.start_time;
    }
    int started = created ? -1 : worker(crashed ? "start-crash" : "start");
    if (created || (!crashed && started) || (crashed && (!started || worker_signal != SIGKILL))) {
        close(fd);
        return 1;
    }
    if (registered) {
        close(volume_handle.writer_fd);
        volume_handle.writer_fd = -1;
        context.writer_lease_fd = -1;
        gstd_volume_root_close(&volume_owner);
        if (gstd_volume_root_lock(volume_root_fd, fixture_store, &volume_owner) != GSTD_VOLUME_OK)
            return 1;
        gstd_volume_handle rival = {.data_fd = -1, .writer_fd = -1};
        if (gstd_volume_acquire(volume_root_fd, &volume_owner, "repositories", producer, 0,
                                &volume_handle.identity, &rival) != GSTD_VOLUME_HELD)
            return 1;
        puts("PASS: workload retained the writer lease after parent handles closed; unrelated root "
             "lock was not inherited");
    }
    char marker[4096];
    n = snprintf(marker, sizeof(marker), "%s/%s/lifecycle", argv[1],
                 registered ? "volumes/volume-repositories/data" : "output");
    if (n < 0 || (size_t)n >= sizeof(marker)) {
        close(fd);
        return 2;
    }
    double deadline = monotonic() + 5;
    while (access(marker, F_OK)) {
        if (monotonic() >= deadline) {
            worker("stop");
            worker("delete");
            close(fd);
            return 1;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    if (joined) {
        snprintf(marker, sizeof(marker), "%s/volumes/volume-repositories/data/namespaces", argv[1]);
        FILE *proof = fopen(marker, "r");
        unsigned long long user, net;
        if (!proof)
            return 1;
        int valid = fscanf(proof, "%llu %llu", &user, &net) == 2 && user == network.user_inode &&
                    net == network.network_inode;
        fclose(proof);
        if (!valid)
            return 1;
        puts("PASS: registered workload joined the exact owned user/network namespace and "
             "stale-keeper CREATE was refused");
    }
    if (running(1)) {
        close(fd);
        return 1;
    }
    pid_t crash = fork();
    if (crash < 0) {
        close(fd);
        return 1;
    }
    if (!crash) {
        raise(SIGKILL);
        _exit(99);
    }
    int status;
    if (waitpid(crash, &status, 0) != crash || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGKILL || running(1)) {
        close(fd);
        return 1;
    }
    if (interrupted_observation() || running(1) || worker("stale-stop") || running(1) ||
        worker("stop")) {
        close(fd);
        return 1;
    }
    deadline = monotonic() + 5;
    while (running(0)) {
        if (monotonic() >= deadline) {
            close(fd);
            return 1;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    deadline = monotonic() + 5;
    int collected;
    gstd_native_exit exit_result;
    do {
        collected = gstd_native_exit_poll(workload_pid, context.expected_start_time, &exit_result);
        if (collected < 0) {
            close(fd);
            return 1;
        }
        if (!collected) {
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
    } while (!collected && monotonic() < deadline);
    if (collected != 1 || !exit_result.exited || exit_result.code != 0) {
        close(fd);
        return 1;
    }
    puts("PASS: subreaper collected actual payload exit code 0");
    if (worker("delete")) {
        close(fd);
        return 1;
    }
    close(fd);
    if (registered) {
        gstd_volume_handle reopened = {.data_fd = -1, .writer_fd = -1};
        if (gstd_volume_acquire(volume_root_fd, &volume_owner, "repositories", producer, 0,
                                &volume_handle.identity, &reopened) != GSTD_VOLUME_OK)
            return 1;
        gstd_volume_handle_close(&reopened);
        input.role_fds[GSTD_MOUNT_REPOSITORIES] = -1;
        gstd_volume_handle_close(&volume_handle);
        gstd_volume_root_close(&volume_owner);
        close(volume_root_fd);
        for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
            if (input.role_fds[i] >= 0)
                close(input.role_fds[i]);
        if (joined) {
            if (gstd_network_worker_signal(&network_worker, 0))
                return 1;
            int status = 0, reaped = 0;
            for (unsigned attempt = 0; attempt < 500 && !reaped; attempt++) {
                reaped = waitpid(network_worker.pid, &status, WNOHANG) == network_worker.pid;
                if (!reaped) {
                    struct timespec delay = {0, 10000000};
                    nanosleep(&delay, NULL);
                }
            }
            if (!reaped || !WIFEXITED(status) || WEXITSTATUS(status))
                return 1;
            gstd_network_worker_observation_close(&network_observed);
            gstd_network_worker_close(&network_worker);
            puts("PASS: durable network attachment preceded CREATE; confined packet worker stopped "
                 "after workload");
        }
        json_decref(manifest);
        if (sqlite3_close(database) != SQLITE_OK)
            return 1;
    }
    json_decref(config);
    close(context.state_fd);
    close(context.bundle_fd);
    close(context.rootfs_fd);
    puts("PASS: daemon adapter create/start/observe/stale-identity rejection/TERM/delete and "
         "lost-start acknowledgement");
    return 0;
}
