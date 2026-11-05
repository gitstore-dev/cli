#define _GNU_SOURCE 1
#include "backup.h"
#include "gitstored/protocol.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "backup line %d: %s\n", __LINE__, #x);                                 \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    int result = 1, directory = -1, file = -1;
    char *root = gstd_test_dir();
    json_t *manifest = NULL, *changed = NULL, *rejected = NULL;
    const char *version = "gitstore-06152d-schema-012";
    CHECK(root);
    directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(directory >= 3);
    file = openat(directory, "sstable", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    CHECK(file >= 3 && write(file, "data", 4) == 4 && !fsync(file));
    CHECK(!mkdirat(directory, "empty", 0700));
    manifest = gstd_backup_describe(directory, version, 1024);
    CHECK(manifest && json_object_size(manifest) == 8);
    CHECK(json_integer_value(json_object_get(manifest, "logicalBytes")) == 4);
    CHECK(!gstd_backup_restore_validate(directory, manifest, version, 1024));
    CHECK(gstd_backup_restore_validate(directory, manifest, "unknown", 1024) == -1);
    CHECK(gstd_backup_restore_validate(directory, manifest, version, 3) == -1);
    CHECK(!gstd_backup_describe(directory, version, 0));
    changed = json_deep_copy(manifest);
    CHECK(changed && !json_object_set_new(changed, "nativePid", json_integer(getpid())));
    CHECK(gstd_backup_restore_validate(directory, changed, version, 1024) == -1);
    json_decref(changed);
    changed = json_deep_copy(manifest);
    CHECK(changed && !json_object_set_new(changed, "targetDigest", json_string("forged")));
    CHECK(gstd_backup_restore_validate(directory, changed, version, 1024) == -1);
    CHECK(pwrite(file, "X", 1, 0) == 1 && !fsync(file));
    CHECK(gstd_backup_restore_validate(directory, manifest, version, 1024) == -1);
    CHECK(pwrite(file, "d", 1, 0) == 1 && !fsync(file));
    CHECK(!gstd_backup_restore_validate(directory, manifest, version, 1024));
    CHECK(!symlinkat("sstable", directory, "linked"));
    rejected = gstd_backup_describe(directory, version, 1024);
    CHECK(!rejected);
    CHECK(!unlinkat(directory, "linked", 0));
    CHECK(!linkat(directory, "sstable", directory, "alias", 0));
    rejected = gstd_backup_describe(directory, version, 1024);
    CHECK(!rejected);
    CHECK(!unlinkat(directory, "alias", 0));
    CHECK(!mkfifoat(directory, "pipe", 0600));
    rejected = gstd_backup_describe(directory, version, 1024);
    CHECK(!rejected);
    CHECK(!unlinkat(directory, "pipe", 0));
    CHECK(!fchmod(directory, 0755));
    CHECK(gstd_backup_restore_validate(directory, manifest, version, 1024) == -1);
    CHECK(!fchmod(directory, 0700));
    CHECK(!gstd_backup_restore_validate(directory, manifest, version, 1024));
    result = 0;
done:
    json_decref(manifest);
    json_decref(changed);
    json_decref(rejected);
    if (file >= 0)
        close(file);
    if (directory >= 0) {
        (void)fchmod(directory, 0700);
        close(directory);
    }
    if (root)
        gstd_test_remove(root);
    return result;
}
