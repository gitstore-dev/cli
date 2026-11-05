#define _GNU_SOURCE 1
#include "migration_history.h"
#include "gitstored/protocol.h"
#include "migration.h"
#include <stdint.h>
#include <string.h>
#ifdef __linux__
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
static int exact_field(const json_t *row, const char *field, const char *expected) {
    json_t *value = json_object_get(row, field);
    return json_is_object(row) && json_object_size(row) == 1 && json_is_string(value) &&
           json_string_length(value) == strlen(expected) &&
           !memcmp(json_string_value(value), expected, strlen(expected));
}
int gstd_migration_schema_inspection(const json_t *sets, int *ledger_exists) {
    if (ledger_exists)
        *ledger_exists = 0;
    json_t *keyspace = json_array_get(sets, 0), *ledger = json_array_get(sets, 1);
    if (!ledger_exists || !json_is_array(sets) || json_array_size(sets) != 2 ||
        !json_is_array(keyspace) || json_array_size(keyspace) != 1 ||
        !exact_field(json_array_get(keyspace, 0), "keyspace_name", "gitstore") ||
        !json_is_array(ledger) || json_array_size(ledger) > 1 ||
        (json_array_size(ledger) &&
         !exact_field(json_array_get(ledger, 0), "table_name", "gocqlx_migrate")))
        return -1;
    *ledger_exists = json_array_size(ledger) != 0;
    return 0;
}
int gstd_migration_output_create(void) {
#ifdef __linux__
    if (!geteuid())
        return -1;
    int fd = (int)syscall(SYS_memfd_create, "gitstored-migration-output",
                          MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 3 || fchmod(fd, 0600) || ftruncate(fd, 65536) ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
#else
    return -1;
#endif
}
json_t *gstd_migration_output_rows(int fd, unsigned sets) {
#ifdef __linux__
    struct stat before, after;
    if (fd < 3 || fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
        before.st_nlink || (before.st_mode & 07777) != 0600 || before.st_size != 65536)
        return NULL;
    off_t length = lseek(fd, 0, SEEK_CUR);
    if (length <= 0 || length > 65536)
        return NULL;
    int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || (seals & (F_SEAL_GROW | F_SEAL_SHRINK)) != (F_SEAL_GROW | F_SEAL_SHRINK) ||
        ((seals & required) != required && fcntl(fd, F_ADD_SEALS, required)))
        return NULL;
    unsigned char bytes[65536];
    ssize_t count = pread(fd, bytes, (size_t)length, 0);
    if (count != length || lseek(fd, 0, SEEK_CUR) != length || fstat(fd, &after) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size)
        return NULL;
    return gstd_migration_cql_decode(bytes, (size_t)count, sets);
#else
    (void)fd;
    (void)sets;
    return NULL;
#endif
}
json_t *gstd_migration_cql_decode(const void *data, size_t size, unsigned sets) {
    if (!data || !size || size > 65536 || !sets || sets > 2 || memchr(data, 0, size))
        return NULL;
    const unsigned char *bytes = data;
    json_t *groups = json_array(), *current = NULL;
    unsigned state = 0, finished = 0;
    if (!groups)
        return NULL;
    for (size_t offset = 0; offset < size;) {
        size_t end = offset;
        while (end < size && bytes[end] != '\n')
            end++;
        const unsigned char *line = bytes + offset;
        size_t length = end - offset;
        offset = end < size ? end + 1 : size;
        if (length > 8192)
            goto failed;
        while (length && (*line == ' ' || *line == '\t' || *line == '\r')) {
            line++;
            length--;
        }
        while (length &&
               (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r'))
            length--;
        if (!length)
            continue;
        if (!state) {
            if (finished == sets || length != 6 || memcmp(line, "[json]", 6))
                goto failed;
            current = json_array();
            if (!current)
                goto failed;
            state = 1;
        } else if (state == 1) {
            if (length < 3)
                goto failed;
            for (size_t i = 0; i < length; i++)
                if (line[i] != '-')
                    goto failed;
            state = 2;
        } else if (*line == '{') {
            if (json_array_size(current) == 13)
                goto failed;
            json_t *row = gstd_json_parse(line, length);
            int valid = json_is_object(row) && !json_array_append(current, row);
            json_decref(row);
            if (!valid)
                goto failed;
        } else {
            if (*line != '(')
                goto failed;
            size_t i = 1;
            unsigned count = 0;
            while (i < length && line[i] >= '0' && line[i] <= '9') {
                if (i > 2)
                    goto failed;
                count = count * 10 + line[i++] - '0';
            }
            if (i == 1 || length - i != 6 || memcmp(line + i, " rows)", 6) ||
                count != json_array_size(current) || json_array_append(groups, current))
                goto failed;
            json_decref(current);
            current = NULL;
            finished++;
            state = 0;
        }
    }
    if (!state && finished == sets)
        return groups;
failed:
    json_decref(current);
    json_decref(groups);
    return NULL;
}
static int same(const json_t *a, const json_t *b) {
    return json_is_string(a) && json_is_string(b) && json_equal(a, b);
}
static int number(const char *text, unsigned start, unsigned length) {
    int value = 0;
    for (unsigned i = start; i < start + length; i++) {
        if (text[i] < '0' || text[i] > '9')
            return -1;
        value = value * 10 + text[i] - '0';
    }
    return value;
}
static int timestamp(const json_t *value) {
    const char *s = json_string_value(value);
    /* Pinned cqlsh JSON timestamp format, checked against native output. */
    if (!s || json_string_length(value) != 24 || strlen(s) != 24 || s[4] != '-' || s[7] != '-' ||
        (s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':' || s[19] != '.' ||
        s[23] != 'Z')
        return 0;
    int year = number(s, 0, 4), month = number(s, 5, 2), day = number(s, 8, 2),
        hour = number(s, 11, 2), minute = number(s, 14, 2), second = number(s, 17, 2),
        ms = number(s, 20, 3);
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59 || ms < 0)
        return 0;
    unsigned maximum =
        days[month - 1] + (month == 2 && !(year % 4) && ((year % 100) || !(year % 400)));
    return (unsigned)day <= maximum &&
           !(year == 1 && month == 1 && day == 1 && !hour && !minute && !second && !ms);
}
int gstd_migration_history_validate(const char *version, const json_t *rows, int complete) {
    if (!json_is_array(rows) || (complete != 0 && complete != 1))
        return -1;
    json_t *target = gstd_migration_target(version);
    const json_t *files = json_object_get(json_object_get(target, "schema"), "files");
    size_t count = json_array_size(files), actual = json_array_size(rows);
    int valid = target && count > 0 && count <= 32 && actual <= count;
    uint32_t seen = 0, unfinished = 0;
    unsigned last = 0;
    for (size_t i = 0; valid && i < actual; i++) {
        const json_t *row = json_array_get(rows, i), *expected = NULL;
        unsigned index = 0;
        for (; index < count; index++) {
            expected = json_array_get(files, index);
            if (same(json_object_get(row, "name"), json_object_get(expected, "name")))
                break;
        }
        if (!json_is_object(row) || json_object_size(row) != 4 || index == count ||
            (seen & (UINT32_C(1) << index))) {
            valid = 0;
            break;
        }
        json_t *done = json_object_get(row, "done"), *end = json_object_get(row, "end_time");
        json_int_t statements = json_integer_value(json_object_get(expected, "statementCount"));
        valid = same(json_object_get(row, "checksum"), json_object_get(expected, "ledgerMd5")) &&
                json_is_integer(done) && statements > 0 && statements <= 4096 &&
                json_integer_value(done) >= 0 && json_integer_value(done) <= statements &&
                (json_is_null(end) || (timestamp(end) && json_integer_value(done) == statements));
        if (!valid)
            break;
        seen |= UINT32_C(1) << index;
        if (json_is_null(end))
            unfinished |= UINT32_C(1) << index;
        if (index > last)
            last = index;
    }
    uint32_t prefix = !actual ? 0 : last == 31 ? UINT32_MAX : (UINT32_C(1) << (last + 1)) - 1;
    uint32_t full = count >= 32 ? UINT32_MAX : count ? (UINT32_C(1) << count) - 1 : 0;
    valid = valid && seen == prefix && (!unfinished || unfinished == (UINT32_C(1) << last)) &&
            (!complete || (seen == full && !unfinished));
    json_decref(target);
    return valid ? 0 : -1;
}
