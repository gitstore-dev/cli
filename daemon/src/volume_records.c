#define _POSIX_C_SOURCE 200809L
#include "volume_records.h"
#include "gitstored/protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int identifier(const char *text) { return text && gstd_identifier(text, strnlen(text, 65)); }
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = NULL;
    return sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK ? statement : NULL;
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
static int authorized(sqlite3 *db, const char *volume, const char *operation) {
    sqlite3_stmt *statement = prepare(
        db,
        "SELECT o.caller_uid FROM resource_ownership r JOIN operations o "
        "ON r.operation_id=o.operation_id AND r.deployment_id=o.deployment_id WHERE "
        "r.resource_id=? AND r.resource_type='repository-volume' AND r.ownership_state='held' AND "
        "r.lifecycle_stage IN ('reserved','allocated','attached','running') AND o.operation_id=? "
        "AND "
        "o.method='deployment.start' AND o.state IN ('accepted','running')");
    if (!statement)
        return 0;
    bind(statement, 1, volume);
    bind(statement, 2, operation);
    int valid = sqlite3_step(statement) == SQLITE_ROW &&
                sqlite3_column_int64(statement, 0) == (sqlite3_int64)geteuid() &&
                sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return valid;
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
int gstd_volume_record_begin(sqlite3 *db, const gstd_volume_root *owner, const char *volume,
                             const char *operation, char producer[65],
                             gstd_volume_identity *expected) {
    if (!producer || !expected)
        return -1;
    producer[0] = 0;
    memset(expected, 0, sizeof(*expected));
    if (!db || sqlite3_get_autocommit(db) || !identifier(volume) || !identifier(operation) ||
        !root(db, owner) || !authorized(db, volume, operation))
        return -1;
    sqlite3_stmt *statement =
        prepare(db, "SELECT producer_operation_id,store_id,root_device,root_inode,"
                    "owner_uid,allocated,volume_device,volume_inode,data_device,data_inode,writer_"
                    "device,writer_inode "
                    "FROM repository_volumes WHERE volume_id=?");
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
            if (result == 1 && identity(statement, 6, expected))
                result = -1;
            if (result == 0 || result == 1)
                snprintf(producer, 65, "%s", first);
        }
        sqlite3_finalize(statement);
        return result;
    }
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE)
        return -1;
    statement =
        prepare(db, "INSERT INTO repository_volumes(volume_id,role,owner_uid,producer_operation_id,"
                    "store_id,root_device,root_inode,compatibility) "
                    "VALUES(?,'repositories',?,?,?,?,?,'gitstore-repositories-v1')");
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
int gstd_volume_record_ready(sqlite3 *db, const gstd_volume_root *owner, const char *volume,
                             const char *operation, const gstd_volume_handle *handle) {
    if (!db || sqlite3_get_autocommit(db) || !handle || !root(db, owner) || !identifier(volume) ||
        !identifier(operation) || !authorized(db, volume, operation))
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
    sqlite3_stmt *reserved = prepare(db, "SELECT 1 FROM repository_volumes WHERE volume_id=?");
    if (!reserved)
        return -1;
    bind(reserved, 1, volume);
    int exists = sqlite3_step(reserved) == SQLITE_ROW;
    sqlite3_finalize(reserved);
    if (!exists)
        return -1;
    char producer[65];
    gstd_volume_identity expected;
    int state = gstd_volume_record_begin(db, owner, volume, operation, producer, &expected);
    if (state < 0)
        return -1;
    if (state == 1)
        return equal(&expected, proof) ? 0 : -1;
    sqlite3_stmt *statement =
        prepare(db, "UPDATE repository_volumes SET allocated=1,volume_device=?,volume_inode=?,"
                    "data_device=?,data_inode=?,writer_device=?,writer_inode=? WHERE volume_id=? "
                    "AND allocated=0");
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

int gstd_volume_record_recovery_read(sqlite3 *db, const gstd_volume_root *owner,
                                     const char *operation, char volume[65], char producer[65],
                                     gstd_volume_identity *expected) {
    if (!db || !volume || !producer || !expected || !identifier(operation) || !root(db, owner))
        return -1;
    sqlite3_stmt *s = prepare(
        db,
        "SELECT v.volume_id,v.producer_operation_id,v.store_id,v.root_device,v.root_inode,"
        "v.owner_uid,v.volume_device,v.volume_inode,v.data_device,v.data_inode,v.writer_device,v."
        "writer_inode "
        "FROM repository_volumes v JOIN resource_ownership r ON r.resource_id=v.volume_id "
        "JOIN operations o ON o.operation_id=r.operation_id AND o.deployment_id=r.deployment_id "
        "WHERE o.operation_id=? AND o.caller_uid=? AND o.method='deployment.start' AND "
        "r.resource_type='repository-volume' AND r.ownership_state IN ('held','uncertain') "
        "AND v.role='repositories' AND v.compatibility='gitstore-repositories-v1' AND "
        "v.allocated=1");
    if (!s)
        return -1;
    bind(s, 1, operation);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
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
