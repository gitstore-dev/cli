#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/asset_records.h"
#include "gitstored/asset_cache.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/profiles.h"
#include "gitstored/protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif
static int hex(const char *s) {
    if (!s || strnlen(s, 65) != 64)
        return 0;
    for (size_t i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}
static int id(const char *s) { return s && gstd_identifier(s, strnlen(s, 65)); }
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    return s;
}
static void text(sqlite3_stmt *s, int column, const char *value) {
    sqlite3_bind_text(s, column, value, -1, SQLITE_TRANSIENT);
}
static int equal(sqlite3_stmt *s, int column, const char *value) {
    const unsigned char *found = sqlite3_column_text(s, column);
    return found && sqlite3_column_type(s, column) == SQLITE_TEXT &&
           sqlite3_column_bytes(s, column) == (int)strlen(value) &&
           !memcmp(found, value, strlen(value));
}
static int same(const json_t *v, const char *s) {
    return json_is_string(v) && json_string_length(v) == strlen(s) &&
           !memcmp(json_string_value(v), s, strlen(s));
}
static int finish(sqlite3_stmt *s) {
    if (!s)
        return -1;
    int result = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(s);
    return result;
}
static int unsigned_text(sqlite3_stmt *s, int column, uint64_t *result) {
    const char *value = (const char *)sqlite3_column_text(s, column);
    char *end = NULL;
    errno = 0;
    unsigned long long number = value ? strtoull(value, &end, 10) : 0;
    if (!value || !*value || !end || *end || errno || number > UINT64_MAX)
        return -1;
    *result = (uint64_t)number;
    return 0;
}
int gstd_asset_record_cache_state(sqlite3 *db, gstd_cache_owner *expected) {
    if (!db || !expected)
        return -1;
    memset(expected, 0, sizeof(*expected));
    expected->lock_fd = -1;
    sqlite3_stmt *s = prepare(db, "SELECT q.store_id,b.store_id,b.device,b.inode FROM query_meta q "
                                  "LEFT JOIN asset_cache_binding b ON b.singleton=1");
    if (!s)
        return -1;
    int result = -1;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_bytes(s, 0) == 32) {
        const char *store = (const char *)sqlite3_column_text(s, 0);
        memcpy(expected->store_id, store, 32);
        if (sqlite3_column_type(s, 1) == SQLITE_NULL)
            result = 1;
        else if (equal(s, 1, expected->store_id) && !unsigned_text(s, 2, &expected->device) &&
                 !unsigned_text(s, 3, &expected->inode) && expected->inode)
            result = 0;
        if (sqlite3_step(s) != SQLITE_DONE)
            result = -1;
    }
    sqlite3_finalize(s);
    return result;
}
int gstd_asset_record_cache_bind(sqlite3 *db, const gstd_cache_owner *owner) {
    if (!db || sqlite3_get_autocommit(db) || !owner || owner->lock_fd < 0 || !owner->inode)
        return -1;
    struct stat lock;
    if (fstat(owner->lock_fd, &lock) || !S_ISREG(lock.st_mode) || lock.st_uid != geteuid() ||
        (lock.st_mode & 07777) != 0600 || lock.st_nlink != 1 || lock.st_size ||
        (uint64_t)lock.st_dev != owner->device)
        return -1;
    gstd_cache_owner expected;
    int state = gstd_asset_record_cache_state(db, &expected);
    if (state < 0 || strcmp(owner->store_id, expected.store_id))
        return -1;
    if (!state)
        return owner->device == expected.device && owner->inode == expected.inode ? 0 : -1;
    sqlite3_stmt *s = prepare(db, "INSERT INTO asset_cache_binding SELECT 1,?,?,? "
                                  "WHERE NOT EXISTS(SELECT 1 FROM assets)");
    if (!s)
        return -1;
    char device[32], inode[32];
    snprintf(device, sizeof(device), "%" PRIu64, owner->device);
    snprintf(inode, sizeof(inode), "%" PRIu64, owner->inode);
    text(s, 1, owner->store_id);
    text(s, 2, device);
    text(s, 3, inode);
    return finish(s) || sqlite3_changes(db) != 1 ? -1 : 0;
}
static int cache_matches(sqlite3 *db, int fd) {
    gstd_cache_owner expected;
    struct stat info;
    return !gstd_asset_record_cache_state(db, &expected) && !fstat(fd, &info) &&
           S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 07777) == 0700 &&
           (uint64_t)info.st_dev == expected.device && (uint64_t)info.st_ino == expected.inode;
}
static int pin(sqlite3 *db, const char *key, const char *resource) {
    sqlite3_stmt *s = prepare(db, "SELECT asset_key FROM asset_refs WHERE resource_id=?");
    if (!s)
        return -1;
    text(s, 1, resource);
    int step = sqlite3_step(s), matched = step == SQLITE_ROW && equal(s, 0, key);
    sqlite3_finalize(s);
    if (step == SQLITE_ROW)
        return matched ? 0 : -1;
    if (step != SQLITE_DONE)
        return -1;
    s = prepare(db, "SELECT count(*) FROM asset_refs");
    if (!s)
        return -1;
    int room = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int64(s, 0) < 262144;
    sqlite3_finalize(s);
    if (!room)
        return -1;
    s = prepare(db, "INSERT INTO asset_refs VALUES(?,?)");
    if (!s)
        return -1;
    text(s, 1, resource);
    text(s, 2, key);
    return finish(s);
}
static int authorized(sqlite3 *db, const json_t *manifest, const char *component, const char *op,
                      const char *resource, int *consent) {
    char platform[64];
    json_t *target = json_object_get(manifest, "platform");
    int formatted = snprintf(platform, sizeof(platform), "%s-%s",
                             json_string_value(json_object_get(target, "os")),
                             json_string_value(json_object_get(target, "architecture")));
    if (formatted < 0 || (size_t)formatted >= sizeof(platform))
        return -1;
    sqlite3_stmt *s = prepare(
        db, "SELECT "
            "o.intent,o.caller_uid,o.method,o.state,p.server_revision,p.manifest_digest,p.platform "
            "FROM operations o JOIN "
            "resource_ownership r ON r.operation_id=o.operation_id AND "
            "r.deployment_id=o.deployment_id JOIN deployments d ON d.deployment_id=o.deployment_id "
            "JOIN profiles p ON p.profile_id=d.profile_id AND p.profile_version=d.profile_version "
            "WHERE o.operation_id=? AND r.resource_id=? "
            "AND r.resource_type='asset-root' AND r.ownership_state='held' AND "
            "r.lifecycle_stage IN ('reserved','allocated','attached','running')");
    if (!s)
        return -1;
    text(s, 1, op);
    text(s, 2, resource);
    json_t *intent = NULL;
    int valid = 0;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int64(s, 1) == (sqlite3_int64)geteuid() &&
        equal(s, 2, "deployment.start") && (equal(s, 3, "accepted") || equal(s, 3, "running")) &&
        equal(s, 4, json_string_value(json_object_get(manifest, "serverRevision"))) &&
        equal(s, 5, json_string_value(json_object_get(manifest, "profileDigest"))) &&
        equal(s, 6, platform)) {
        const unsigned char *bytes = sqlite3_column_text(s, 0);
        int length = sqlite3_column_bytes(s, 0);
        if (bytes && length > 0)
            intent = gstd_json_parse(bytes, (size_t)length);
    }
    sqlite3_finalize(s);
    if (intent &&
        json_equal(json_object_get(intent, "profileId"), json_object_get(manifest, "profileId")) &&
        json_equal(json_object_get(intent, "profileVersion"),
                   json_object_get(manifest, "profileVersion"))) {
        json_t *components = gstd_profile_components(json_object_get(intent, "components"));
        for (size_t i = 0; i < json_array_size(components); i++)
            if (same(json_array_get(components, i), component))
                valid = 1;
        json_decref(components);
    }
    *consent = json_is_true(json_object_get(intent, "allowProvision"));
    json_decref(intent);
    return valid ? 0 : -1;
}
static gstd_asset_record_status begin(sqlite3 *db, const json_t *manifest, int offline_fd,
                                      const char *component, const char *operation,
                                      const char *resource, uint64_t download_limit,
                                      uint64_t merge_limit, uint64_t limit, char key[65],
                                      char name[138]) {
    if (!key || !name)
        return GSTD_ASSET_RECORD_ERROR;
    key[0] = 0;
    name[0] = 0;
    if (!db || sqlite3_get_autocommit(db) || !component || !id(operation) || !id(resource) ||
        !download_limit || download_limit > UINT64_C(2) * 1024 * 1024 * 1024 ||
        merge_limit < 1024 || merge_limit > UINT64_C(8) * 1024 * 1024 * 1024 || !limit ||
        limit > UINT64_C(64) * 1024 * 1024 * 1024 || gstd_asset_manifest_check(manifest))
        return GSTD_ASSET_RECORD_ERROR;
    struct stat source = {0};
    if (offline_fd >= 0 &&
        (fstat(offline_fd, &source) || !S_ISDIR(source.st_mode) ||
         (source.st_uid != 0 && source.st_uid != geteuid()) || (source.st_mode & 0022)))
        return GSTD_ASSET_RECORD_ERROR;
    uint64_t reservation = download_limit + 2 * merge_limit + UINT64_C(1024) * 1024;
    json_t *images = json_object_get(manifest, "images");
    int selected = 0;
    for (size_t i = 0; i < json_array_size(images); i++)
        if (same(json_object_get(json_array_get(images, i), "component"), component))
            selected = 1;
    if (!selected)
        return GSTD_ASSET_RECORD_ERROR;
    gstd_cache_owner binding;
    if (gstd_asset_record_cache_state(db, &binding))
        return GSTD_ASSET_RECORD_ERROR;
    int consent = 0;
    if (authorized(db, manifest, component, operation, resource, &consent))
        return GSTD_ASSET_RECORD_ERROR;
    json_t *components = json_pack("[s]", component);
    const char *architecture =
        json_string_value(json_object_get(json_object_get(manifest, "platform"), "architecture"));
    json_t *single = components ? gstd_asset_manifest(components, architecture) : NULL;
    json_decref(components);
    if (!single)
        return GSTD_ASSET_RECORD_ERROR;
    char *descriptor = gstd_json_dump(single);
    int hashed = gstd_fingerprint(single, key);
    json_decref(single);
    if (!descriptor || hashed) {
        free(descriptor);
        return GSTD_ASSET_RECORD_ERROR;
    }
    gstd_asset_record_status result = GSTD_ASSET_RECORD_ERROR;
    sqlite3_stmt *s = prepare(db, "SELECT descriptor,state FROM assets WHERE asset_key=?");
    if (!s)
        goto done;
    text(s, 1, key);
    int step = sqlite3_step(s), exists = step == SQLITE_ROW,
        match = exists && equal(s, 0, descriptor), active = exists && equal(s, 1, "active");
    sqlite3_finalize(s);
    if (exists) {
        if (!match)
            goto done;
        result = active
                     ? (pin(db, key, resource) ? GSTD_ASSET_RECORD_ERROR : GSTD_ASSET_RECORD_ACTIVE)
                     : GSTD_ASSET_RECORD_HELD;
        goto done;
    }
    if (step != SQLITE_DONE)
        goto done;
    if (!consent && offline_fd < 0) {
        result = GSTD_ASSET_RECORD_CONSENT;
        goto done;
    }
    s = prepare(db, "SELECT count(*),coalesce(sum(CASE WHEN state IN ('active','evicting') THEN "
                    "logical_bytes ELSE "
                    "reserved_bytes END),0) FROM assets");
    if (!s)
        goto done;
    step = sqlite3_step(s);
    int full = 1;
    if (step == SQLITE_ROW) {
        sqlite3_int64 used = sqlite3_column_int64(s, 1);
        full = used < 0 || sqlite3_column_int64(s, 0) >= 4096 || (uint64_t)used > limit ||
               reservation > limit - (uint64_t)used;
    }
    sqlite3_finalize(s);
    if (step != SQLITE_ROW)
        goto done;
    if (full) {
        result = GSTD_ASSET_RECORD_LIMIT;
        goto done;
    }
    if (gstd_asset_stage_name(key, operation, name))
        goto done;
    s = prepare(db, "INSERT INTO "
                    "assets(asset_key,descriptor,producer_operation_id,operation_id,staging_name,"
                    "state,reserved_bytes,download_limit,merge_limit,acquisition_mode,source_"
                    "device,source_inode) "
                    "VALUES(?,?,?,?,?,'staging',?,?,?,?,?,?)");
    if (!s)
        goto done;
    text(s, 1, key);
    text(s, 2, descriptor);
    text(s, 3, operation);
    text(s, 4, operation);
    text(s, 5, name);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)reservation);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)download_limit);
    sqlite3_bind_int64(s, 8, (sqlite3_int64)merge_limit);
    text(s, 9, offline_fd >= 0 ? "offline" : "registry");
    char source_device[32], source_inode[32];
    snprintf(source_device, sizeof(source_device), "%ju", (uintmax_t)source.st_dev);
    snprintf(source_inode, sizeof(source_inode), "%ju", (uintmax_t)source.st_ino);
    if (offline_fd >= 0) {
        text(s, 10, source_device);
        text(s, 11, source_inode);
    } else {
        sqlite3_bind_null(s, 10);
        sqlite3_bind_null(s, 11);
    }
    if (finish(s) || pin(db, key, resource))
        goto done;
    result = GSTD_ASSET_RECORD_NEW;
done:
    if (result != GSTD_ASSET_RECORD_NEW)
        name[0] = 0;
    free(descriptor);
    return result;
}
gstd_asset_record_status gstd_asset_record_begin(sqlite3 *db, const json_t *manifest,
                                                 const char *component, const char *operation,
                                                 const char *resource, uint64_t download,
                                                 uint64_t merge, uint64_t limit, char key[65],
                                                 char name[138]) {
    return begin(db, manifest, -1, component, operation, resource, download, merge, limit, key,
                 name);
}
gstd_asset_record_status gstd_asset_record_begin_offline(sqlite3 *db, const json_t *manifest,
                                                         int source_fd, const char *component,
                                                         const char *operation,
                                                         const char *resource, uint64_t download,
                                                         uint64_t merge, uint64_t limit,
                                                         char key[65], char name[138]) {
    if (source_fd < 0) {
        if (key)
            key[0] = 0;
        if (name)
            name[0] = 0;
        return GSTD_ASSET_RECORD_ERROR;
    }
    return begin(db, manifest, source_fd, component, operation, resource, download, merge, limit,
                 key, name);
}
int gstd_asset_record_source(sqlite3 *db, const char *key, const char *op, int source_fd) {
    if (!db || !hex(key) || !id(op) || source_fd < -1)
        return -1;
    sqlite3_stmt *s = prepare(db, "SELECT a.acquisition_mode,a.source_device,a.source_inode FROM "
                                  "assets a JOIN operations o ON o.operation_id=a.operation_id "
                                  "WHERE a.asset_key=? AND a.operation_id=? AND a.state='staging' "
                                  "AND o.state IN ('accepted','running') AND o.caller_uid=?");
    if (!s)
        return -1;
    text(s, 1, key);
    text(s, 2, op);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)geteuid());
    int valid = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        if (source_fd < 0)
            valid = equal(s, 0, "registry");
        else {
            struct stat source;
            char device[32], inode[32];
            if (!fstat(source_fd, &source) && S_ISDIR(source.st_mode) &&
                (source.st_uid == 0 || source.st_uid == geteuid()) && !(source.st_mode & 0022)) {
                snprintf(device, sizeof(device), "%ju", (uintmax_t)source.st_dev);
                snprintf(inode, sizeof(inode), "%ju", (uintmax_t)source.st_ino);
                valid = equal(s, 0, "offline") && equal(s, 1, device) && equal(s, 2, inode);
            }
        }
    }
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
static int mutable(sqlite3 *db, const char *key, const char *op) {
    return db && !sqlite3_get_autocommit(db) && hex(key) && id(op);
}
static void identity_strings(const struct stat *st, char device[32], char inode[32]) {
    snprintf(device, 32, "%ju", (uintmax_t)st->st_dev);
    snprintf(inode, 32, "%ju", (uintmax_t)st->st_ino);
}
int gstd_asset_record_identity(sqlite3 *db, const char *key, const char *op, int fd) {
    struct stat info;
    if (!mutable(db, key, op) || fstat(fd, &info) || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 07777) != 0700)
        return -1;
    int parent = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int bound = parent >= 0 && cache_matches(db, parent);
    if (parent >= 0)
        close(parent);
    if (!bound)
        return -1;
    char device[32], inode[32];
    identity_strings(&info, device, inode);
    sqlite3_stmt *s =
        prepare(db, "UPDATE assets SET staging_device=?,staging_inode=? WHERE asset_key=? AND "
                    "operation_id=? AND state='staging' AND (staging_inode IS NULL OR "
                    "(staging_inode=? AND staging_device=?)) AND EXISTS(SELECT 1 FROM operations "
                    "WHERE operation_id=? AND caller_uid=? AND state IN ('accepted','running'))");
    if (!s)
        return -1;
    text(s, 1, device);
    text(s, 2, inode);
    text(s, 3, key);
    text(s, 4, op);
    text(s, 5, inode);
    text(s, 6, device);
    text(s, 7, op);
    sqlite3_bind_int64(s, 8, (sqlite3_int64)geteuid());
    return finish(s) || sqlite3_changes(db) != 1 ? -1 : 0;
}
int gstd_asset_record_verified(sqlite3 *db, const char *key, const char *op,
                               const gstd_provision_result *proof) {
    if (!mutable(db, key, op) || !proof || !hex(proof->tree_digest) || !proof->downloaded_bytes ||
        proof->downloaded_bytes > UINT64_C(2) * 1024 * 1024 * 1024 || proof->merged_bytes < 1024 ||
        proof->merged_bytes > UINT64_C(8) * 1024 * 1024 * 1024 ||
        proof->logical_bytes > UINT64_C(8) * 1024 * 1024 * 1024)
        return -1;
    char device[32], inode[32];
    snprintf(device, sizeof(device), "%" PRIu64, proof->staging_device);
    snprintf(inode, sizeof(inode), "%" PRIu64, proof->staging_inode);
    sqlite3_stmt *s = prepare(
        db,
        "UPDATE assets SET "
        "state='verified',tree_digest=?,logical_bytes=?,downloaded_bytes=?,merged_bytes=? WHERE "
        "asset_key=? AND operation_id=? AND state IN ('staging','verified') AND staging_device=? "
        "AND staging_inode=? AND reserved_bytes>=? AND EXISTS(SELECT 1 FROM operations WHERE "
        "operation_id=? AND caller_uid=? AND state IN ('accepted','running'))");
    if (!s)
        return -1;
    text(s, 1, proof->tree_digest);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)proof->logical_bytes);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)proof->downloaded_bytes);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)proof->merged_bytes);
    text(s, 5, key);
    text(s, 6, op);
    text(s, 7, device);
    text(s, 8, inode);
    sqlite3_bind_int64(s, 9, (sqlite3_int64)proof->logical_bytes);
    text(s, 10, op);
    sqlite3_bind_int64(s, 11, (sqlite3_int64)geteuid());
    return finish(s) || sqlite3_changes(db) != 1 ? -1 : 0;
}
int gstd_asset_record_active(sqlite3 *db, const char *key, const char *op, int cache) {
    if (!db || !cache_matches(db, cache))
        return -1;
    if (!mutable(db, key, op))
        return -1;
#if defined(__linux__) && defined(SYS_openat2)
    sqlite3_stmt *s =
        prepare(db, "SELECT staging_name,staging_device,staging_inode,state FROM assets WHERE "
                    "asset_key=? AND producer_operation_id=? AND state IN ('verified','active')");
    if (!s)
        return -1;
    text(s, 1, key);
    text(s, 2, op);
    struct stat info;
    int result = -1, active = 0, fd = -1;
    if (sqlite3_step(s) != SQLITE_ROW)
        goto done;
    const char *name = (const char *)sqlite3_column_text(s, 0);
    if (!name || fstat(cache, &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700)
        goto done;
    if (!fstatat(cache, name, &info, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
        goto done;
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    fd = (int)syscall(SYS_openat2, cache, key, &how, sizeof(how));
    if (fd < 0 || fstat(fd, &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0700)
        goto done;
    int parent = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int bound = parent >= 0 && cache_matches(db, parent);
    if (parent >= 0)
        close(parent);
    if (!bound)
        return -1;
    char device[32], inode[32];
    identity_strings(&info, device, inode);
    if (!equal(s, 1, device) || !equal(s, 2, inode))
        goto done;
    active = equal(s, 3, "active");
    result = 0;
done:
    if (fd >= 0)
        close(fd);
    sqlite3_finalize(s);
    if (result || active)
        return result;
    s = prepare(
        db, "UPDATE assets SET state='active',operation_id=NULL,reserved_bytes=0 WHERE asset_key=? "
            "AND operation_id=? AND state='verified' AND EXISTS(SELECT 1 FROM operations WHERE "
            "operation_id=? AND caller_uid=? AND state IN ('accepted','running'))");
    if (!s)
        return -1;
    text(s, 1, key);
    text(s, 2, op);
    text(s, 3, op);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)geteuid());
    return finish(s) || sqlite3_changes(db) != 1 ? -1 : 0;
#else
    (void)cache;
    return -1;
#endif
}
int gstd_asset_record_unpin(sqlite3 *db, const char *resource) {
    if (!db || sqlite3_get_autocommit(db) || !id(resource))
        return -1;
    sqlite3_stmt *s = prepare(db, "DELETE FROM asset_refs WHERE resource_id=?");
    if (!s)
        return -1;
    text(s, 1, resource);
    return finish(s);
}
int gstd_asset_record_refcount(sqlite3 *db, const char *key, uint64_t *count) {
    if (!db || !hex(key) || !count)
        return -1;
    *count = 0;
    sqlite3_stmt *s = prepare(db, "SELECT count(*) FROM asset_refs WHERE asset_key=?");
    if (!s)
        return -1;
    text(s, 1, key);
    int valid = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int64(s, 0) >= 0;
    if (valid)
        *count = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}

int gstd_asset_record_evict_begin(sqlite3 *db, const char *key, gstd_provision_result *proof) {
    if (!proof)
        return -1;
    memset(proof, 0, sizeof(*proof));
    if (!db || sqlite3_get_autocommit(db) || !hex(key))
        return -1;
    sqlite3_stmt *s = prepare(db, "SELECT staging_device,staging_inode,tree_digest,logical_bytes,"
                                  "downloaded_bytes,merged_bytes FROM assets WHERE asset_key=? "
                                  "AND state IN ('active','evicting') AND NOT EXISTS(SELECT 1 FROM "
                                  "asset_refs WHERE asset_key=?)");
    if (!s)
        return -1;
    text(s, 1, key);
    text(s, 2, key);
    int valid = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        uint64_t identity[2];
        valid = 1;
        for (int i = 0; i < 2; i++) {
            const char *value = (const char *)sqlite3_column_text(s, i);
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = value ? strtoull(value, &end, 10) : 0;
            if (!value || !*value || !end || *end || errno || parsed > UINT64_MAX) {
                valid = 0;
                break;
            }
            identity[i] = (uint64_t)parsed;
        }
        const char *hash = (const char *)sqlite3_column_text(s, 2);
        if (!hex(hash))
            valid = 0;
        if (valid) {
            proof->staging_device = identity[0];
            proof->staging_inode = identity[1];
            memcpy(proof->tree_digest, hash, 65);
            proof->logical_bytes = (uint64_t)sqlite3_column_int64(s, 3);
            proof->downloaded_bytes = (uint64_t)sqlite3_column_int64(s, 4);
            proof->merged_bytes = (uint64_t)sqlite3_column_int64(s, 5);
        }
    }
    sqlite3_finalize(s);
    if (!valid)
        return -1;
    s = prepare(db,
                "UPDATE assets SET state='evicting' WHERE asset_key=? AND state IN "
                "('active','evicting') AND NOT EXISTS(SELECT 1 FROM asset_refs WHERE asset_key=?)");
    if (!s)
        return -1;
    text(s, 1, key);
    text(s, 2, key);
    return finish(s) || sqlite3_changes(db) != 1 ? -1 : 0;
}
int gstd_asset_record_evict_finish(sqlite3 *db, const char *key, int cache) {
    if (!db || sqlite3_get_autocommit(db) || !hex(key) || !cache_matches(db, cache))
        return -1;
    char role[71];
    snprintf(role, sizeof(role), "evict-%s", key);
    struct stat info;
    if (!fstatat(cache, key, &info, AT_SYMLINK_NOFOLLOW) || errno != ENOENT ||
        !fstatat(cache, role, &info, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
        return -1;
    sqlite3_stmt *s = prepare(db, "DELETE FROM assets WHERE asset_key=? AND state='evicting' "
                                  "AND NOT EXISTS(SELECT 1 FROM asset_refs WHERE asset_key=?)");
    if (!s)
        return -1;
    text(s, 1, key);
    text(s, 2, key);
    return finish(s) || sqlite3_changes(db) != 1 ? -1 : 0;
}
int gstd_asset_record_evict_candidate(sqlite3 *db, char key[65]) {
    if (!db || !key)
        return -1;
    key[0] = 0;
    sqlite3_stmt *s = prepare(db, "SELECT asset_key FROM assets a WHERE state IN "
                                  "('active','evicting') AND NOT EXISTS(SELECT 1 FROM asset_refs r "
                                  "WHERE r.asset_key=a.asset_key) ORDER BY "
                                  "CASE state WHEN 'evicting' THEN 0 ELSE 1 END,asset_key LIMIT 1");
    if (!s)
        return -1;
    int step = sqlite3_step(s), result = -1;
    if (step == SQLITE_DONE)
        result = 1;
    else if (step == SQLITE_ROW && hex((const char *)sqlite3_column_text(s, 0))) {
        memcpy(key, sqlite3_column_text(s, 0), 65);
        result = 0;
    }
    sqlite3_finalize(s);
    return result;
}

int gstd_asset_record_receipt(sqlite3 *db, const char *key, const char *operation,
                              const char *resource, gstd_provision_result *proof) {
    if (!proof)
        return -1;
    memset(proof, 0, sizeof(*proof));
    if (!db || !hex(key) || !id(operation) || !id(resource))
        return -1;
    sqlite3_stmt *s = prepare(
        db, "SELECT a.staging_device,a.staging_inode,a.tree_digest,a.logical_bytes,"
            "a.downloaded_bytes,a.merged_bytes FROM assets a JOIN asset_refs f ON "
            "f.asset_key=a.asset_key "
            "JOIN resource_ownership r ON r.resource_id=f.resource_id JOIN operations o ON "
            "o.operation_id=r.operation_id "
            "JOIN deployments d ON d.deployment_id=r.deployment_id "
            "WHERE a.asset_key=? AND a.state='active' AND r.resource_id=? AND r.operation_id=? "
            "AND r.resource_type='asset-root' AND r.ownership_state='held' AND o.caller_uid=? "
            "AND o.method='deployment.start' AND o.state IN ('accepted','running') AND "
            "d.last_operation_id=o.operation_id");
    if (!s)
        return -1;
    text(s, 1, key);
    text(s, 2, resource);
    text(s, 3, operation);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)geteuid());
    int valid = sqlite3_step(s) == SQLITE_ROW;
    uint64_t identity[2] = {0};
    if (valid)
        for (int i = 0; i < 2; i++) {
            const char *value = (const char *)sqlite3_column_text(s, i);
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = value ? strtoull(value, &end, 10) : 0;
            if (!value || !*value || !end || *end || errno || parsed > UINT64_MAX ||
                (i == 1 && !parsed)) {
                valid = 0;
                break;
            }
            identity[i] = (uint64_t)parsed;
        }
    const char *hash = valid ? (const char *)sqlite3_column_text(s, 2) : NULL;
    valid = valid && hex(hash) && sqlite3_column_int64(s, 3) > 0 &&
            sqlite3_column_int64(s, 4) > 0 && sqlite3_column_int64(s, 5) >= 1024;
    if (valid) {
        proof->staging_device = identity[0];
        proof->staging_inode = identity[1];
        memcpy(proof->tree_digest, hash, 65);
        proof->logical_bytes = (uint64_t)sqlite3_column_int64(s, 3);
        proof->downloaded_bytes = (uint64_t)sqlite3_column_int64(s, 4);
        proof->merged_bytes = (uint64_t)sqlite3_column_int64(s, 5);
    }
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
