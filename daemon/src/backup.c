#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "backup.h"
#include "gitstored/asset_tree.h"
#include "gitstored/operations.h"
#include "migration.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int ordinary(int directory, dev_t device, unsigned depth, unsigned *entries) {
    struct stat info;
    if (depth > 64 || fstat(directory, &info) || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || info.st_dev != device || (info.st_mode & 07000))
        return -1;
    int copy = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    DIR *stream = copy < 0 ? NULL : fdopendir(copy);
    if (!stream) {
        if (copy >= 0)
            close(copy);
        return -1;
    }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(stream);
        if (!item) {
            if (errno)
                result = -1;
            break;
        }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
            continue;
        struct stat before, after;
        if (++*entries > 131072 || fstatat(directory, item->d_name, &before, AT_SYMLINK_NOFOLLOW) ||
            before.st_uid != geteuid() || before.st_dev != device || (before.st_mode & 07000) ||
            (!S_ISDIR(before.st_mode) && (!S_ISREG(before.st_mode) || before.st_nlink != 1))) {
            result = -1;
            break;
        }
        if (S_ISDIR(before.st_mode)) {
            int child =
                openat(directory, item->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            result = child < 0 ? -1 : ordinary(child, device, depth + 1, entries);
            if (child >= 0)
                close(child);
            if (result)
                break;
        }
        if (fstatat(directory, item->d_name, &after, AT_SYMLINK_NOFOLLOW) ||
            before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
            before.st_mode != after.st_mode || before.st_nlink != after.st_nlink ||
            before.st_size != after.st_size) {
            result = -1;
            break;
        }
    }
    closedir(stream);
    return result;
}
static int inspect(int directory, uint64_t maximum, char digest[65], uint64_t *bytes) {
    struct stat root;
    unsigned count = 0;
    if (!maximum || maximum > UINT64_C(8) * 1024 * 1024 * 1024 || directory < 3 ||
        fstat(directory, &root) || !S_ISDIR(root.st_mode) || root.st_uid != geteuid() ||
        (root.st_mode & 07777) != 0700 || ordinary(directory, root.st_dev, 0, &count) ||
        gstd_asset_tree_digest(directory, maximum, 0, digest, bytes))
        return -1;
    count = 0;
    return ordinary(directory, root.st_dev, 0, &count);
}
json_t *gstd_backup_describe(int directory, const char *version, uint64_t maximum) {
    json_t *target = gstd_migration_target(version);
    char compatibility[65], tree[65];
    uint64_t bytes = 0;
    int valid = target && !gstd_fingerprint(target, compatibility) &&
                !inspect(directory, maximum, tree, &bytes);
    json_decref(target);
    return valid ? json_pack("{s:i,s:s,s:s,s:s,s:s,s:s,s:s,s:I}", "schemaVersion", 1, "kind",
                             "gitstored-scylla-backup-payload", "datastore", "scylladb",
                             "targetVersion", version, "targetDigest", compatibility,
                             "treeAlgorithm", "gitstored-asset-tree-v1", "treeDigest", tree,
                             "logicalBytes", (json_int_t)bytes)
                 : NULL;
}
int gstd_backup_restore_validate(int directory, const json_t *manifest, const char *version,
                                 uint64_t maximum) {
    if (!json_is_object(manifest) || json_object_size(manifest) != 8)
        return -1;
    json_t *actual = gstd_backup_describe(directory, version, maximum);
    int valid = actual && json_equal(actual, manifest);
    json_decref(actual);
    return valid ? 0 : -1;
}
