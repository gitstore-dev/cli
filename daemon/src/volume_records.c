#define _POSIX_C_SOURCE 200809L
#include "volume_records.h"
#include "gitstored/protocol.h"
#include "migration.h"
#include "migration_execution.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int scylla_id(const char *deployment, char volume[65]);
static int oidc_id(const char *, int, char[65]);
static int oidc_current_deployment(sqlite3 *, const char *, char[65]);
static int identifier(const char *text) { return text && gstd_identifier(text, strnlen(text, 65)); }
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = NULL;
    return sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK ? statement : NULL;
}
static sqlite3_stmt *role_prepare(sqlite3 *db, const char *format, int scylla) {
    if (scylla < 0 || scylla > 3)
        return NULL;
    const char *table[] = {"repository_volumes", "datastore_volumes", "oidc_provider_volumes",
                           "oidc_provider_volumes"};
    const char *role[] = {"repositories", "scylladb", "oidc-hydra", "oidc-kratos"};
    const char *compatibility =
        scylla >= 2 ? gstd_oidc_volume_compatibility((gstd_oidc_volume_role)(scylla - 2))
        : scylla    ? "scylla-2026.1:gitstore-06152d-schema-012"
                    : "gitstore-repositories-v1";
    char *sql = sqlite3_mprintf(format, table[scylla], role[scylla], compatibility);
    sqlite3_stmt *statement = sql ? prepare(db, sql) : NULL;
    sqlite3_free(sql);
    return statement;
}
static void bind(sqlite3_stmt *statement, int column, const char *text) {
    sqlite3_bind_text(statement, column, text, -1, SQLITE_TRANSIENT);
}
static void number(sqlite3_stmt *statement, int column, uint64_t value) {
    char text[32];
    snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    bind(statement, column, text);
}
static int matches(sqlite3_stmt *statement, int column, const char *text) {
    const unsigned char *value = sqlite3_column_text(statement, column);
    return value && sqlite3_column_type(statement, column) == SQLITE_TEXT &&
           sqlite3_column_bytes(statement, column) == (int)strlen(text) &&
           !memcmp(value, text, strlen(text));
}
static int read_number(sqlite3_stmt *statement, int column, uint64_t *output) {
    const char *text = (const char *)sqlite3_column_text(statement, column);
    int length = sqlite3_column_bytes(statement, column);
    if (!text || sqlite3_column_type(statement, column) != SQLITE_TEXT || length < 1 ||
        length > 20 || memchr(text, 0, (size_t)length) || (length > 1 && text[0] == '0'))
        return -1;
    for (int i = 0; i < length; i++)
        if (text[i] < '0' || text[i] > '9')
            return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long value = text ? strtoull(text, &end, 10) : 0;
    if (!text || !*text || *text < '0' || *text > '9' || !end || *end || errno)
        return -1;
    *output = (uint64_t)value;
    return 0;
}
static int root(sqlite3 *db, const gstd_volume_root *owner) {
    struct stat info, directory, named;
    if (!owner || owner->lock_fd < 3 || !owner->inode || strnlen(owner->store_id, 33) != 32 ||
        fstat(owner->lock_fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 07777) != 0600 || info.st_nlink != 1 || info.st_size ||
        (uint64_t)info.st_dev != owner->device || owner->root_fd < 3 ||
        fstat(owner->root_fd, &directory) || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != geteuid() || (directory.st_mode & 07777) != 0700 ||
        (uint64_t)directory.st_dev != owner->device || (uint64_t)directory.st_ino != owner->inode ||
        fstatat(owner->root_fd, ".gitstored-volume-lock", &named, AT_SYMLINK_NOFOLLOW) ||
        info.st_dev != named.st_dev || info.st_ino != named.st_ino)
        return 0;
    sqlite3_stmt *statement = prepare(db, "SELECT store_id FROM query_meta");
    if (!statement)
        return 0;
    int valid = sqlite3_step(statement) == SQLITE_ROW && matches(statement, 0, owner->store_id) &&
                sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return valid;
}
static int authorized(sqlite3 *db, const char *volume, const char *operation, int scylla) {
    sqlite3_stmt *statement = prepare(
        db,
        "SELECT o.caller_uid,o.method FROM resource_ownership r JOIN operations o "
        "ON r.operation_id=o.operation_id AND r.deployment_id=o.deployment_id WHERE "
        "r.resource_id=? AND r.resource_type=? AND r.ownership_state='held' AND "
        "r.lifecycle_stage IN ('reserved','allocated','attached','running') AND o.operation_id=? "
        "AND "
        "o.method IN ('deployment.start','migration.start') AND o.state IN ('accepted','running')");
    if (!statement)
        return 0;
    bind(statement, 1, volume);
    bind(statement, 2, scylla ? "datastore-volume" : "repository-volume");
    bind(statement, 3, operation);
    int valid = sqlite3_step(statement) == SQLITE_ROW &&
                sqlite3_column_int64(statement, 0) == (sqlite3_int64)geteuid();
    int migration = valid && matches(statement, 1, "migration.start");
    valid = valid && (migration || matches(statement, 1, "deployment.start")) &&
            sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (valid && migration) {
        gstd_migration_execution execution = {0};
        char expected[65];
        valid = scylla == 1 && !gstd_migration_execution_read(db, operation, &execution) &&
                !scylla_id(execution.target.deployment_id, expected) && !strcmp(volume, expected);
        gstd_migration_execution_clear(&execution);
    }
    if (valid && scylla >= 2) {
        char deployment[65], expected[65];
        valid = !migration && !oidc_current_deployment(db, operation, deployment) &&
                !oidc_id(deployment, scylla, expected) && !strcmp(volume, expected);
    }
    return valid ? (migration ? 2 : 1) : 0;
}
static int identity(sqlite3_stmt *statement, int first, gstd_volume_identity *output) {
    uint64_t values[6];
    for (int i = 0; i < 6; i++)
        if (read_number(statement, first + i, &values[i]))
            return -1;
    *output =
        (gstd_volume_identity){values[0], values[1], values[2], values[3], values[4], values[5]};
    return !output->volume_inode || !output->data_inode || !output->writer_inode ? -1 : 0;
}
static int equal(const gstd_volume_identity *a, const gstd_volume_identity *b) {
    return a->volume_device == b->volume_device && a->volume_inode == b->volume_inode &&
           a->data_device == b->data_device && a->data_inode == b->data_inode &&
           a->writer_device == b->writer_device && a->writer_inode == b->writer_inode;
}
static int record_begin(sqlite3 *db, const gstd_volume_root *owner, const char *volume,
                        const char *operation, char producer[65], gstd_volume_identity *expected,
                        int scylla) {
    if (!producer || !expected)
        return -1;
    producer[0] = 0;
    memset(expected, 0, sizeof(*expected));
    if (!db || sqlite3_get_autocommit(db) || !identifier(volume) || !identifier(operation) ||
        !root(db, owner))
        return -1;
    int authority = authorized(db, volume, operation, scylla);
    if (!authority)
        return -1;
    sqlite3_stmt *statement =
        role_prepare(db,
                     "SELECT producer_operation_id,store_id,root_device,root_inode,"
                     "owner_uid,allocated,volume_device,volume_inode,data_device,data_inode,writer_"
                     "device,writer_inode "
                     "FROM %s WHERE volume_id=? AND role='%s' AND compatibility='%s'",
                     scylla);
    if (!statement)
        return -1;
    bind(statement, 1, volume);
    int step = sqlite3_step(statement), result = -1;
    if (step == SQLITE_ROW) {
        uint64_t device = 0, inode = 0;
        const char *first = (const char *)sqlite3_column_text(statement, 0);
        if (identifier(first) && matches(statement, 1, owner->store_id) &&
            !read_number(statement, 2, &device) && !read_number(statement, 3, &inode) &&
            device == owner->device && inode == owner->inode &&
            sqlite3_column_int64(statement, 4) == (sqlite3_int64)geteuid()) {
            result = sqlite3_column_int(statement, 5);
            if ((authority == 2 && result != 1) ||
                (result == 1 && identity(statement, 6, expected)))
                result = -1;
            if (result == 0 || result == 1)
                snprintf(producer, 65, "%s", first);
        }
        sqlite3_finalize(statement);
        return result;
    }
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE || authority == 2)
        return -1; /* Standalone migration never allocates a missing datastore. */
    statement = role_prepare(db,
                             "INSERT INTO %s(volume_id,role,owner_uid,producer_operation_id,"
                             "store_id,root_device,root_inode,compatibility) "
                             "VALUES(?,'%s',?,?,?,?,?,'%s')",
                             scylla);
    if (!statement)
        return -1;
    bind(statement, 1, volume);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)geteuid());
    bind(statement, 3, operation);
    bind(statement, 4, owner->store_id);
    number(statement, 5, owner->device);
    number(statement, 6, owner->inode);
    result = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    sqlite3_finalize(statement);
    if (!result)
        snprintf(producer, 65, "%s", operation);
    return result;
}
static int record_ready(sqlite3 *db, const gstd_volume_root *owner, const char *volume,
                        const char *operation, const gstd_volume_handle *handle, int scylla) {
    if (!db || sqlite3_get_autocommit(db) || !handle || !root(db, owner) || !identifier(volume) ||
        !identifier(operation) || !authorized(db, volume, operation, scylla))
        return -1;
    struct stat data, writer;
    const gstd_volume_identity *proof = &handle->identity;
    if (handle->data_fd < 3 || handle->writer_fd < 3 || fstat(handle->data_fd, &data) ||
        fstat(handle->writer_fd, &writer) || !S_ISDIR(data.st_mode) || !S_ISREG(writer.st_mode) ||
        data.st_uid != geteuid() || writer.st_uid != geteuid() || (data.st_mode & 07777) != 0700 ||
        (writer.st_mode & 07777) != 0600 || writer.st_nlink != 1 || writer.st_size ||
        (uint64_t)data.st_dev != proof->data_device || (uint64_t)data.st_ino != proof->data_inode ||
        (uint64_t)writer.st_dev != proof->writer_device ||
        (uint64_t)writer.st_ino != proof->writer_inode || proof->volume_device != owner->device ||
        proof->data_device != owner->device || proof->writer_device != owner->device ||
        !proof->volume_inode || !proof->data_inode || !proof->writer_inode)
        return -1;
    sqlite3_stmt *reserved = role_prepare(
        db, "SELECT 1 FROM %s WHERE volume_id=? AND role='%s' AND compatibility='%s'", scylla);
    if (!reserved)
        return -1;
    bind(reserved, 1, volume);
    int exists = sqlite3_step(reserved) == SQLITE_ROW;
    sqlite3_finalize(reserved);
    if (!exists)
        return -1;
    char producer[65];
    gstd_volume_identity expected;
    int state = record_begin(db, owner, volume, operation, producer, &expected, scylla);
    if (state < 0)
        return -1;
    if (state == 1)
        return equal(&expected, proof) ? 0 : -1;
    sqlite3_stmt *statement =
        role_prepare(db,
                     "UPDATE %s SET allocated=1,volume_device=?,volume_inode=?,"
                     "data_device=?,data_inode=?,writer_device=?,writer_inode=? WHERE volume_id=? "
                     "AND allocated=0",
                     scylla);
    if (!statement)
        return -1;
    number(statement, 1, proof->volume_device);
    number(statement, 2, proof->volume_inode);
    number(statement, 3, proof->data_device);
    number(statement, 4, proof->data_inode);
    number(statement, 5, proof->writer_device);
    number(statement, 6, proof->writer_inode);
    bind(statement, 7, volume);
    int result = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    sqlite3_finalize(statement);
    return result;
}

static int record_recovery_read(sqlite3 *db, const gstd_volume_root *owner, const char *operation,
                                char volume[65], char producer[65], gstd_volume_identity *expected,
                                int scylla) {
    if (!db || !volume || !producer || !expected || !identifier(operation) || !root(db, owner))
        return -1;
    sqlite3_stmt *s = role_prepare(
        db,
        "SELECT v.volume_id,v.producer_operation_id,v.store_id,v.root_device,v.root_inode,"
        "v.owner_uid,v.volume_device,v.volume_inode,v.data_device,v.data_inode,v.writer_device,v."
        "writer_inode "
        "FROM %s v JOIN resource_ownership r ON r.resource_id=v.volume_id "
        "JOIN operations o ON o.operation_id=r.operation_id AND o.deployment_id=r.deployment_id "
        "WHERE o.operation_id=? AND o.caller_uid=? AND "
        "(o.method='deployment.start' OR (o.method='migration.start' AND "
        "r.resource_type='datastore-volume')) AND "
        "r.resource_type=? AND r.ownership_state IN ('held','uncertain') "
        "AND v.role='%s' AND v.compatibility='%s' AND "
        "v.allocated=1",
        scylla);
    if (!s)
        return -1;
    bind(s, 1, operation);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    bind(s, 3, scylla ? "datastore-volume" : "repository-volume");
    int valid = sqlite3_step(s) == SQLITE_ROW;
    const char *id = valid ? (const char *)sqlite3_column_text(s, 0) : NULL;
    const char *first = valid ? (const char *)sqlite3_column_text(s, 1) : NULL;
    uint64_t device, inode;
    gstd_volume_identity value;
    valid = valid && identifier(id) && identifier(first) && matches(s, 2, owner->store_id) &&
            !read_number(s, 3, &device) && !read_number(s, 4, &inode) && device == owner->device &&
            inode == owner->inode && sqlite3_column_int64(s, 5) == (sqlite3_int64)geteuid() &&
            !identity(s, 6, &value);
    char found[65] = {0}, original[65] = {0};
    if (valid) {
        memcpy(found, id, strlen(id) + 1);
        memcpy(original, first, strlen(first) + 1);
        valid = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    if (!valid)
        return -1;
    memcpy(volume, found, sizeof(found));
    memcpy(producer, original, sizeof(original));
    *expected = value;
    return 0;
}

static int scylla_id(const char *deployment, char volume[65]) {
    json_t *identity = json_pack("{s:s}", "deploymentId", deployment);
    char hash[65];
    int valid = identity && !gstd_fingerprint(identity, hash);
    json_decref(identity);
    if (valid)
        snprintf(volume, 65, "scylla-%.56s", hash);
    return valid ? 0 : -1;
}
static gstd_accept_status volume_owner_reserve(sqlite3 *db, const char *operation,
                                               const char *deployment, char output[65], int role) {
    char volume[65];
    if (role >= 2 ? oidc_id(deployment, role, volume) : scylla_id(deployment, volume))
        return GSTD_ACCEPT_DATABASE;
    sqlite3_stmt *s = prepare(
        db, "SELECT r.deployment_id,r.resource_type,r.operation_id,r.ownership_state,o.caller_uid "
            "FROM resource_ownership r JOIN operations o ON o.operation_id=r.operation_id WHERE "
            "r.resource_id=?");
    if (!s)
        return GSTD_ACCEPT_DATABASE;
    bind(s, 1, volume);
    int step = sqlite3_step(s);
    int exists = step == SQLITE_ROW, replay = 0, available = step == SQLITE_DONE;
    if (exists && matches(s, 0, deployment) && matches(s, 1, "datastore-volume") &&
        sqlite3_column_int64(s, 4) == (sqlite3_int64)geteuid()) {
        replay = matches(s, 2, operation) && matches(s, 3, "held");
        available = matches(s, 3, "released");
    }
    sqlite3_finalize(s);
    if (replay) {
        memcpy(output, volume, strlen(volume) + 1);
        return GSTD_ACCEPT_REPLAY;
    }
    if (!available)
        return exists ? GSTD_ACCEPT_OWNERSHIP_UNCERTAIN : GSTD_ACCEPT_DATABASE;
    s = prepare(
        db, "INSERT INTO resource_ownership "
            "VALUES(?,'datastore-volume',?,?,'reserved',NULL,'held',strftime('%Y-%m-%dT%H:%M:%SZ','"
            "now')) "
            "ON CONFLICT(resource_id) DO UPDATE SET "
            "operation_id=excluded.operation_id,lifecycle_stage='reserved',backend_identity=NULL,"
            "ownership_state='held',observed_at=excluded.observed_at WHERE "
            "resource_ownership.deployment_id=excluded.deployment_id "
            "AND resource_ownership.resource_type='datastore-volume' AND "
            "resource_ownership.ownership_state='released'");
    if (!s)
        return GSTD_ACCEPT_DATABASE;
    bind(s, 1, volume);
    bind(s, 2, deployment);
    bind(s, 3, operation);
    int result = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(s);
    if (!result)
        return GSTD_ACCEPT_DATABASE;
    memcpy(output, volume, strlen(volume) + 1);
    return GSTD_ACCEPT_NEW;
}
gstd_accept_status gstd_scylla_volume_reserve(sqlite3 *db, const char *operation, char output[65]) {
    if (!output)
        return GSTD_ACCEPT_INVALID;
    output[0] = 0;
    if (!db || sqlite3_get_autocommit(db) || !identifier(operation))
        return GSTD_ACCEPT_INVALID;
    sqlite3_stmt *s = prepare(
        db, "SELECT o.deployment_id FROM operations o JOIN deployments d ON "
            "d.deployment_id=o.deployment_id "
            "WHERE o.operation_id=? AND o.caller_uid=? AND o.method='deployment.start' "
            "AND o.state IN ('accepted','running') AND d.last_operation_id=o.operation_id "
            "AND d.desired_state='running' AND d.recovery='normal' AND d.writer_ownership='held' "
            "AND d.profile_id='core' AND d.profile_version='1' AND "
            "json_extract(o.intent,'$.datastore')='scylladb'");
    if (!s)
        return GSTD_ACCEPT_DATABASE;
    bind(s, 1, operation);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    int step = sqlite3_step(s);
    const char *found = step == SQLITE_ROW ? (const char *)sqlite3_column_text(s, 0) : NULL;
    char deployment[65] = {0};
    if (identifier(found))
        snprintf(deployment, sizeof(deployment), "%s", found);
    sqlite3_finalize(s);
    if (step != SQLITE_ROW || !*deployment)
        return step == SQLITE_DONE ? GSTD_ACCEPT_NOT_FOUND : GSTD_ACCEPT_DATABASE;
    return volume_owner_reserve(db, operation, deployment, output, 1);
}
gstd_accept_status gstd_scylla_migration_volume_reserve(sqlite3 *db, const char *operation,
                                                        char output[65]) {
    if (!output)
        return GSTD_ACCEPT_INVALID;
    output[0] = 0;
    gstd_migration_execution execution = {0};
    if (gstd_migration_execution_read(db, operation, &execution))
        return GSTD_ACCEPT_MIGRATION_PRECONDITION;
    char volume[65];
    sqlite3_stmt *s = NULL;
    int valid =
        !scylla_id(execution.target.deployment_id, volume) &&
        sqlite3_prepare_v2(
            db,
            "SELECT r.operation_id,o.method FROM datastore_volumes v JOIN resource_ownership r "
            "ON r.resource_id=v.volume_id JOIN operations o ON o.operation_id=r.operation_id "
            "WHERE v.volume_id=? AND v.role='scylladb' AND v.allocated=1 AND v.owner_uid=? "
            "AND v.store_id=? AND v.compatibility='scylla-2026.1:gitstore-06152d-schema-012' "
            "AND r.resource_type='datastore-volume' AND r.deployment_id=? AND o.caller_uid=? "
            "AND (((r.operation_id=? OR (o.method='migration.start' AND "
            "o.state IN ('succeeded','failed') AND o.recovery='normal')) "
            "AND r.ownership_state='released') OR "
            "(r.operation_id=? AND r.ownership_state='held'))",
            -1, &s, NULL) == SQLITE_OK;
    if (valid) {
        bind(s, 1, volume);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
        bind(s, 3, execution.target.store_id);
        bind(s, 4, execution.target.deployment_id);
        sqlite3_bind_int64(s, 5, (sqlite3_int64)geteuid());
        bind(s, 6, execution.source_operation);
        bind(s, 7, operation);
        valid = sqlite3_step(s) == SQLITE_ROW;
        if (valid && matches(s, 1, "migration.start") && !matches(s, 0, operation)) {
            const char *prior_operation = (const char *)sqlite3_column_text(s, 0);
            json_t *prior = gstd_migration_inspect(db, prior_operation),
                   *current = gstd_migration_inspect(db, operation);
            const char *stage = json_string_value(json_object_get(prior, "stage"));
            valid = prior && current && stage &&
                    (!strcmp(stage, "succeeded") || !strcmp(stage, "failed")) &&
                    json_equal(json_object_get(prior, "sourceOperationId"),
                               json_object_get(current, "sourceOperationId")) &&
                    json_equal(json_object_get(prior, "targetDigest"),
                               json_object_get(current, "targetDigest"));
            json_decref(prior);
            json_decref(current);
        }
        valid = valid && sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    gstd_accept_status status =
        valid ? volume_owner_reserve(db, operation, execution.target.deployment_id, output, 1)
              : GSTD_ACCEPT_MIGRATION_PRECONDITION;
    gstd_migration_execution_clear(&execution);
    return status;
}

int gstd_volume_record_begin(sqlite3 *db, const gstd_volume_root *root, const char *volume,
                             const char *operation, char producer[65],
                             gstd_volume_identity *expected) {
    return record_begin(db, root, volume, operation, producer, expected, 0);
}
int gstd_scylla_volume_record_begin(sqlite3 *db, const gstd_volume_root *root, const char *volume,
                                    const char *operation, char producer[65],
                                    gstd_volume_identity *expected) {
    return record_begin(db, root, volume, operation, producer, expected, 1);
}
int gstd_volume_record_ready(sqlite3 *db, const gstd_volume_root *root, const char *volume,
                             const char *operation, const gstd_volume_handle *handle) {
    return record_ready(db, root, volume, operation, handle, 0);
}
int gstd_scylla_volume_record_ready(sqlite3 *db, const gstd_volume_root *root, const char *volume,
                                    const char *operation, const gstd_volume_handle *handle) {
    return record_ready(db, root, volume, operation, handle, 1);
}
int gstd_volume_record_recovery_read(sqlite3 *db, const gstd_volume_root *root,
                                     const char *operation, char volume[65], char producer[65],
                                     gstd_volume_identity *expected) {
    return record_recovery_read(db, root, operation, volume, producer, expected, 0);
}
int gstd_scylla_volume_record_recovery_read(sqlite3 *db, const gstd_volume_root *root,
                                            const char *operation, char volume[65],
                                            char producer[65], gstd_volume_identity *expected) {
    return record_recovery_read(db, root, operation, volume, producer, expected, 1);
}

static int oidc_id(const char *deployment, int role, char volume[65]) {
    if (!identifier(deployment) || (role != 2 && role != 3))
        return -1;
    const char *provider = role == 2 ? "hydra" : "kratos";
    json_t *identity = json_pack("{s:s,s:s}", "deploymentId", deployment, "oidcProvider", provider);
    char hash[65];
    int valid = identity && !gstd_fingerprint(identity, hash);
    json_decref(identity);
    if (valid)
        snprintf(volume, 65, "oidc-%s-%.48s", provider, hash);
    return valid ? 0 : -1;
}
static int oidc_current_deployment(sqlite3 *db, const char *operation, char deployment[65]) {
    sqlite3_stmt *s = prepare(
        db, "SELECT d.deployment_id FROM operations o JOIN deployments d ON "
            "d.deployment_id=o.deployment_id WHERE o.operation_id=? AND o.caller_uid=? AND "
            "o.method='deployment.start' AND o.state IN ('accepted','running') AND "
            "d.last_operation_id=o.operation_id AND d.profile_id='core' AND d.profile_version='2' "
            "AND d.desired_state='running' AND d.recovery='normal' AND d.writer_ownership='held' "
            "AND EXISTS(SELECT 1 FROM json_each(o.intent,'$.components') WHERE value='oidc')");
    if (!s)
        return -1;
    bind(s, 1, operation);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    int valid = sqlite3_step(s) == SQLITE_ROW;
    const char *value = valid ? (const char *)sqlite3_column_text(s, 0) : NULL;
    valid = valid && identifier(value);
    if (valid)
        snprintf(deployment, 65, "%s", value);
    valid = valid && sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
gstd_accept_status gstd_oidc_volume_reserve(sqlite3 *db, gstd_oidc_volume_role role,
                                            const char *operation, char output[65]) {
    if (!output)
        return GSTD_ACCEPT_INVALID;
    output[0] = 0;
    if (!db || sqlite3_get_autocommit(db) || !identifier(operation) ||
        !gstd_oidc_volume_compatibility(role))
        return GSTD_ACCEPT_INVALID;
    char deployment[65];
    if (oidc_current_deployment(db, operation, deployment))
        return GSTD_ACCEPT_NOT_FOUND;
    return volume_owner_reserve(db, operation, deployment, output, (int)role + 2);
}
int gstd_oidc_volume_record_begin(sqlite3 *db, const gstd_volume_root *root,
                                  gstd_oidc_volume_role role, const char *volume,
                                  const char *operation, char producer[65],
                                  gstd_volume_identity *expected) {
    if (!gstd_oidc_volume_compatibility(role))
        return -1;
    return record_begin(db, root, volume, operation, producer, expected, (int)role + 2);
}
int gstd_oidc_volume_record_ready(sqlite3 *db, const gstd_volume_root *root,
                                  gstd_oidc_volume_role role, const char *volume,
                                  const char *operation, const gstd_volume_handle *handle) {
    if (!gstd_oidc_volume_compatibility(role))
        return -1;
    return record_ready(db, root, volume, operation, handle, (int)role + 2);
}
int gstd_oidc_volume_record_recovery_read(sqlite3 *db, const gstd_volume_root *root,
                                          gstd_oidc_volume_role role, const char *operation,
                                          char volume[65], char producer[65],
                                          gstd_volume_identity *expected) {
    if (!gstd_oidc_volume_compatibility(role))
        return -1;
    return record_recovery_read(db, root, operation, volume, producer, expected, (int)role + 2);
}
