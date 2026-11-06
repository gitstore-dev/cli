#define _GNU_SOURCE 1
#include "runtime_manager.h"
#include "cancellation.h"
#include "cleanup.h"
#include "gitstored/assets.h"
#include "gitstored/execution.h"
#include "gitstored/operations.h"
#include "gitstored/profiles.h"
#include "gitstored/scheduler.h"
#include "image_backend.h"
#include "migration.h"
#include "migration_recovery.h"
#include "migration_runtime.h"
#include "monitor.h"
#include "oidc_profile.h"
#include "recovery.h"
#include "restart.h"
#include "restoration.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#include <sys/prctl.h>
#endif
#define LIVE_LIMIT 32
struct gstd_runtime_manager {
    sqlite3 *database;
    int state_fd, capable, roots_state;
    gstd_deployment_environment environment;
    gstd_filesystem_job initialize;
    gstd_filesystem_reply roots;
    struct {
        gstd_deployment deployment;
        gstd_cleanup cleanup;
        gstd_restoration restoration;
        char recovery_stop[65], stop_waiting[65];
        int start_finished, stop_finished, cancellation_fd, restart_notified;
        char cancel_operation[65];
    } slots[LIVE_LIMIT];
    gstd_migration_recovery migration_recovery[LIVE_LIMIT];
    gstd_migration_runtime migrations[LIVE_LIMIT];
    gstd_restart restarts[LIVE_LIMIT];
};
static int restart_submit(void *context, gstd_method method, json_t *params, const char *key,
                          char operation[65]) {
    gstd_request request = {
        .method = method, .params = params, .request_id = "owned-restart", .idempotency_key = key};
    int error = 1;
    json_t *result = gstd_runtime_manager_submit(context, &request, &error);
    const char *id = result ? json_string_value(json_object_get(result, "operationId")) : NULL;
    int valid = !error && id && gstd_identifier(id, strlen(id));
    if (valid)
        memcpy(operation, id, strlen(id) + 1);
    json_decref(result);
    return valid ? 0 : -1;
}
static void departed(gstd_runtime_manager *m, unsigned slot) {
    gstd_deployment *d = &m->slots[slot].deployment;
    if (m->slots[slot].restart_notified || !d->monitor.exit_observed ||
        d->monitor.observation_failed || d->recovered)
        return;
    unsigned selected = LIVE_LIMIT, available = LIVE_LIMIT;
    for (unsigned i = 0; i < LIVE_LIMIT; i++) {
        if (m->restarts[i].phase && !strcmp(m->restarts[i].current, d->target.operation_id)) {
            selected = i;
            break;
        }
        if (!m->restarts[i].phase || m->restarts[i].phase == 5)
            available = i;
    }
    if (selected == LIVE_LIMIT && available != LIVE_LIMIT) {
        selected = available;
        gstd_restart_clear(&m->restarts[selected]);
    }
    if (selected != LIVE_LIMIT)
        (void)gstd_restart_departed(m->database, d->target.operation_id, &m->restarts[selected],
                                    restart_submit, m);
    /* No implicit re-acceptance after an admission failure or uncertain state.
     * An operator can inspect the failed readiness and submit explicit stop. */
    m->slots[slot].restart_notified = 1;
}
static const char *architecture(void) {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#else
    return "amd64";
#endif
}
static json_t *error_value(const char *code) {
    return json_pack("{s:s,s:s,s:b,s:s}", "code", code, "stage", "runtime", "retryable", 0,
                     "message",
                     "Local lifecycle request could not be accepted; inspect the selected "
                     "deployment and host prerequisites");
}
static int installed_file(int fd, struct stat *st) {
    return fd >= 3 && !fstat(fd, st) && S_ISREG(st->st_mode) && st->st_nlink == 1 &&
           (st->st_uid == 0 || st->st_uid == geteuid()) && !(st->st_mode & 0022) &&
           (st->st_mode & 0111);
}
static int sibling(const char *entrypoint) {
#ifdef __linux__
    if (!entrypoint || entrypoint[0] != '/' || strlen(entrypoint) > 4095)
        return -1;
    char path[4096];
    memcpy(path, entrypoint, strlen(entrypoint) + 1);
    char *slash = strrchr(path, '/');
    if (!slash || !slash[1])
        return -1;
    const char *leaf = slash + 1;
    *slash = 0;
    int directory = open(*path ? path : "/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 3)
        return -1;
    struct stat parent, original, after, current, broker;
    int named = openat(directory, leaf, O_RDONLY | O_CLOEXEC | O_NOFOLLOW),
        self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int valid = !fstat(directory, &parent) && (parent.st_uid == 0 || parent.st_uid == geteuid()) &&
                !(parent.st_mode & 0022) && installed_file(named, &original) && self >= 3 &&
                !fstat(self, &current) && original.st_size > 0 &&
                original.st_size == current.st_size;
    unsigned char first[65536], second[65536];
    for (off_t offset = 0; valid && offset < original.st_size;) {
        size_t amount = (size_t)(original.st_size - offset);
        if (amount > sizeof(first))
            amount = sizeof(first);
        valid = pread(named, first, amount, offset) == (ssize_t)amount &&
                pread(self, second, amount, offset) == (ssize_t)amount &&
                !memcmp(first, second, amount);
        offset += (off_t)amount;
    }
    valid = valid && !fstat(named, &after) && original.st_dev == after.st_dev &&
            original.st_ino == after.st_ino && original.st_size == after.st_size &&
            original.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            original.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
            original.st_ctim.tv_sec == after.st_ctim.tv_sec &&
            original.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    if (named >= 3)
        close(named);
    if (self >= 3)
        close(self);
    int fd = valid ? openat(directory, "gs", O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
    close(directory);
    unsigned char magic[4];
    if (fd >= 3 && (!installed_file(fd, &broker) || pread(fd, magic, 4, 0) != 4 ||
                    memcmp(magic, "\177ELF", 4))) {
        close(fd);
        fd = -1;
    }
    return fd;
#else
    (void)entrypoint;
    return -1;
#endif
}
gstd_runtime_manager *gstd_runtime_manager_open(sqlite3 *db, int state,
                                                const gstd_runtime_settings *settings,
                                                const char *entrypoint) {
    if (!db || state < 3 || !settings)
        return NULL;
    gstd_runtime_manager *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    if (gstd_schedule_read_recover(db)) {
        free(m);
        return NULL;
    }
    m->database = db;
    m->state_fd = state;
    m->environment = (gstd_deployment_environment){.database = db,
                                                   .state_fd = state,
                                                   .settings = settings,
                                                   .cache_fd = -1,
                                                   .resource_fd = -1,
                                                   .native_fd = -1,
                                                   .broker_fd = -1,
                                                   .volume_root = {.root_fd = -1, .lock_fd = -1}};
#ifdef __linux__
    if (geteuid() && gstd_native_backend_available() && gstd_image_backend_available() &&
        gstd_network_engine_available()) {
        m->environment.broker_fd = sibling(entrypoint);
        struct rlimit core = {0, 0};
        if (m->environment.broker_fd < 3)
            fputs("gitstored: installed credential broker unavailable\n", stderr);
        else if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) || setrlimit(RLIMIT_CORE, &core))
            fputs("gitstored: native parent prerequisites unavailable\n", stderr);
        else if (gstd_recovery_invalidate(db))
            fputs("gitstored: runtime journal recovery unavailable\n", stderr);
        else
            m->capable = 1;
    }
#else
    (void)sibling;
    (void)entrypoint;
    (void)installed_file;
    (void)gstd_recovery_invalidate;
#endif
    return m;
}
int gstd_runtime_manager_observation_blocked(const gstd_runtime_manager *m, const char *deployment,
                                             const char *operation) {
    for (unsigned i = 0; m && i < LIVE_LIMIT; i++)
        if (gstd_deployment_monitor_observation_blocked(&m->slots[i].deployment, deployment,
                                                        operation))
            return 1;
    return 0;
}
int gstd_runtime_manager_capable(const gstd_runtime_manager *m) { return m && m->capable; }
int gstd_runtime_manager_assess(gstd_runtime_manager *m, json_t *assessment) {
    if (!m || !assessment)
        return -1;
    json_t *assets = json_object_get(assessment, "requiredAssets");
    for (size_t i = 0; i < json_array_size(assets); i++) {
        json_t *asset = json_array_get(assets, i);
        const char *name = json_string_value(json_object_get(asset, "component"));
        const json_t *full =
            json_object_get(json_object_get(assessment, "effectivePlan"), "assetManifest");
        json_t *manifest = name ? gstd_asset_manifest_component(full, name) : NULL;
        char key[65];
        int valid = manifest && !gstd_fingerprint(manifest, key);
        json_decref(manifest);
        if (!valid)
            return -1;
        sqlite3_stmt *s = NULL;
        int cached = 0;
        if (sqlite3_prepare_v2(m->database,
                               "SELECT 1 FROM assets WHERE asset_key=? AND state='active'", -1, &s,
                               NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        cached = step == SQLITE_ROW;
        sqlite3_finalize(s);
        if (!cached && step != SQLITE_DONE)
            return -1;
        int offline = gstd_runtime_settings_image(m->environment.settings, name) >= 3;
        if (json_object_set_new(asset, "cacheCandidate", json_boolean(cached)) ||
            json_object_set_new(asset, "offlineInput", json_boolean(offline)) ||
            json_object_set_new(asset, "requiresDownload", json_boolean(!cached && !offline)) ||
            json_object_set_new(asset, "downloadLimitBytes",
                                json_integer((!strcmp(name, "scylladb") || gstd_oidc_component(name)
                                                  ? UINT64_C(512)
                                                  : UINT64_C(128)) *
                                             1024 * 1024)) ||
            json_object_set_new(asset, "mergeLimitBytes",
                                json_integer((gstd_oidc_component(name)   ? UINT64_C(2048)
                                              : !strcmp(name, "scylladb") ? UINT64_C(1024)
                                                                          : UINT64_C(512)) *
                                             1024 * 1024)))
            return -1;
    }
    return 0;
}
static int profile(gstd_runtime_manager *m, const char *version) {
    json_t *components = gstd_profile_components_for(version, NULL),
           *manifest = components
                           ? gstd_profile_manifest(version, components, architecture(), "memdb")
                           : NULL;
    json_decref(components);
    if (!manifest)
        return -1;
    sqlite3_stmt *s = NULL;
    int ok = sqlite3_prepare_v2(m->database,
                                "INSERT INTO profiles VALUES('core',?,?,?,?) ON "
                                "CONFLICT(profile_id,profile_version) DO NOTHING",
                                -1, &s, NULL) == SQLITE_OK;
    char platform[32];
    snprintf(platform, sizeof(platform), "linux-%s", architecture());
    if (ok) {
        sqlite3_bind_text(s, 1, version, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, json_string_value(json_object_get(manifest, "serverRevision")), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, json_string_value(json_object_get(manifest, "profileDigest")), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, platform, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    json_decref(manifest);
    return ok ? 0 : -1;
}
static int consent(gstd_runtime_manager *m, const json_t *manifest, const json_t *params) {
    if (json_is_true(json_object_get(params, "allowProvision")))
        return 1;
    const json_t *images = json_object_get(manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *name =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (gstd_runtime_settings_image(m->environment.settings, name) >= 3)
            continue;
        json_t *single = gstd_asset_manifest_component(manifest, name);
        char key[65];
        int valid = single && !gstd_fingerprint(single, key);
        json_decref(single);
        if (!valid)
            return -1;
        sqlite3_stmt *s = NULL;
        valid = sqlite3_prepare_v2(m->database,
                                   "SELECT 1 FROM assets WHERE asset_key=? AND state='active'", -1,
                                   &s, NULL) == SQLITE_OK;
        if (valid) {
            sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
            valid = sqlite3_step(s) == SQLITE_ROW;
        }
        sqlite3_finalize(s);
        if (!valid)
            return 0;
    }
    return 1;
}
json_t *gstd_runtime_manager_submit(gstd_runtime_manager *m, const gstd_request *request,
                                    int *error) {
    if (!error)
        return NULL;
    *error = 1;
    if (!m || !m->capable)
        return error_value("UNSUPPORTED_CAPABILITY");
    if (request->method != GSTD_START && request->method != GSTD_STOP &&
        request->method != GSTD_CANCEL && request->method != GSTD_MIGRATE)
        return error_value("UNSUPPORTED_CAPABILITY");
    json_t *components = NULL;
    char volume[65] = {0};
    if (request->method == GSTD_START) {
        const char *id = json_string_value(json_object_get(request->params, "profileId")),
                   *version = json_string_value(json_object_get(request->params, "profileVersion")),
                   *datastore = json_string_value(json_object_get(request->params, "datastore"));
        if (!id || strcmp(id, "core") || !version || (!gstd_profile_digest_for(version)) ||
            (datastore && strcmp(datastore, "memdb") && strcmp(datastore, "scylladb")))
            return error_value("INVALID_ARGUMENT");
        components =
            gstd_profile_components_for(version, json_object_get(request->params, "components"));
        if (!components)
            return error_value("INVALID_ARGUMENT");
        json_t *selected_manifest = gstd_profile_manifest(version, components, architecture(),
                                                          datastore ? datastore : "memdb");
        if (!selected_manifest) {
            json_decref(components);
            return error_value("INVALID_ARGUMENT");
        }
        json_t *approved = json_object_get(request->params, "assetManifestDigest");
        if (approved &&
            !json_equal(approved, json_object_get(selected_manifest, "manifestDigest"))) {
            json_decref(selected_manifest);
            json_decref(components);
            return error_value("IDEMPOTENCY_CONFLICT");
        }
        if (!gstd_execution_kernel_supported()) {
            json_decref(selected_manifest);
            json_decref(components);
            return error_value("UNSUPPORTED_HOST");
        }
        int permission = consent(m, selected_manifest, request->params);
        json_decref(selected_manifest);
        if (permission != 1) {
            json_decref(components);
            return error_value(permission ? "INTERNAL" : "PROVISIONING_REQUIRED");
        }
        json_t *identity =
            json_pack("{s:O}", "deploymentId", json_object_get(request->params, "deploymentId"));
        char hash[65];
        int ok = identity && !gstd_fingerprint(identity, hash);
        json_decref(identity);
        if (!ok || profile(m, version)) {
            json_decref(components);
            return error_value("INTERNAL");
        }
        int core = 0;
        for (size_t i = 0; i < json_array_size(components); i++) {
            const char *name = json_string_value(json_array_get(components, i));
            core |= name && strcmp(name, "oidc");
        }
        if (core)
            snprintf(volume, sizeof(volume), "repo-%.56s", hash);
    }
    char operation[65];
    gstd_accept_status accepted =
        gstd_accept(m->database, request, components, *volume ? volume : NULL, operation);
    json_decref(components);
    if (accepted != GSTD_ACCEPT_NEW && accepted != GSTD_ACCEPT_REPLAY) {
        const char *code =
            accepted == GSTD_ACCEPT_CONFLICT                 ? "IDEMPOTENCY_CONFLICT"
            : accepted == GSTD_ACCEPT_DEPLOYMENT_CONFLICT    ? "DEPLOYMENT_CONFLICT"
            : accepted == GSTD_ACCEPT_OWNERSHIP_UNCERTAIN    ? "OWNERSHIP_UNCERTAIN"
            : accepted == GSTD_ACCEPT_MIGRATION_PRECONDITION ? "MIGRATION_PRECONDITION"
            : accepted == GSTD_ACCEPT_MIGRATION_CONFLICT     ? "MIGRATION_CONFLICT"
            : accepted == GSTD_ACCEPT_UNSUPPORTED_DATASTORE  ? "UNSUPPORTED_CAPABILITY"
            : accepted == GSTD_ACCEPT_LIMIT                  ? "RESOURCE_LIMIT"
            : accepted == GSTD_ACCEPT_NOT_FOUND              ? "NOT_FOUND"
            : accepted == GSTD_ACCEPT_INVALID                ? "INVALID_ARGUMENT"
                                                             : "INTERNAL";
        return error_value(code);
    }
    sqlite3_stmt *receipt = NULL;
    json_t *result = NULL;
    if (sqlite3_prepare_v2(m->database,
                           "SELECT state,deployment_id FROM operations WHERE operation_id=?", -1,
                           &receipt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(receipt, 1, operation, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(receipt) == SQLITE_ROW)
            result = json_pack("{s:s,s:s,s:s,s:b}", "operationId", operation, "state",
                               (const char *)sqlite3_column_text(receipt, 0), "deploymentId",
                               (const char *)sqlite3_column_text(receipt, 1), "replayed",
                               accepted == GSTD_ACCEPT_REPLAY);
    }
    sqlite3_finalize(receipt);
    if (!result)
        return error_value("INTERNAL");
    *error = 0;
    return result;
}
static int already_stopped(gstd_runtime_manager *m, const char *operation) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            m->database,
            "UPDATE operations SET "
            "state='succeeded',result='{\"stopped\":true,\"alreadyStopped\":true}',"
            "updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE operation_id=? AND "
            "method='deployment.stop' "
            "AND state='running' AND EXISTS(SELECT 1 FROM deployments d JOIN operations prior "
            "ON prior.operation_id=d.last_operation_id WHERE "
            "d.deployment_id=operations.deployment_id "
            "AND d.desired_state='stopped' AND d.generation=d.observed_generation AND "
            "d.recovery='normal' "
            "AND d.writer_ownership='released' AND ((prior.method IN "
            "('deployment.stop','operation.cancel') AND "
            "prior.state='succeeded') OR (prior.method='migration.start' AND "
            "prior.state IN ('succeeded','failed') AND prior.recovery='normal') OR "
            "(prior.method='deployment.start' AND "
            "prior.state='cancelled')) "
            "AND NOT EXISTS(SELECT 1 FROM resource_ownership r WHERE "
            "r.deployment_id=d.deployment_id "
            "AND r.ownership_state!='released'))",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    int done = sqlite3_step(s) == SQLITE_DONE, changed = sqlite3_changes(m->database);
    sqlite3_finalize(s);
    if (!done)
        return -1;
    return changed ? (gstd_schedule_finish(m->database, operation) ? -1 : 1) : 0;
}
static int failed(gstd_runtime_manager *m, const char *operation, const char *code) {
    sqlite3 *db = m->database;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    json_t *value = error_value(code);
    char *bytes = value ? gstd_json_dump(value) : NULL;
    json_decref(value);
    sqlite3_stmt *s = NULL;
    int ok = bytes && sqlite3_prepare_v2(
                          db,
                          "UPDATE operations SET "
                          "state='failed',error=?,recovery=CASE WHEN method='migration.start' THEN "
                          "'uncertain' ELSE recovery END,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','"
                          "now') WHERE operation_id=? AND state IN ('accepted','running')",
                          -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, bytes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    free(bytes);
    if (ok) {
        ok = sqlite3_prepare_v2(
                 db,
                 "UPDATE resource_ownership SET ownership_state='uncertain' WHERE operation_id=? "
                 "AND ownership_state='held' AND EXISTS(SELECT 1 FROM operations o WHERE "
                 "o.operation_id=resource_ownership.operation_id AND o.method='migration.start')",
                 -1, &s, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(s) == SQLITE_DONE;
        }
        sqlite3_finalize(s);
    }
    if (ok) {
        ok = sqlite3_prepare_v2(
                 db,
                 "UPDATE deployments SET "
                 "readiness='failed',recovery='uncertain',observed_at=strftime('%Y-%"
                 "m-%dT%H:%M:%SZ','now') WHERE last_operation_id=? OR EXISTS(SELECT 1 "
                 "FROM operations o WHERE o.operation_id=? AND "
                 "o.deployment_id=deployments.deployment_id AND o.method='migration.start')",
                 -1, &s, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(s) == SQLITE_DONE;
        }
        sqlite3_finalize(s);
    }
    if (!ok || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return gstd_schedule_finish(db, operation);
}
/* A begin rejection before even the configuration reservation commits has no
 * external work. Finish that exact control-only preflight as a known failure;
 * never strand it as an uncertain native migration. */
static int migration_unstarted_failed(gstd_runtime_manager *m, const char *operation) {
    sqlite3 *db = m->database;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    sqlite3_stmt *s = NULL;
    int valid = sqlite3_prepare_v2(
                    db,
                    "SELECT 1 FROM resource_ownership WHERE operation_id=? AND "
                    "resource_id='migration-'||operation_id AND resource_type='configuration' "
                    "AND ownership_state='held' AND lifecycle_stage='reserved' AND "
                    "(SELECT count(*) FROM resource_ownership WHERE operation_id=?)=1",
                    -1, &s, NULL) == SQLITE_OK;
    if (valid) {
        sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
        valid = sqlite3_step(s) == SQLITE_ROW;
    }
    sqlite3_finalize(s);
    valid = valid && gstd_migration_cancel_safe(db, operation) == 1 &&
            !gstd_migration_finish(db, operation, 0, 1);
    if (!valid || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return gstd_schedule_finish(db, operation);
}
static int roots(gstd_runtime_manager *m, int existing) {
    if (m->roots_state == 2)
        return 1;
    if (m->roots_state == -1)
        return -1;
    if (!m->roots_state) {
        gstd_cache_owner expected;
        int cache_state = gstd_asset_record_cache_state(m->database, &expected);
        if (cache_state < 0 || (existing && cache_state != 0)) {
            m->roots_state = -1;
            return -1;
        }
        gstd_filesystem_input input = {.action =
                                           existing ? GSTD_FS_REOPEN_ROOTS : GSTD_FS_INITIALIZE,
                                       .store_id = expected.store_id,
                                       .expected_cache = expected,
                                       .descriptors = {m->state_fd, -1, -1}};
        if (gstd_filesystem_launch(&input, 10000, &m->initialize)) {
            m->roots_state = -1;
            return -1;
        }
        m->roots_state = 1;
        return 0;
    }
    gstd_filesystem_reply reply = {0};
    int ready = gstd_filesystem_poll(&m->initialize, -1, &reply);
    if (!ready)
        return 0;
    if (ready < 0 || reply.status) {
        gstd_filesystem_reply_close(&reply);
        m->roots_state = -1;
        return -1;
    }
    m->roots = reply;
    gstd_cache_owner cache = reply.cache_owner;
    cache.lock_fd = reply.descriptors[1];
    int ok = sqlite3_exec(m->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    if (ok)
        ok = !gstd_asset_record_cache_bind(m->database, &cache);
    if (!ok || sqlite3_exec(m->database, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(m->database, "ROLLBACK", NULL, NULL, NULL);
        m->roots_state = -1;
        return -1;
    }
    m->environment.cache_fd = reply.descriptors[0];
    m->environment.resource_fd = reply.descriptors[4];
    m->environment.native_fd = reply.descriptors[5];
    m->environment.volume_root = reply.volume_root;
    m->environment.volume_root.root_fd = reply.descriptors[2];
    m->environment.volume_root.lock_fd = reply.descriptors[3];
    m->roots_state = 2;
    return 1;
}
static int notify_descriptor(void) {
#ifdef __linux__
    return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#else
    return -1;
#endif
}
static int control_result(gstd_runtime_manager *m, const char *operation, const char *target,
                          int cancelled, int noop, const char *target_state) {
    json_t *value = json_pack("{s:s,s:b,s:b,s:s}", "targetOperationId", target, "cancelled",
                              cancelled, "noOp", noop, "targetState", target_state);
    char *bytes = value ? gstd_json_dump(value) : NULL;
    json_decref(value);
    sqlite3_stmt *statement = NULL;
    int ok = bytes && sqlite3_prepare_v2(m->database,
                                         "UPDATE operations SET state='succeeded',result=?,"
                                         "updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                                         "operation_id=? AND state='running'",
                                         -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, bytes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, operation, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(m->database) == 1;
    }
    sqlite3_finalize(statement);
    free(bytes);
    return ok ? 0 : -1;
}
static void control(gstd_runtime_manager *m) {
    char operation[65];
    const char *waiting[LIVE_LIMIT];
    size_t count = 0;
    for (unsigned i = 0; i < LIVE_LIMIT; i++)
        if (*m->slots[i].cancel_operation && !m->slots[i].stop_finished)
            waiting[count++] = m->slots[i].cancel_operation;
    if (gstd_cancellation_next(m->database, waiting, count, operation) != 1)
        return;
    sqlite3_stmt *statement = NULL;
    char target[65] = {0}, state[16] = {0}, method[32] = {0}, job[16] = {0};
    int ok =
        sqlite3_prepare_v2(
            m->database,
            "SELECT t.operation_id,t.state,t.method,coalesce(j.state,'') FROM operations c JOIN "
            "operations t ON "
            "t.operation_id=c.target_operation_id LEFT JOIN jobs j ON "
            "j.operation_id=t.operation_id WHERE c.operation_id=? AND c.method='operation.cancel'",
            -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, operation, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(statement) == SQLITE_ROW;
        if (ok) {
            char *outputs[] = {target, state, method, job};
            size_t sizes[] = {65, 16, 32, 16};
            for (unsigned i = 0; i < 4; i++) {
                const char *value = (const char *)sqlite3_column_text(statement, (int)i);
                if (!value || strlen(value) >= sizes[i]) {
                    ok = 0;
                    break;
                }
                memcpy(outputs[i], value, strlen(value) + 1);
            }
        }
    }
    sqlite3_finalize(statement);
    if (!ok) {
        failed(m, operation, "INTERNAL");
        return;
    }
    if (!strcmp(state, "succeeded") || !strcmp(state, "failed") || !strcmp(state, "cancelled")) {
        if (!control_result(m, operation, target, 0, 1, state))
            gstd_schedule_finish(m->database, operation);
        return;
    }
    if (!strcmp(method, "migration.start")) {
        if (strcmp(state, "accepted") || (strcmp(job, "queued") && strcmp(job, "reserved"))) {
            failed(m, operation, "UNSAFE_CANCELLATION");
            return;
        }
        if (sqlite3_exec(m->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return;
        if (gstd_migration_cancel_queued(m->database, target) ||
            control_result(m, operation, target, 1, 0, "cancelled") ||
            sqlite3_exec(m->database, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(m->database, "ROLLBACK", NULL, NULL, NULL);
            failed(m, operation, "UNSAFE_CANCELLATION");
            return;
        }
        gstd_schedule_finish(m->database, target);
        gstd_schedule_finish(m->database, operation);
        return;
    }
    if (strcmp(method, "deployment.start")) {
        failed(m, operation, "UNSAFE_CANCELLATION");
        return;
    }
    if (!strcmp(state, "accepted") && (!strcmp(job, "queued") || !strcmp(job, "reserved"))) {
        sqlite3 *db = m->database;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return;
        ok = sqlite3_prepare_v2(db,
                                "SELECT 1 FROM resource_ownership WHERE operation_id=? AND NOT "
                                "(resource_type='repository-volume' AND lifecycle_stage='reserved' "
                                "AND backend_identity IS NULL) LIMIT 1",
                                -1, &statement, NULL) == SQLITE_OK;
        int proof = SQLITE_ERROR;
        if (ok) {
            sqlite3_bind_text(statement, 1, target, -1, SQLITE_TRANSIENT);
            proof = sqlite3_step(statement);
            ok = proof == SQLITE_DONE;
        }
        sqlite3_finalize(statement);
        if (!ok) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (proof == SQLITE_ROW)
                failed(m, operation, "OWNERSHIP_UNCERTAIN");
            return;
        }
        const char *steps[] = {
            "UPDATE resource_ownership SET ownership_state='released',lifecycle_stage='retained' "
            "WHERE operation_id=?",
            "UPDATE deployments SET "
            "desired_state='stopped',writer_ownership='released',observed_generation=generation,"
            "readiness='pending',recovery='normal' WHERE last_operation_id=?",
            "UPDATE operations SET "
            "state='cancelled',error='{\"code\":\"OPERATION_FAILED\",\"stage\":\"cancel\","
            "\"retryable\":false}',updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
            "operation_id=? AND state='accepted'"};
        for (unsigned i = 0; ok && i < 3; i++) {
            ok = sqlite3_prepare_v2(db, steps[i], -1, &statement, NULL) == SQLITE_OK;
            if (ok) {
                sqlite3_bind_text(statement, 1, target, -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(statement) == SQLITE_DONE;
            }
            sqlite3_finalize(statement);
        }
        if (ok)
            ok = !control_result(m, operation, target, 1, 0, "cancelled");
        if (!ok || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            /* No backend effect was dispatched. Retain running intent for a
             * later control turn; same-key replay cannot create a second cancel. */
            return;
        }
        gstd_schedule_finish(db, target);
        gstd_schedule_finish(db, operation);
        return;
    }
    unsigned slot;
    for (slot = 0; slot < LIVE_LIMIT; slot++)
        if (m->slots[slot].deployment.stage &&
            !strcmp(m->slots[slot].deployment.target.operation_id, target))
            break;
    if (slot == LIVE_LIMIT || m->slots[slot].start_finished || *m->slots[slot].cancel_operation) {
        failed(m, operation, "UNSAFE_CANCELLATION");
        return;
    }
    gstd_deployment *d = &m->slots[slot].deployment;
    if (!gstd_cancellation_can_notify(d)) {
        failed(m, operation, "UNSAFE_CANCELLATION");
        return;
    }
    uint64_t notice = 1;
    if (m->slots[slot].cancellation_fd < 3 ||
        write(m->slots[slot].cancellation_fd, &notice, sizeof(notice)) != sizeof(notice)) {
        failed(m, operation, "OWNERSHIP_UNCERTAIN");
        return;
    }
    memcpy(m->slots[slot].cancel_operation, operation, strlen(operation) + 1);
    d->cancel_requested = 1;
}
void gstd_runtime_manager_poll(gstd_runtime_manager *m) {
    gstd_log_capture_reap();
    if (!m || !m->capable)
        return;
    for (unsigned i = 0; i < LIVE_LIMIT; i++) {
        gstd_migration_runtime *migration = &m->migrations[i];
        if (migration->stage && gstd_migration_runtime_poll(migration, -1) == 1 &&
            !gstd_schedule_finish(m->database, migration->execution.target.operation_id))
            (void)gstd_migration_runtime_close(migration);
    }
    for (unsigned i = 0; i < LIVE_LIMIT; i++) {
        gstd_migration_recovery *recovery = &m->migration_recovery[i];
        if (recovery->stage && gstd_migration_recovery_poll(recovery) == 1 &&
            !gstd_schedule_finish(m->database, recovery->stop_operation))
            (void)gstd_migration_recovery_close(recovery);
    }
    for (unsigned i = 0; i < LIVE_LIMIT; i++) {
        if (*m->slots[i].recovery_stop) {
            int restored = gstd_restoration_poll(&m->slots[i].restoration);
            if (restored == 1) {
                if (!m->slots[i].restoration.outcome &&
                    !gstd_cleanup_begin(&m->slots[i].deployment, m->slots[i].recovery_stop,
                                        &m->slots[i].cleanup)) {
                    m->slots[i].recovery_stop[0] = 0;
                } else {
                    failed(m, m->slots[i].recovery_stop, "OWNERSHIP_UNCERTAIN");
                    if (!gstd_restoration_discard(&m->slots[i].restoration))
                        memset(&m->slots[i], 0, sizeof(m->slots[i]));
                    else
                        m->slots[i].recovery_stop[0] = 0;
                }
            }
            /* A still-unreaped observer fault retains its slot/operation and
             * cannot release ownership or manufacture terminal completion. */
        }
        if (m->slots[i].deployment.stage && !m->slots[i].start_finished) {
            int finished = gstd_deployment_poll(
                &m->slots[i].deployment,
                m->slots[i].cancellation_fd >= 3 ? m->slots[i].cancellation_fd : -1);
            if (finished == 1 &&
                !gstd_schedule_finish(m->database, m->slots[i].deployment.target.operation_id)) {
                m->slots[i].start_finished = 1;
                if (*m->slots[i].cancel_operation) {
                    if (m->slots[i].cancellation_fd >= 3)
                        close(m->slots[i].cancellation_fd);
                    m->slots[i].cancellation_fd = -1;
                }
            }
        }
        if (*m->slots[i].cancel_operation && m->slots[i].start_finished &&
            !m->slots[i].cleanup.stage && !m->slots[i].stop_finished &&
            gstd_cleanup_begin(&m->slots[i].deployment, m->slots[i].cancel_operation,
                               &m->slots[i].cleanup)) {
            /* Retry completion bookkeeping if storage is temporarily unavailable.
             * Keep the cancellation mapped so the dispatcher cannot signal twice. */
            if (!failed(m, m->slots[i].cancel_operation, "OWNERSHIP_UNCERTAIN"))
                m->slots[i].cancel_operation[0] = 0;
        }
        if (m->slots[i].start_finished && !m->slots[i].cleanup.stage &&
            !*m->slots[i].recovery_stop && !m->slots[i].deployment.recovered &&
            !m->slots[i].deployment.outcome &&
            (!*m->slots[i].stop_waiting || m->slots[i].deployment.monitor.running)) {
            (void)gstd_deployment_monitor_poll(&m->slots[i].deployment);
            departed(m, i);
        }
        if (*m->slots[i].stop_waiting && !m->slots[i].deployment.monitor.running) {
            if (gstd_cleanup_begin(&m->slots[i].deployment, m->slots[i].stop_waiting,
                                   &m->slots[i].cleanup))
                failed(m, m->slots[i].stop_waiting, "OWNERSHIP_UNCERTAIN");
            m->slots[i].stop_waiting[0] = 0;
        }
        if (m->slots[i].cleanup.stage && !m->slots[i].stop_finished) {
            int finished = gstd_cleanup_poll(&m->slots[i].cleanup);
            if (finished == 1 &&
                !gstd_schedule_finish(m->database, m->slots[i].cleanup.operation_id)) {
                m->slots[i].stop_finished = 1;
                if (m->slots[i].cancellation_fd >= 3)
                    close(m->slots[i].cancellation_fd);
                m->slots[i].cancellation_fd = -1;
            }
        }
    }
    for (unsigned i = 0; i < LIVE_LIMIT; i++)
        if (m->slots[i].deployment.stage && m->slots[i].stop_finished &&
            !m->slots[i].cleanup.outcome && !gstd_cleanup_dispose(&m->slots[i].cleanup))
            memset(&m->slots[i], 0, sizeof(m->slots[i]));
    control(m);
    struct timespec tick;
    int64_t now = clock_gettime(CLOCK_MONOTONIC, &tick)
                      ? -1
                      : (int64_t)tick.tv_sec * 1000 + tick.tv_nsec / 1000000;
    for (unsigned i = 0; i < LIVE_LIMIT; i++)
        gstd_restart_poll(m->database, &m->restarts[i], now, restart_submit, m);
    if (m->roots_state == 1)
        roots(m, 0);
    /* Starts may initialize roots after durable acceptance. A recorded stop may
     * only reopen existing bound roots; it cannot create or repair directories. */
    if (m->roots_state != 2) {
        sqlite3_stmt *s = NULL;
        int pending = 0;
        if (sqlite3_prepare_v2(
                m->database,
                "SELECT 1 FROM operations o JOIN jobs j ON j.operation_id=o.operation_id WHERE "
                "o.method='deployment.start' AND o.state='accepted' AND j.state='reserved' LIMIT 1",
                -1, &s, NULL) == SQLITE_OK)
            pending = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
        s = NULL;
        int recovery_stop = 0;
        if (!pending &&
            sqlite3_prepare_v2(
                m->database,
                "SELECT 1 FROM operations o JOIN jobs j ON j.operation_id=o.operation_id WHERE "
                "o.method IN ('deployment.stop','migration.start') AND o.state='accepted' AND "
                "j.state='reserved' AND "
                "EXISTS("
                "SELECT 1 FROM resource_ownership r WHERE r.deployment_id=o.deployment_id AND "
                "r.resource_type='configuration' AND r.ownership_state IN ('held','uncertain')) "
                "LIMIT 1",
                -1, &s, NULL) == SQLITE_OK)
            recovery_stop = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
        if (pending || recovery_stop) {
            int ready = roots(m, !pending);
            if (!ready)
                return;
        }
    }
    char operation[65];
    if (gstd_schedule_take(m->database, 0, operation) != 1)
        return;
    sqlite3_stmt *s = NULL;
    char method[32] = {0}, deployment[65] = {0};
    int ok = sqlite3_prepare_v2(m->database,
                                "SELECT method,deployment_id FROM operations WHERE operation_id=?",
                                -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_ROW;
        if (ok) {
            const char *a = (const char *)sqlite3_column_text(s, 0),
                       *b = (const char *)sqlite3_column_text(s, 1);
            ok = a && b && strlen(a) < sizeof(method) && strlen(b) < sizeof(deployment);
            if (ok) {
                memcpy(method, a, strlen(a) + 1);
                memcpy(deployment, b, strlen(b) + 1);
            }
        }
    }
    sqlite3_finalize(s);
    if (!ok) {
        failed(m, operation, "INTERNAL");
        return;
    }
    if (!strcmp(method, "deployment.start")) {
        if (m->roots_state != 2) {
            failed(m, operation, "OWNERSHIP_UNCERTAIN");
            return;
        }
        unsigned slot;
        for (slot = 0; slot < LIVE_LIMIT; slot++)
            if (!m->slots[slot].deployment.stage)
                break;
        if (slot == LIVE_LIMIT) {
            failed(m, operation, "RESOURCE_LIMIT");
            return;
        }
        m->slots[slot].cancellation_fd = notify_descriptor();
        if (m->slots[slot].cancellation_fd < 3) {
            failed(m, operation, "RESOURCE_LIMIT");
            return;
        }
        if (gstd_deployment_begin(&m->environment, operation, &m->slots[slot].deployment)) {
            close(m->slots[slot].cancellation_fd);
            gstd_deployment_target_clear(&m->slots[slot].deployment.target);
            json_decref(m->slots[slot].deployment.credential_source);
            memset(&m->slots[slot], 0, sizeof(m->slots[slot]));
            failed(m, operation, "OPERATION_FAILED");
        }
    } else if (!strcmp(method, "migration.start")) {
        unsigned index;
        for (index = 0; index < LIVE_LIMIT; index++)
            if (!m->migrations[index].stage)
                break;
        if (m->roots_state != 2 || index == LIVE_LIMIT ||
            gstd_migration_runtime_begin(&m->environment, operation, &m->migrations[index]))
            if (migration_unstarted_failed(m, operation))
                failed(m, operation, "OWNERSHIP_UNCERTAIN");
    } else if (!strcmp(method, "deployment.stop")) {
        int stopped = already_stopped(m, operation);
        if (stopped)
            return;
        /* A migration owns different scopes from a deployment START. Never
         * feed those receipts to ordinary restoration or retry its mutator. */
        char migration[65] = {0};
        sqlite3_stmt *prior = NULL;
        int selected =
            sqlite3_prepare_v2(
                m->database,
                "SELECT DISTINCT o.operation_id FROM operations o JOIN resource_ownership r "
                "ON r.operation_id=o.operation_id WHERE o.deployment_id=? AND o.caller_uid=? "
                "AND o.method='migration.start' AND o.state='failed' AND o.recovery='uncertain' "
                "AND r.ownership_state IN ('held','uncertain')",
                -1, &prior, NULL) == SQLITE_OK;
        if (selected) {
            sqlite3_bind_text(prior, 1, deployment, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(prior, 2, (sqlite3_int64)geteuid());
            int step = sqlite3_step(prior);
            if (step == SQLITE_ROW) {
                const char *id = (const char *)sqlite3_column_text(prior, 0);
                int length = sqlite3_column_bytes(prior, 0);
                selected = id && gstd_identifier(id, (size_t)length);
                if (selected)
                    memcpy(migration, id, (size_t)length + 1);
                selected = selected && sqlite3_step(prior) == SQLITE_DONE;
            } else
                selected = step == SQLITE_DONE;
        }
        sqlite3_finalize(prior);
        if (!selected) {
            failed(m, operation, "OWNERSHIP_UNCERTAIN");
            return;
        }
        if (*migration) {
            unsigned index;
            for (index = 0; index < LIVE_LIMIT; index++)
                if (!m->migration_recovery[index].stage)
                    break;
            if (m->roots_state != 2 || index == LIVE_LIMIT ||
                gstd_migration_recovery_begin(&m->environment, migration, operation,
                                              &m->migration_recovery[index]))
                failed(m, operation, "OWNERSHIP_UNCERTAIN");
            return;
        }
        unsigned slot;
        for (slot = 0; slot < LIVE_LIMIT; slot++)
            if (m->slots[slot].deployment.stage &&
                !strcmp(m->slots[slot].deployment.target.deployment_id, deployment))
                break;
        if (slot == LIVE_LIMIT && m->roots_state == 2) {
            char source[65] = {0};
            sqlite3_stmt *record = NULL;
            if (sqlite3_prepare_v2(
                    m->database,
                    "SELECT DISTINCT r.operation_id FROM resource_ownership r JOIN operations o ON "
                    "o.operation_id=r.operation_id WHERE r.deployment_id=? AND "
                    "r.resource_type='configuration' "
                    "AND r.ownership_state IN ('held','uncertain') AND o.method='deployment.start' "
                    "AND o.caller_uid=?",
                    -1, &record, NULL) == SQLITE_OK) {
                sqlite3_bind_text(record, 1, deployment, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(record, 2, (sqlite3_int64)geteuid());
                if (sqlite3_step(record) == SQLITE_ROW) {
                    const char *id = (const char *)sqlite3_column_text(record, 0);
                    int n = sqlite3_column_bytes(record, 0);
                    if (id && gstd_identifier(id, (size_t)n))
                        memcpy(source, id, (size_t)n + 1);
                    if (sqlite3_step(record) != SQLITE_DONE)
                        source[0] = 0;
                }
            }
            sqlite3_finalize(record);
            if (*source) {
                for (slot = 0; slot < LIVE_LIMIT; slot++)
                    if (!m->slots[slot].deployment.stage)
                        break;
                if (slot < LIVE_LIMIT) {
                    m->slots[slot].cancellation_fd = -1;
                    if (!gstd_restoration_begin(&m->environment, source, &m->slots[slot].deployment,
                                                &m->slots[slot].restoration)) {
                        memcpy(m->slots[slot].recovery_stop, operation, strlen(operation) + 1);
                        m->slots[slot].start_finished = 1;
                        return;
                    }
                    (void)gstd_restoration_discard(&m->slots[slot].restoration);
                    memset(&m->slots[slot], 0, sizeof(m->slots[slot]));
                }
            }
            failed(m, operation, "OWNERSHIP_UNCERTAIN");
            return;
        }
        if (slot < LIVE_LIMIT && m->slots[slot].stop_finished) {
            if (gstd_cleanup_retry_begin(&m->slots[slot].cleanup, operation))
                failed(m, operation, "OWNERSHIP_UNCERTAIN");
            else
                m->slots[slot].stop_finished = 0;
            return;
        }
        if (slot < LIVE_LIMIT && m->slots[slot].start_finished && !m->slots[slot].cleanup.stage &&
            !*m->slots[slot].recovery_stop && m->slots[slot].deployment.monitor.running) {
            memcpy(m->slots[slot].stop_waiting, operation, strlen(operation) + 1);
            return;
        }
        if (slot == LIVE_LIMIT || !m->slots[slot].start_finished || *m->slots[slot].recovery_stop ||
            m->slots[slot].cleanup.stage || *m->slots[slot].stop_waiting ||
            gstd_cleanup_begin(&m->slots[slot].deployment, operation, &m->slots[slot].cleanup))
            failed(m, operation, "OWNERSHIP_UNCERTAIN");
    } else
        failed(m, operation, "UNSUPPORTED_CAPABILITY");
}
void gstd_runtime_manager_close(gstd_runtime_manager *m) {
    if (!m)
        return;
    /* At process shutdown worker PDEATHSIG boundaries handle private short jobs.
     * Do not dispose unknown/live actors, issue signals, delete or release their
     * persisted resource owners. The process closes capabilities on exit. */
    if (m->environment.broker_fd >= 3)
        close(m->environment.broker_fd);
    if (!m->initialize.worker)
        gstd_filesystem_reply_close(&m->roots);
    for (unsigned i = 0; i < LIVE_LIMIT; i++)
        gstd_restart_clear(&m->restarts[i]);
    free(m);
}
#ifdef GITSTORED_TESTING
/* Explicit native fixture only: borrow already reopened roots so the private
 * probe exercises production submission/scheduling without installation setup. */
gstd_runtime_manager *gstd_test_runtime_reopened(const gstd_deployment_environment *environment) {
    if (!environment || !environment->database || environment->resource_fd < 3 ||
        environment->native_fd < 3)
        return NULL;
    gstd_runtime_manager *m = calloc(1, sizeof(*m));
    if (m) {
        m->database = environment->database;
        m->state_fd = environment->state_fd;
        m->capable = 1;
        m->roots_state = 2;
        m->environment = *environment;
    }
    return m;
}
int gstd_test_runtime_schema_observed(const gstd_runtime_manager *m) {
    for (unsigned i = 0; m && i < LIVE_LIMIT; i++) {
        const gstd_migration_recovery *r = &m->migration_recovery[i];
        if (r->stage && r->datastore.workloads[2].launched &&
            r->datastore.workloads[2].death_fd >= 3 &&
            r->datastore.workloads[2].population.directory_fd >= 3)
            return 1;
    }
    return 0;
}
/* One isolated durable control turn for metadata fixtures. No server calls this
 * hook, no runtime capability/profile is registered and no effect FD is supplied. */
void gstd_test_control_once(sqlite3 *db) {
    gstd_runtime_manager manager = {.database = db};
    control(&manager);
}
#endif
