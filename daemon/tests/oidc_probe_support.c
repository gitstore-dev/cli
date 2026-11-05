#define _GNU_SOURCE 1
/* Developer-only pinned-provider serving compatibility, never a daemon
 * method. Uses the existing native worker boundary and exact selected policy. */
#include "bootstrap_http.h"
#include "execution_plan.h"
#include "gitstored/execution.h"
#include "gitstored/protocol.h"
#include "helper.h"
#include "native_exit.h"
#include "native_job.h"
#include "native_population.h"
#include "oidc_config.h"
#include "oidc_enrollment.h"
#include "oidc_identity_schema.inc"
#include "oidc_material.h"
#include "oidc_profile.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <net/if.h>
#include <openssl/crypto.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "oidc_probe_support.h"
int64_t oidc_probe_now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void oidc_probe_pause_poll(void) {
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}
int oidc_probe_native(gstd_native_context *context, gstd_native_action action,
                      gstd_native_observation *observed) {
    gstd_native_job job = {0};
    if (gstd_native_launch(context, action, 15000, &job))
        return -1;
    gstd_native_status status;
    int done;
    while (!(done = gstd_native_poll(&job, -1, &status, observed)))
        oidc_probe_pause_poll();
    if (done != 1 || status != GSTD_NATIVE_OK)
        fprintf(stderr, "native action=%d status=%d error=%d\n", (int)action,
                done == 1 ? (int)status : -1, observed->native_error);
    return done == 1 && status == GSTD_NATIVE_OK ? 0 : -1;
}
int oidc_probe_record(int directory, const char *name, json_t *value) {
    char *bytes = value ? gstd_json_dump(value) : NULL;
    if (!bytes)
        return -1;
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    size_t length = strlen(bytes);
    int valid = fd >= 3 && write(fd, bytes, length) == (ssize_t)length && !fsync(fd);
    if (fd >= 3)
        close(fd);
    OPENSSL_cleanse(bytes, length);
    free(bytes);
    return valid && !fsync(directory) ? 0 : -1;
}
static int secret_file(int directory, const char *name, const gstd_credential *value) {
    if (!value || !value->bytes || !value->length || value->length > 4096)
        return -1;
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 3)
        return -1;
    int good = write(fd, value->bytes, value->length) == (ssize_t)value->length && !fsync(fd);
    close(fd);
    return good && !fsync(directory) ? 0 : -1;
}
static int stored_material(int directory, gstd_credential *output) {
    int fd = openat(directory, "material.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 1 : -1;
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
        before.st_nlink != 1 || (before.st_mode & 07777) != 0600 || before.st_size <= 0 ||
        before.st_size > 1024) {
        close(fd);
        return -1;
    }
    output->length = (size_t)before.st_size + 1;
    output->bytes = calloc(1, output->length);
    ssize_t length = output->bytes ? read(fd, output->bytes, output->length) : -1;
    int good = length == before.st_size && !fstat(fd, &after) && before.st_dev == after.st_dev &&
               before.st_ino == after.st_ino && before.st_size == after.st_size &&
               before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
               before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
               before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
               before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    close(fd);
    if (!good) {
        gstd_credential_clear(output);
        return -1;
    }
    output->length = (size_t)length;
    if (gstd_oidc_material_check(output)) {
        gstd_credential_clear(output);
        return -1;
    }
    return 0;
}
int oidc_probe_prepare(int directory, int material_directory, const char *role, int allow_initial) {
    if (strcmp(role, "hydra") && strcmp(role, "kratos") && strcmp(role, "ui"))
        return -1;
    pid_t parent = getpid(), child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        if (prctl(PR_SET_DUMPABLE, 0) || prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
            _exit(1);
        alarm(8);
        gstd_credential material = {0}, environment = {0};
        gstd_oidc_ports ports = {24001, 24002, 24003, 24004, 24005};
        int missing = stored_material(material_directory, &material);
        int bad =
            missing < 0 ||
            (missing == 1 && (!allow_initial || gstd_oidc_material_generate(parent, &material)));
        json_t *config = NULL;
        if (!bad && !strcmp(role, "ui"))
            bad = gstd_oidc_ui_environment(&ports, &material, &environment) ||
                  secret_file(directory, "ui.env", &environment);
        else if (!bad) {
            config = gstd_oidc_provider_config(
                !strcmp(role, "hydra") ? GSTD_OIDC_HYDRA : GSTD_OIDC_KRATOS, &ports, &material);
            bad = !config || oidc_probe_record(directory, "provider.json", config);
        }
        json_t *saved_material = bad ? NULL : gstd_json_parse(material.bytes, material.length);
        bad = bad || !saved_material ||
              (missing == 1 &&
               oidc_probe_record(material_directory, "material.json", saved_material));
        gstd_oidc_config_clear(saved_material);
        json_decref(saved_material);
        if (!bad && !strcmp(role, "kratos")) {
            json_t *schema = gstd_json_parse((const unsigned char *)oidc_identity_schema,
                                             sizeof(oidc_identity_schema) - 1);
            bad = !schema || oidc_probe_record(directory, "identity.schema.json", schema);
            json_decref(schema);
        }
        gstd_oidc_config_clear(config);
        json_decref(config);
        gstd_credential_clear(&material);
        gstd_credential_clear(&environment);
        _exit(bad ? 1 : 0);
    }
    int status;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status) ? 0
                                                                                            : -1;
}
static size_t discard(char *bytes, size_t size, size_t count, void *argument) {
    (void)bytes;
    size_t *total = argument;
    if (size && count > (4096 - *total) / size)
        return 0;
    *total += size * count;
    return size * count;
}
int oidc_probe_ready(pid_t pid, const char *component, const gstd_oidc_ports *ports) {
    int reports[2];
    if (pipe2(reports, O_CLOEXEC))
        return -1;
    pid_t child = fork();
    if (child < 0) {
        close(reports[0]);
        close(reports[1]);
        return -1;
    }
    if (!child) {
        close(reports[0]);
        alarm(5);
        char path[128];
        snprintf(path, sizeof(path), "/proc/%ld/ns/user", (long)pid);
        int user = open(path, O_RDONLY | O_CLOEXEC);
        snprintf(path, sizeof(path), "/proc/%ld/ns/net", (long)pid);
        int net = open(path, O_RDONLY | O_CLOEXEC);
        if (user < 3 || net < 3 || setns(user, CLONE_NEWUSER) || setns(net, CLONE_NEWNET))
            _exit(20);
        close(user);
        close(net);
        int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        struct ifreq link = {0};
        memcpy(link.ifr_name, "lo", 3);
        if (socket_fd < 0 || ioctl(socket_fd, SIOCGIFFLAGS, &link))
            _exit(1);
        link.ifr_flags |= IFF_UP;
        if (ioctl(socket_fd, SIOCSIFFLAGS, &link))
            _exit(1);
        close(socket_fd);
        struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
        struct __user_cap_data_struct caps[2] = {{0}};
        if (syscall(SYS_capset, &header, caps) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
            _exit(1);
        CURL *curl = curl_easy_init();
        long code = 0;
        size_t total = 0;
        if (!curl)
            _exit(1);
        char target[128];
        if (!strcmp(component, "hydra"))
            snprintf(target, sizeof(target), "http://127.0.0.1:4445/health/ready");
        else if (!strcmp(component, "kratos"))
            snprintf(target, sizeof(target), "http://127.0.0.1:4434/admin/health/ready");
        else if (!strcmp(component, "bridge"))
            snprintf(target, sizeof(target), "http://127.0.0.1:%u/health", ports->bridge);
        else if (!strcmp(component, "ui"))
            snprintf(target, sizeof(target), "http://127.0.0.1:%u/health/ready", ports->ui);
        else if (!strcmp(component, "smtp"))
            snprintf(target, sizeof(target), "http://127.0.0.1:%u/readyz", ports->mail);
        else
            _exit(1);
        curl_easy_setopt(curl, CURLOPT_URL, target);
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &total);
        CURLcode result = curl_easy_perform(curl);
        if (result == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(curl);
        int report = result == CURLE_OK ? (int)code : -(int)result;
        int saved = write(reports[1], &report, sizeof(report)) == sizeof(report);
        close(reports[1]);
        _exit(saved ? 0 : 1);
    }
    close(reports[1]);
    int status, report = -1;
    int waited = waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
    ssize_t length = read(reports[0], &report, sizeof(report));
    close(reports[0]);
    if (!waited || length != sizeof(report))
        return -1;
    return report == 200 ? 0 : report;
}
int oidc_probe_enrollment(int owner, int material_directory, int volume_directory, pid_t pid,
                          uint64_t start, const char *boot, int lose_reply) {
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    network.keeper_pid = pid;
    network.keeper_start = start;
    memcpy(network.boot_id, boot, 37);
    char path[128];
    struct stat info;
    snprintf(path, sizeof(path), "/proc/%ld/ns/user", (long)pid);
    network.user_fd = open(path, O_RDONLY | O_CLOEXEC);
    snprintf(path, sizeof(path), "/proc/%ld/ns/net", (long)pid);
    network.network_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (network.user_fd < 3 || network.network_fd < 3 || fstat(network.user_fd, &info))
        return -1;
    network.user_device = info.st_dev;
    network.user_inode = info.st_ino;
    if (fstat(network.network_fd, &info))
        return -1;
    network.network_device = info.st_dev;
    network.network_inode = info.st_ino;
    if (gstd_network_namespace_check(&network))
        return -1;
    int data = fcntl(material_directory, F_DUPFD_CLOEXEC, 3);
    if (data < 3)
        return -1;
    /* A durable receipt precedes this single POST. There is no retry or public
     * client adoption. A missing private reply remains dispatched/uncertain. */
    struct stat volume;
    if (volume_directory < 3 || fstat(volume_directory, &volume) || !S_ISDIR(volume.st_mode) ||
        volume.st_uid != geteuid() || (volume.st_mode & 07777) != 0700)
        return -1;
    char device[32], inode[32];
    snprintf(device, sizeof(device), "%llu", (unsigned long long)volume.st_dev);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)volume.st_ino);
    gstd_oidc_client client = {"gs-oidc-fixture-native", "gitstore",
                               "http://127.0.0.1:24004/callback"};
    json_t *binding = json_pack(
        "{s:s,s:s,s:s,s:s,s:s,s:s,s:i,s:s,s:s}", "storeId", "fixture-store", "deploymentId",
        "fixture-deployment", "operationId", "fixture-start", "providerResourceId", "fixture-hydra",
        "volumeProducerId", "fixture-import", "credentialRef", "fixture-protected-material",
        "generation", 1, "volumeDevice", device, "volumeInode", inode);
    json_t *prepared = gstd_oidc_enrollment_prepare(binding, &client);
    json_t *dispatch = prepared ? gstd_oidc_enrollment_dispatch(prepared) : NULL;
    int saved = !dispatch || oidc_probe_record(owner, "enrollment-prepared.json", prepared) ||
                oidc_probe_record(owner, "enrollment-dispatched.json", dispatch);
    json_decref(prepared);
    json_decref(binding);
    if (saved) {
        json_decref(dispatch);
        return -1;
    }
    int reports[2];
    if (pipe2(reports, O_CLOEXEC))
        return -1;
    pid_t parent = getpid(), child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        close(reports[0]);
        if (prctl(PR_SET_DUMPABLE, 0) || prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
            _exit(1);
        alarm(8);
        int fd = openat(data, "material.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat st;
        if (fd < 3 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            st.st_nlink != 1 || (st.st_mode & 07777) != 0600 || st.st_size <= 0 ||
            st.st_size > 1024)
            _exit(1);
        unsigned char bytes[1025] = {0};
        ssize_t length = read(fd, bytes, sizeof(bytes));
        close(fd);
        gstd_credential material = {bytes, length > 0 ? (size_t)length : 0};
        if (length != st.st_size || gstd_oidc_material_check(&material))
            _exit(1);
        json_t *document = gstd_json_parse(bytes, (size_t)length);
        const char *secret = json_string_value(json_object_get(document, "clientSecret"));
        gstd_credential client_secret = {(unsigned char *)secret, secret ? strlen(secret) : 0};
        gstd_bootstrap_http_context context = {.control_owner = parent, .network = &network};
        int status = gstd_oidc_http(&context, GSTD_OIDC_HTTP_CREATE, &client, &client_secret);
        gstd_oidc_config_clear(document);
        json_decref(document);
        OPENSSL_cleanse(bytes, sizeof(bytes));
        if (lose_reply && status == GSTD_OIDC_CLIENT_CREATED) {
            close(reports[1]);
            _exit(0);
        }
        int sent = write(reports[1], &status, sizeof(status)) == sizeof(status);
        close(reports[1]);
        _exit(sent ? 0 : 1);
    }
    close(reports[1]);
    int status, outcome = GSTD_OIDC_UNCERTAIN;
    int waited = waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
    ssize_t length = read(reports[0], &outcome, sizeof(outcome));
    close(reports[0]);
    close(data);
    close(network.user_fd);
    close(network.network_fd);
    if (!waited || length != sizeof(outcome))
        outcome = GSTD_OIDC_UNCERTAIN;
    json_t *observed = gstd_oidc_enrollment_observe(dispatch, (gstd_oidc_http_status)outcome);
    int persisted = observed ? oidc_probe_record(owner, "enrollment-observed.json", observed) : -1;
    json_decref(observed);
    json_decref(dispatch);
    json_t *ack =
        json_pack("{s:i,s:b}", "status", outcome, "created", outcome == GSTD_OIDC_CLIENT_CREATED);
    int result = oidc_probe_record(owner, "enrollment-result.json", ack);
    json_decref(ack);
    if (result || persisted)
        return -1;
    return outcome == GSTD_OIDC_CLIENT_CREATED ? 0 : outcome == GSTD_OIDC_UNCERTAIN ? 1 : -1;
}
static int adapter_rejects(const gstd_execution_inputs *input) {
    json_t *value = gstd_execution_build(input);
    int rejected = value == NULL;
    json_decref(value);
    return rejected;
}
int oidc_probe_adapter_policy_checks(const gstd_execution_inputs *input) {
    gstd_execution_inputs changed = *input;
    changed.purpose = GSTD_EXEC_SCHEMA_MIGRATE;
    if (!adapter_rejects(&changed))
        return -1;
    changed = *input;
    changed.role_fds[GSTD_MOUNT_DATASTORE] = input->rootfs_fd;
    if (!adapter_rejects(&changed))
        return -1;
    changed = *input;
    changed.role_fds[GSTD_MOUNT_OVERLAY] = input->role_fds[GSTD_MOUNT_RESOLVER];
    if (!adapter_rejects(&changed))
        return -1;
    changed = *input;
    gstd_oidc_ports ports = *input->oidc_ports;
    ports.issuer = 4445;
    changed.oidc_ports = &ports;
    if (!adapter_rejects(&changed))
        return -1;
    changed = *input;
    gstd_network_namespace network = *input->network;
    network.network_inode++;
    changed.network = &network;
    if (!adapter_rejects(&changed))
        return -1;
    changed = *input;
    changed.purpose = GSTD_EXEC_OIDC_MIGRATE;
    changed.role_fds[GSTD_MOUNT_OVERLAY] = -1;
    changed.role_fds[GSTD_MOUNT_POLICY] = -1;
    json_t *migration = gstd_execution_build(&changed);
    int sql = !strcmp(input->component, "oidc-hydra") || !strcmp(input->component, "oidc-kratos");
    int good = sql ? (migration != NULL) : (migration == NULL);
    json_decref(migration);
    return good ? 0 : -1;
}
int oidc_probe_migrate_provider(int owner, int native_state, int rootfs, int log, int lease,
                                const char *id, const char *boot,
                                const gstd_execution_inputs *service) {
    if (mkdirat(owner, "migration-bundle", 0700))
        return -1;
    int bundle = openat(owner, "migration-bundle", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (bundle < 3)
        return -1;
    char migration_id[65];
    int length = snprintf(migration_id, sizeof(migration_id), "%s-migrate", id);
    if (length <= 0 || length >= 65)
        return -1;
    gstd_execution_inputs input = *service;
    input.purpose = GSTD_EXEC_OIDC_MIGRATE;
    input.role_fds[GSTD_MOUNT_OVERLAY] = -1;
    input.role_fds[GSTD_MOUNT_POLICY] = -1;
    json_t *configuration = gstd_execution_build(&input);
    if (!configuration || oidc_probe_record(bundle, "config.json", configuration))
        return -1;
    gstd_native_context context = {.backend_id = migration_id,
                                   .component = service->component,
                                   .boot_id = boot,
                                   .state_fd = native_state,
                                   .bundle_fd = bundle,
                                   .rootfs_fd = rootfs,
                                   .stdout_fd = log,
                                   .stderr_fd = log,
                                   .writer_lease_fd = lease,
                                   .control_owner = getpid(),
                                   .configuration = configuration,
                                   .registered_inputs = &input};
    json_t *dispatch = json_pack(
        "{s:s,s:s,s:s}", "id", migration_id, "purpose", "fixed-provider-sqlite-migration",
        "profileDigest", json_string_value(json_object_get(service->manifest, "profileDigest")));
    int saved = gstd_helper_validate(&context) ||
                oidc_probe_record(owner, "migration-dispatched.json", dispatch);
    json_decref(dispatch);
    if (saved)
        return -1;
    gstd_helper helper = {0};
    if (gstd_helper_begin(&context, &helper))
        return -1;
    int status;
    while ((status = gstd_helper_poll(&helper, -1)) != 2) {
        if (status < 0)
            return -1;
        if (status == 1) {
            json_t *population =
                gstd_native_population_identity(&helper.population, migration_id, boot);
            json_t *created =
                json_pack("{s:s,s:i,s:I,s:O}", "id", migration_id, "pid",
                          (int)helper.context.expected_pid, "startTime",
                          (json_int_t)helper.context.expected_start_time, "population", population);
            int failed = !created || oidc_probe_record(owner, "migration-created.json", created);
            json_decref(created);
            json_decref(population);
            if (failed || gstd_helper_permit_start(&helper))
                return -1;
        }
        oidc_probe_pause_poll();
    }
    int good = helper.outcome == 0 && helper.deleted && !helper.may_exist &&
               gstd_native_watch_poll(helper.death_fd) == 1 &&
               gstd_native_population_empty(&helper.population) == 1;
    json_t *result = json_pack("{s:i,s:b,s:b}", "outcome", helper.outcome, "deleted",
                               helper.deleted, "populationEmpty", good);
    saved = !result || oidc_probe_record(owner, "migration-completed.json", result);
    json_decref(result);
    if (!good || saved || gstd_helper_close(&helper))
        return -1;
    json_decref(configuration);
    close(bundle);
    return 0;
}
static json_t *public_receipt(int directory, const char *name) {
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 3)
        return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 07777) != 0600 || st.st_size <= 0 || st.st_size > 8192) {
        close(fd);
        return NULL;
    }
    unsigned char bytes[8193];
    ssize_t length = read(fd, bytes, sizeof(bytes));
    close(fd);
    return length == st.st_size ? gstd_json_parse(bytes, (size_t)length) : NULL;
}
/* Fixed developer-only recovery of the retained UI signal test. No caller PID,
 * signal or component selection. Bind the original runtime tuple, held pidfd
 * and recorded cgroup inode pair before KILL, then observe death/emptiness before
 * non-forced DELETE. Unknown outcomes retain the entire fixture. */
int oidc_probe_recover_ui_stop(const char *path) {
    struct rlimit core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &core) || prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0))
        return 1;
    int root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (root < 3 || fstat(root, &st) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0700)
        return 1;
    json_t *created = public_receipt(root, "created.json");
    const char *id = json_string_value(json_object_get(created, "id")),
               *boot = json_string_value(json_object_get(created, "boot"));
    json_int_t pid = json_integer_value(json_object_get(created, "pid")),
               start = json_integer_value(json_object_get(created, "startTime"));
    if (!created || json_object_size(created) != 5 || !id || strncmp(id, "oidc-sqlite-ui-", 15) ||
        !gstd_identifier(id, strlen(id)) || !boot || strlen(boot) != 36 || pid <= 0 ||
        pid > INT32_MAX || start <= 0)
        return 1;
    int state = openat(root, "native", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
        bundle = openat(root, "bundle", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
        execution = openat(root, "execution", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
        image = execution < 3
                    ? -1
                    : openat(execution, "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int death = -1;
    gstd_native_population population = GSTD_NATIVE_POPULATION_INITIAL;
    if (state < 3 || bundle < 3 || image < 3 ||
        gstd_native_watch_open((pid_t)pid, (uint64_t)start, boot, &death) ||
        gstd_native_population_reopen(json_object_get(created, "population"), id, boot,
                                      &population) ||
        gstd_native_population_matches(&population, id, (pid_t)pid, (uint64_t)start, boot) ||
        gstd_native_watch_poll(death) != 0)
        return 1;
    gstd_native_context context = {.backend_id = id,
                                   .component = "oidc-ui",
                                   .boot_id = boot,
                                   .state_fd = state,
                                   .bundle_fd = bundle,
                                   .rootfs_fd = image,
                                   .stdout_fd = -1,
                                   .stderr_fd = -1,
                                   .writer_lease_fd = -1,
                                   .control_owner = getpid(),
                                   .expected_pid = (pid_t)pid,
                                   .expected_start_time = (uint64_t)start};
    gstd_native_observation observed;
    if (oidc_probe_native(&context, GSTD_NATIVE_OBSERVE, &observed) || !observed.running)
        return 1;
    json_t *intent =
        json_pack("{s:s,s:O}", "action", "fixed-ui-kill-after-term-timeout", "original", created);
    int saved = oidc_probe_record(root, "recovery-stop-intent.json", intent);
    json_decref(intent);
    if (saved || oidc_probe_native(&context, GSTD_NATIVE_KILL, &observed))
        return 1;
    int64_t deadline = oidc_probe_now_ms() + 10000;
    while (oidc_probe_now_ms() < deadline &&
           (gstd_native_watch_poll(death) != 1 || gstd_native_population_empty(&population) != 1))
        oidc_probe_pause_poll();
    if (gstd_native_watch_poll(death) != 1 || gstd_native_population_empty(&population) != 1 ||
        oidc_probe_native(&context, GSTD_NATIVE_DELETE, &observed))
        return 1;
    json_t *done = json_pack("{s:b,s:b,s:b}", "originalPidfdDead", 1, "originalCgroupEmpty", 1,
                             "nativeDeleted", 1);
    saved = oidc_probe_record(root, "recovered-cleanup.json", done);
    json_decref(done);
    gstd_native_population_close(&population);
    close(death);
    close(image);
    close(execution);
    close(bundle);
    close(state);
    close(root);
    json_decref(created);
    if (saved)
        return 1;
    puts("{\"originalPidfdDead\":true,\"originalCgroupEmpty\":true,\"nativeDeleted\":true}");
    return 0;
}

/* Maintainer-only local Postman execution. No product CLI dependency, host admin
 * forwarding, ambient credentials, proxy or cloud reporting. Responses are not
 * exported; the protected runner log contains only normal assertion output. */
int oidc_probe_courier(int owner, const char *root, const gstd_network_namespace *network,
                       const char *postman, const char *collection) {
    struct stat executable, input;
    if (gstd_network_namespace_check(network) || root[0] != '/' || postman[0] != '/' ||
        collection[0] != '/' || lstat(postman, &executable) || !S_ISREG(executable.st_mode) ||
        executable.st_uid != geteuid() || (executable.st_mode & 0022) || stat(collection, &input) ||
        !S_ISDIR(input.st_mode))
        return -1;
    json_t *values = json_array();
    const char *keys[] = {"ownedDisposableOidcFixture",
                          "identityAdminUrl",
                          "identityPublicUrl",
                          "mailboxUrl",
                          "fixtureEmail",
                          "fixtureUsername"};
    const char *data[] = {"true",
                          "http://127.0.0.1:4434",
                          "http://127.0.0.1:24003",
                          "http://127.0.0.1:24005",
                          "gitstore-courier@example.invalid",
                          "gitstore-oidc-fixture-courier"};
    if (!values)
        return -1;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        json_t *entry = json_pack("{s:s,s:s,s:b,s:s}", "key", keys[i], "value", data[i], "enabled",
                                  1, "type", "default");
        if (!entry || json_array_append_new(values, entry)) {
            json_decref(values);
            return -1;
        }
    }
    json_t *environment =
        json_pack("{s:s,s:o}", "name", "Owned disposable OIDC courier", "values", values);
    int saved = !environment || oidc_probe_record(owner, "courier-environment.json", environment);
    json_decref(environment);
    if (saved || mkdirat(owner, "postman-home", 0700))
        return -1;
    int log = openat(owner, "courier-postman.log",
                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log < 3)
        return -1;
    char environment_path[4096], home[4200];
    if (snprintf(environment_path, sizeof(environment_path), "%s/courier-environment.json", root) >=
            (int)sizeof(environment_path) ||
        snprintf(home, sizeof(home), "HOME=%s/postman-home", root) >= (int)sizeof(home)) {
        close(log);
        return -1;
    }
    pid_t parent = getpid(), child = fork();
    if (child < 0) {
        close(log);
        return -1;
    }
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || setpgid(0, 0) ||
            setns(network->user_fd, CLONE_NEWUSER) || setns(network->network_fd, CLONE_NEWNET))
            _exit(20);
        struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
        struct __user_cap_data_struct caps[2] = {{0}};
        struct rlimit memory = {UINT64_C(1) << 40, UINT64_C(1) << 40};
        struct rlimit descriptors = {1024, 1024};
        if (syscall(SYS_capset, &header, caps) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
            setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_NOFILE, &descriptors) ||
            dup2(log, STDOUT_FILENO) != STDOUT_FILENO || dup2(log, STDERR_FILENO) != STDERR_FILENO)
            _exit(21);
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null < 3 || dup2(null, STDIN_FILENO) != STDIN_FILENO)
            _exit(22);
        if (syscall(SYS_close_range, 3U, ~0U, 0))
            _exit(23);
        alarm(80);
        char *args[] = {(char *)postman,
                        "collection",
                        "run",
                        (char *)collection,
                        "-e",
                        environment_path,
                        "--no-report-events",
                        "--ignore-redirects",
                        "--delay-request",
                        "1000",
                        "--timeout",
                        "60000",
                        "--timeout-request",
                        "5000",
                        "--timeout-script",
                        "5000",
                        "--bail",
                        NULL};
        char *env[] = {home, "PATH=/nonexistent", "NO_COLOR=1", "TERM=dumb", NULL};
        execve(postman, args, env);
        _exit(24);
    }
    close(log);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    int passed = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    json_t *report =
        json_pack("{s:b,s:b,s:i,s:b}", "ownedNamespace", 1, "passed", passed, "exitCode",
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1, "responsesExported", 0);
    saved = !report || oidc_probe_record(owner, "courier-result.json", report);
    json_decref(report);
    return !saved && passed ? 0 : -1;
}
