#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "logs.h"
#include "gitstored/protocol.h"
#include "oidc_profile.h"
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct gstd_logs {
    sqlite3 *db;
    gstd_log_limits limits;
    char epoch[33];
    int readonly;
};
static int sql(gstd_logs *logs, const char *text) {
    return sqlite3_exec(logs->db, text, NULL, NULL, NULL);
}
static sqlite3_stmt *prepare(gstd_logs *logs, const char *text) {
    sqlite3_stmt *s = NULL;
    return sqlite3_prepare_v2(logs->db, text, -1, &s, NULL) == SQLITE_OK ? s : NULL;
}
static int scalar(gstd_logs *logs, const char *text, sqlite3_int64 *output) {
    sqlite3_stmt *s = prepare(logs, text);
    int valid = s && sqlite3_step(s) == SQLITE_ROW;
    if (valid)
        *output = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
static void bind(sqlite3_stmt *s, int index, const char *text) {
    sqlite3_bind_text(s, index, text, -1, SQLITE_TRANSIENT);
}
static int identifier(const char *value) {
    return value && gstd_identifier(value, strnlen(value, 65));
}
static int component_valid(const char *name) {
    if (gstd_oidc_component(name))
        return 1;
    const char *names[] = {"api", "git", "controller", "oidc", "admin", "storefront", "scylladb"};
    if (!name)
        return 1;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}
static int valid_time(int64_t now) { return now >= 0 && now <= INT64_C(253402300799); }
static int valid_kind(gstd_stream_kind kind) {
    return kind == GSTD_STREAM_LOGS || kind == GSTD_STREAM_EVENTS_KIND;
}
static int rollback(gstd_logs *logs) {
    sql(logs, "ROLLBACK");
    return sqlite3_get_autocommit(logs->db) ? 0 : -1;
}
static const char schema[] =
    "CREATE TABLE log_meta(epoch TEXT NOT NULL CHECK(length(epoch)=32),bytes INTEGER NOT NULL "
    "DEFAULT 0,records INTEGER NOT NULL DEFAULT 0,segments INTEGER NOT NULL DEFAULT 0) STRICT;"
    "INSERT INTO log_meta(epoch) VALUES(lower(hex(randomblob(16))));"
    "CREATE TABLE log_segments(id INTEGER PRIMARY KEY AUTOINCREMENT,deployment TEXT NOT "
    "NULL,created INTEGER NOT NULL,bytes INTEGER NOT NULL DEFAULT 0) STRICT;"
    "CREATE INDEX log_segments_deployment ON log_segments(deployment,id);"
    "CREATE TABLE log_entries(sequence INTEGER PRIMARY KEY AUTOINCREMENT "
    "CHECK(sequence<=9007199254740991),segment INTEGER NOT NULL REFERENCES log_segments(id) ON "
    "DELETE CASCADE,component TEXT NOT NULL,kind INTEGER NOT NULL CHECK(kind IN (0,1)),observed "
    "INTEGER NOT NULL,payload TEXT NOT NULL CHECK(json_valid(payload)),charge INTEGER NOT NULL "
    "CHECK(charge>0)) STRICT;"
    "CREATE INDEX log_entries_segment ON log_entries(segment,sequence);"
    "CREATE TRIGGER log_entry_insert AFTER INSERT ON log_entries BEGIN UPDATE log_segments SET "
    "bytes=bytes+NEW.charge WHERE id=NEW.segment;UPDATE log_meta SET "
    "bytes=bytes+NEW.charge,records=records+1;END;"
    "CREATE TRIGGER log_entry_delete AFTER DELETE ON log_entries BEGIN UPDATE log_segments SET "
    "bytes=bytes-OLD.charge WHERE id=OLD.segment;UPDATE log_meta SET "
    "bytes=bytes-OLD.charge,records=records-1;END;"
    "CREATE TRIGGER log_entry_immutable BEFORE UPDATE ON log_entries BEGIN SELECT "
    "RAISE(ABORT,'retained log entry is immutable');END;"
    "CREATE TRIGGER log_segment_insert AFTER INSERT ON log_segments BEGIN UPDATE log_meta SET "
    "segments=segments+1;END;"
    "CREATE TRIGGER log_segment_delete AFTER DELETE ON log_segments BEGIN UPDATE log_meta SET "
    "segments=segments-1;END;"
    "PRAGMA user_version=1;";
static gstd_log_status open_file(const char *path, const gstd_log_limits *limits, int allow_create,
                                 gstd_logs **output) {
    if (!output)
        return GSTD_LOG_INVALID;
    *output = NULL;
    gstd_log_limits hard = GSTD_LOG_LIMITS, actual = limits ? *limits : hard;
    if (!path || actual.segment_bytes < 256 || actual.segment_bytes > hard.segment_bytes ||
        actual.deployment_bytes < actual.segment_bytes ||
        actual.deployment_bytes > hard.deployment_bytes ||
        actual.global_bytes < actual.deployment_bytes || actual.global_bytes > hard.global_bytes ||
        !actual.segments || actual.segments > hard.segments || !actual.records ||
        actual.records > hard.records || !actual.max_age_seconds ||
        actual.max_age_seconds > hard.max_age_seconds)
        return GSTD_LOG_INVALID;
    int fd =
        open(path, (allow_create ? O_CREAT | O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat metadata;
    int valid = fd >= 0 && !fstat(fd, &metadata) && S_ISREG(metadata.st_mode) &&
                metadata.st_uid == geteuid() && metadata.st_nlink == 1 &&
                (metadata.st_mode & 07777) == 0600;
    if (fd >= 0)
        close(fd);
    if (!valid)
        return GSTD_LOG_IO;
    gstd_logs *logs = calloc(1, sizeof(*logs));
    if (!logs)
        return GSTD_LOG_IO;
    logs->limits = actual;
    logs->readonly = !allow_create;
    if (sqlite3_open_v2(path, &logs->db,
                        (allow_create ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) |
                            SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_EXRESCODE,
                        NULL) != SQLITE_OK)
        goto fail;
    sqlite3_db_config(logs->db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
    sqlite3_db_config(logs->db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    sqlite3_limit(logs->db, SQLITE_LIMIT_ATTACHED, 0);
    sqlite3_limit(logs->db, SQLITE_LIMIT_LENGTH, 1048576);
    sqlite3_busy_timeout(logs->db, 100);
    sqlite3_int64 version, count, page_size;
    if (scalar(logs, "PRAGMA page_size", &page_size) || page_size != 4096)
        goto fail;
    if (scalar(logs, "PRAGMA user_version", &version) || version < 0 || version > 1 ||
        (!allow_create && version != 1))
        goto fail;
    if (!version &&
        (scalar(logs, "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'",
                &count) ||
         count))
        goto fail;
    /* Do not change journal/settings on a foreign version-1 metadata store. */
    if (version && (scalar(logs, "SELECT count(*) FROM log_meta", &count) || count != 1))
        goto fail;
    if (allow_create && sql(logs, "PRAGMA foreign_keys=ON;PRAGMA journal_mode=DELETE;PRAGMA "
                                  "synchronous=EXTRA;PRAGMA max_page_count=131072;PRAGMA "
                                  "journal_size_limit=0;PRAGMA temp_store=MEMORY") != SQLITE_OK)
        goto fail;
    if (sql(logs, "PRAGMA foreign_keys=ON;PRAGMA temp_store=MEMORY;BEGIN") != SQLITE_OK)
        goto fail;
    if (!version && sql(logs, schema) != SQLITE_OK)
        goto transaction_fail;
    sqlite3_stmt *s = prepare(logs, "SELECT epoch FROM log_meta");
    valid = s && sqlite3_step(s) == SQLITE_ROW && sqlite3_column_bytes(s, 0) == 32;
    if (valid) {
        const char *epoch = (const char *)sqlite3_column_text(s, 0);
        for (unsigned i = 0; i < 32; i++)
            if (!((epoch[i] >= '0' && epoch[i] <= '9') || (epoch[i] >= 'a' && epoch[i] <= 'f')))
                valid = 0;
        if (valid)
            memcpy(logs->epoch, epoch, 32);
    }
    sqlite3_finalize(s);
    if (!valid ||
        scalar(logs,
               "SELECT count(*) FROM log_meta WHERE bytes=(SELECT coalesce(sum(charge),0) FROM "
               "log_entries) AND records=(SELECT count(*) FROM log_entries) AND segments=(SELECT "
               "count(*) FROM log_segments)",
               &count) ||
        count != 1 || sql(logs, "COMMIT") != SQLITE_OK)
        goto transaction_fail;
    *output = logs;
    return GSTD_LOG_OK;
transaction_fail:
    rollback(logs);
fail:
    gstd_logs_close(logs);
    return GSTD_LOG_IO;
}
gstd_log_status gstd_logs_open(const char *path, const gstd_log_limits *limits,
                               gstd_logs **output) {
    return open_file(path, limits, 1, output);
}
static int private_directory(int fd, struct stat *info) {
    return fd >= 3 && !fstat(fd, info) && S_ISDIR(info->st_mode) && info->st_uid == geteuid() &&
           (info->st_mode & 07777) == 0700;
}
int gstd_logs_directory(int state, const char *store, int create) {
    struct stat parent, directory, named, file;
    if (!store || strnlen(store, 33) != 32 || (create != 0 && create != 1) ||
        !private_directory(state, &parent))
        return -1;
    for (unsigned i = 0; i < 32; i++)
        if (!((store[i] >= '0' && store[i] <= '9') || (store[i] >= 'a' && store[i] <= 'f')))
            return -1;
    int root = openat(state, "logs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0 && errno == ENOENT && create) {
        if (mkdirat(state, "logs", 0700) && errno != EEXIST)
            return -1;
        root = openat(state, "logs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (!private_directory(root, &directory)) {
        if (root >= 0)
            close(root);
        return -1;
    }
    int fresh = 0, marker = -1;
    if (create) {
        marker = openat(root, ".gitstored-logs", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                        0600);
        fresh = marker >= 3;
    }
    if (marker < 0)
        marker = openat(root, ".gitstored-logs", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    int locked = 0;
    for (unsigned i = 0; marker >= 3 && i < 100; i++) {
        if (!flock(marker, LOCK_EX | LOCK_NB)) {
            locked = 1;
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    char expected[80], actual[80];
    int length = snprintf(expected, sizeof(expected), "gitstored-logs-v1\n%s\n", store);
    int valid = locked;
    if (valid && fresh)
        valid = write(marker, expected, (size_t)length) == length && !fsync(marker) &&
                !fsync(root) && !fsync(state);
    valid = valid && !fstat(marker, &file) && S_ISREG(file.st_mode) && file.st_uid == geteuid() &&
            (file.st_mode & 07777) == 0600 && file.st_nlink == 1 && file.st_size == length &&
            pread(marker, actual, sizeof(actual), 0) == length &&
            !memcmp(actual, expected, (size_t)length) &&
            !fstatat(state, "logs", &named, AT_SYMLINK_NOFOLLOW) && S_ISDIR(named.st_mode) &&
            named.st_dev == directory.st_dev && named.st_ino == directory.st_ino;
    if (marker >= 0)
        close(marker);
    if (!valid) {
        close(root);
        return -1;
    }
    return root;
}
static int reopen_directory(const char *path) {
    if (!path || path[0] != '/' || strlen(path) > 4095)
        return -1;
    char *copy = strdup(path);
    if (!copy)
        return -1;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    char *saved = NULL;
    for (char *part = strtok_r(copy, "/", &saved); part && fd >= 0;
         part = strtok_r(NULL, "/", &saved)) {
        struct stat parent;
        if (!strcmp(part, ".") || !strcmp(part, "..") || fstat(fd, &parent) ||
            (parent.st_uid != 0 && parent.st_uid != geteuid()) ||
            ((parent.st_mode & 0022) && !(parent.st_uid == 0 && (parent.st_mode & S_ISVTX)))) {
            close(fd);
            fd = -1;
            break;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = next;
    }
    free(copy);
    return fd;
}
gstd_log_status gstd_logs_open_at(int directory, const gstd_log_limits *limits, int allow_create,
                                  gstd_logs **output) {
    if (!output)
        return GSTD_LOG_INVALID;
    *output = NULL;
    struct stat held, named;
    if (!private_directory(directory, &held))
        return GSTD_LOG_IO;
    char path[4096], link[64];
#if defined(__linux__)
    snprintf(link, sizeof(link), "/proc/self/fd/%d", directory);
    ssize_t length = readlink(link, path, sizeof(path) - 32);
    if (length <= 0 || length >= (ssize_t)sizeof(path) - 32)
        return GSTD_LOG_IO;
    path[length] = 0;
#elif defined(__APPLE__)
    (void)link;
    if (fcntl(directory, F_GETPATH, path))
        return GSTD_LOG_IO;
    size_t length = strlen(path);
    if (length >= sizeof(path) - 32)
        return GSTD_LOG_IO;
#else
    (void)link;
    (void)limits;
    return GSTD_LOG_IO;
#endif
    int reopened = reopen_directory(path);
    int valid = private_directory(reopened, &named) && named.st_dev == held.st_dev &&
                named.st_ino == held.st_ino;
    if (reopened >= 0)
        close(reopened);
    if (!valid)
        return GSTD_LOG_IO;
    char parent[4096];
    memcpy(parent, path, strlen(path) + 1);
    strcat(path, "/segments.db");
    gstd_log_status status = open_file(path, limits, allow_create, output);
    reopened = reopen_directory(parent);
    valid = private_directory(reopened, &named) && named.st_dev == held.st_dev &&
            named.st_ino == held.st_ino;
    if (reopened >= 0)
        close(reopened);
    if (!valid) {
        gstd_logs_close(*output);
        *output = NULL;
        return GSTD_LOG_IO;
    }
    return status;
}
void gstd_logs_close(gstd_logs *logs) {
    if (logs) {
        if (logs->db)
            sqlite3_close(logs->db);
        free(logs);
    }
}
static int expire(gstd_logs *logs, int64_t now) {
    sqlite3_stmt *s = prepare(logs, "DELETE FROM log_segments WHERE created<?");
    if (!s)
        return -1;
    sqlite3_bind_int64(s, 1, now - (int64_t)logs->limits.max_age_seconds);
    int valid = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
gstd_log_status gstd_logs_prune(gstd_logs *logs, int64_t now) {
    if (!logs || logs->readonly || !valid_time(now) || !sqlite3_get_autocommit(logs->db))
        return GSTD_LOG_INVALID;
    if (sql(logs, "BEGIN IMMEDIATE") != SQLITE_OK)
        return GSTD_LOG_IO;
    if (expire(logs, now) || sql(logs, "COMMIT") != SQLITE_OK) {
        rollback(logs);
        return GSTD_LOG_IO;
    }
    return GSTD_LOG_OK;
}
static int oldest(gstd_logs *logs, const char *deployment) {
    sqlite3_stmt *s =
        prepare(logs, deployment ? "DELETE FROM log_segments WHERE id=(SELECT id FROM log_segments "
                                   "WHERE deployment=? ORDER BY id LIMIT 1)"
                                 : "DELETE FROM log_segments WHERE id=(SELECT id FROM log_segments "
                                   "ORDER BY id LIMIT 1)");
    if (!s)
        return -1;
    if (deployment)
        bind(s, 1, deployment);
    int valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(logs->db) == 1;
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
static int append(gstd_logs *logs, const char *deployment, const char *component,
                  gstd_stream_kind kind, const char *payload, size_t charge, int64_t now) {
    if (sql(logs, "BEGIN IMMEDIATE") != SQLITE_OK)
        return SQLITE_IOERR;
    if (expire(logs, now))
        goto fail;
    sqlite3_int64 segment = 0;
    for (unsigned attempt = 0; attempt <= logs->limits.segments; attempt++) {
        sqlite3_stmt *s = prepare(
            logs, "SELECT m.bytes,m.records,m.segments,coalesce((SELECT sum(bytes) FROM "
                  "log_segments WHERE deployment=?),0),coalesce((SELECT id FROM log_segments WHERE "
                  "deployment=? AND bytes+?<=? ORDER BY id DESC LIMIT 1),0) FROM log_meta m");
        if (!s)
            goto fail;
        bind(s, 1, deployment);
        bind(s, 2, deployment);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)charge);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)logs->limits.segment_bytes);
        int valid = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_int64 values[5] = {0};
        if (valid)
            for (unsigned i = 0; i < 5; i++)
                values[i] = sqlite3_column_int64(s, (int)i);
        sqlite3_finalize(s);
        if (!valid)
            goto fail;
        segment = values[4];
        int per_deployment =
            values[3] + (sqlite3_int64)charge > (sqlite3_int64)logs->limits.deployment_bytes;
        int full = values[0] + (sqlite3_int64)charge > (sqlite3_int64)logs->limits.global_bytes ||
                   values[1] >= logs->limits.records ||
                   (!segment && values[2] >= logs->limits.segments);
        if (!per_deployment && !full)
            break;
        if (attempt == logs->limits.segments || oldest(logs, per_deployment ? deployment : NULL))
            goto fail;
    }
    if (!segment) {
        sqlite3_stmt *s = prepare(logs, "INSERT INTO log_segments(deployment,created) VALUES(?,?)");
        if (!s)
            goto fail;
        bind(s, 1, deployment);
        sqlite3_bind_int64(s, 2, now);
        int valid = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        if (!valid)
            goto fail;
        segment = sqlite3_last_insert_rowid(logs->db);
    }
    sqlite3_stmt *s =
        prepare(logs, "INSERT INTO log_entries(segment,component,kind,observed,payload,charge) "
                      "VALUES(?,?,?,?,?,?)");
    if (!s)
        goto fail;
    sqlite3_bind_int64(s, 1, segment);
    bind(s, 2, component);
    sqlite3_bind_int(s, 3, (int)kind);
    sqlite3_bind_int64(s, 4, now);
    bind(s, 5, payload);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)charge);
    int valid = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!valid)
        goto fail;
    /* Commit errors have uncertain durability and are never retried blindly. */
    if (sql(logs, "COMMIT") != SQLITE_OK) {
        rollback(logs);
        return SQLITE_IOERR;
    }
    return SQLITE_OK;
fail: {
    int code = sqlite3_errcode(logs->db) & 255;
    if (rollback(logs))
        return SQLITE_IOERR;
    return code == SQLITE_FULL ? SQLITE_FULL : SQLITE_IOERR;
}
}
gstd_log_status gstd_logs_append(gstd_logs *logs, const char *deployment, const char *component,
                                 gstd_stream_kind kind, const json_t *payload, int64_t now) {
    if (!logs || !identifier(deployment) || !component || !component_valid(component) ||
        !valid_kind(kind) || !valid_time(now) || !json_is_object(payload) ||
        json_object_get(payload, "cursor") || !sqlite3_get_autocommit(logs->db))
        return GSTD_LOG_INVALID;
    char *bytes = gstd_json_dump(payload);
    if (!bytes)
        return GSTD_LOG_INVALID;
    size_t size = strlen(bytes), charge = size + 128;
    json_t *checked = gstd_json_parse((const unsigned char *)bytes, size);
    int bounded = json_is_object(checked);
    json_decref(checked);
    if (!bounded) {
        free(bytes);
        return GSTD_LOG_INVALID;
    }
    if (size > 131072 || charge > logs->limits.segment_bytes) {
        free(bytes);
        return GSTD_LOG_LIMIT;
    }
    int status = append(logs, deployment, component, kind, bytes, charge, now);
    if (status == SQLITE_FULL && sql(logs, "BEGIN IMMEDIATE") == SQLITE_OK) {
        if (!oldest(logs, NULL) && sql(logs, "COMMIT") == SQLITE_OK)
            status = append(logs, deployment, component, kind, bytes, charge, now);
        else
            rollback(logs);
    }
    free(bytes);
    return status == SQLITE_OK ? GSTD_LOG_OK : GSTD_LOG_IO;
}
static int cursor_read(gstd_logs *logs, const char *cursor, sqlite3_int64 *sequence) {
    char prefix[40];
    snprintf(prefix, sizeof(prefix), "log:%s:", logs->epoch);
    size_t length = strnlen(cursor, 65), start = strlen(prefix);
    if (length <= start || length > start + 14 || strncmp(cursor, prefix, start) ||
        cursor[start] == '0')
        return -1;
    for (size_t i = start; i < length; i++)
        if (!((cursor[i] >= '0' && cursor[i] <= '9') || (cursor[i] >= 'a' && cursor[i] <= 'f')))
            return -1;
    errno = 0;
    unsigned long long value = strtoull(cursor + start, NULL, 16);
    if (errno || !value || value > (unsigned long long)GSTD_SAFE_INTEGER)
        return -1;
    *sequence = (sqlite3_int64)value;
    return 0;
}
gstd_log_status gstd_logs_replay(gstd_logs *logs, const char *deployment, const char *component,
                                 gstd_stream_kind kind, const char *cursor, unsigned limit,
                                 int64_t now, json_t **records) {
    if (!records)
        return GSTD_LOG_INVALID;
    *records = NULL;
    if (!logs || !identifier(deployment) || !component_valid(component) || !valid_kind(kind) ||
        !limit || limit > 256 || !valid_time(now))
        return GSTD_LOG_INVALID;
    if (!logs->readonly && gstd_logs_prune(logs, now) != GSTD_LOG_OK)
        return GSTD_LOG_IO;
    sqlite3_int64 sequence = 0;
    if (cursor) {
        if (cursor_read(logs, cursor, &sequence))
            return GSTD_LOG_CURSOR_EXPIRED;
        sqlite3_stmt *s =
            prepare(logs, "SELECT 1 FROM log_entries e JOIN log_segments s ON "
                          "s.id=e.segment WHERE e.sequence=? AND s.deployment=? AND s.created>=?");
        if (!s)
            return GSTD_LOG_IO;
        sqlite3_bind_int64(s, 1, sequence);
        bind(s, 2, deployment);
        sqlite3_bind_int64(s, 3, now - (int64_t)logs->limits.max_age_seconds);
        int found = sqlite3_step(s);
        sqlite3_finalize(s);
        if (found != SQLITE_ROW)
            return found == SQLITE_DONE ? GSTD_LOG_CURSOR_EXPIRED : GSTD_LOG_IO;
    }
    sqlite3_stmt *s = prepare(
        logs,
        "SELECT e.sequence,e.component,e.observed,e.payload FROM log_entries e JOIN "
        "log_segments s ON s.id=e.segment WHERE s.deployment=? AND e.sequence>? AND "
        "e.kind=? AND (?4 IS NULL OR e.component=?4 OR (?4='oidc' AND "
        "e.component IN ('oidc-smtp','oidc-hydra','oidc-kratos','oidc-ui','oidc-bridge'))) "
        "AND s.created>=?5 ORDER BY e.sequence LIMIT ?6");
    if (!s)
        return GSTD_LOG_IO;
    bind(s, 1, deployment);
    sqlite3_bind_int64(s, 2, sequence);
    sqlite3_bind_int(s, 3, (int)kind);
    if (component) {
        bind(s, 4, component);
    } else {
        sqlite3_bind_null(s, 4);
    }
    sqlite3_bind_int64(s, 5, now - (int64_t)logs->limits.max_age_seconds);
    sqlite3_bind_int(s, 6, (int)limit);
    json_t *array = json_array();
    int step = SQLITE_ERROR, valid = array != NULL, page_full = 0;
    size_t replay_bytes = 0;
    while (valid && (step = sqlite3_step(s)) == SQLITE_ROW) {
        const unsigned char *bytes = sqlite3_column_text(s, 3);
        int size = sqlite3_column_bytes(s, 3);
        if (size > 0 && (size_t)size + 512 > GSTD_STREAM_BYTES - replay_bytes) {
            page_full = 1;
            break;
        }
        if (size > 0)
            replay_bytes += (size_t)size + 512;
        json_t *record =
            bytes && size > 0 && size <= 131072 ? gstd_json_parse(bytes, (size_t)size) : NULL;
        char id[65], timestamp[32];
        snprintf(id, sizeof(id), "log:%s:%llx", logs->epoch,
                 (unsigned long long)sqlite3_column_int64(s, 0));
        time_t observed = (time_t)sqlite3_column_int64(s, 2);
        struct tm utc;
        valid = json_is_object(record) && gmtime_r(&observed, &utc) &&
                strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) &&
                !json_object_set_new(record, "cursor", json_string(id)) &&
                !json_object_set_new(record, "component",
                                     json_string((const char *)sqlite3_column_text(s, 1))) &&
                !json_object_set_new(record, "observedAt", json_string(timestamp)) &&
                !json_array_append(array, record);
        json_decref(record);
    }
    sqlite3_finalize(s);
    if (!valid || (step != SQLITE_DONE && !page_full)) {
        json_decref(array);
        return GSTD_LOG_IO;
    }
    *records = array;
    return GSTD_LOG_OK;
}
size_t gstd_logs_retained_bytes(gstd_logs *logs) {
    sqlite3_int64 bytes;
    return logs && !scalar(logs, "SELECT bytes FROM log_meta", &bytes) && bytes >= 0 ? (size_t)bytes
                                                                                     : 0;
}
