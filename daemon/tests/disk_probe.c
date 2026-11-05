#define _GNU_SOURCE 1
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "gitstored/provisioning.h"
#include "image_backend.h"
#include "volumes.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "disk probe line %d\n", __LINE__);                                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int mapping(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    size_t size = strlen(text);
    int result = write(fd, text, size) == (ssize_t)size ? 0 : -1;
    close(fd);
    return result;
}
int main(int argc, char **argv) {
    /* Explicit developer-only fixture root, no arbitrary workload command. */
    CHECK(argc == 2 && argv[1][0] == '/' && geteuid() && gstd_image_backend_available());
    uid_t uid = geteuid();
    gid_t gid = getegid();
    struct stat root;
    CHECK(!lstat(argv[1], &root) && S_ISDIR(root.st_mode) && root.st_uid == uid &&
          (root.st_mode & 07777) == 0700);
    CHECK(!chdir(argv[1]));
    CHECK(!mkdir("source", 0700) && !mkdir("limited", 0700));
    int source = open("source", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(source >= 3);
    CHECK(!prctl(PR_SET_DUMPABLE, 1) && !unshare(CLONE_NEWUSER));
    char text[128];
    snprintf(text, sizeof(text), "%lu %lu 1\n", (unsigned long)uid, (unsigned long)uid);
    CHECK(!mapping("/proc/self/uid_map", text));
    CHECK(!mapping("/proc/self/setgroups", "deny\n"));
    snprintf(text, sizeof(text), "%lu %lu 1\n", (unsigned long)gid, (unsigned long)gid);
    CHECK(!mapping("/proc/self/gid_map", text));
    CHECK(!unshare(CLONE_NEWNS));
    CHECK(!mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL));
    snprintf(text, sizeof(text), "size=1m,mode=0700,uid=%lu,gid=%lu", (unsigned long)uid,
             (unsigned long)gid);
    CHECK(!mount("tmpfs", "limited", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, text));
    CHECK(!mkdir("limited/staging", 0700) && !mkdir("limited/volumes", 0700));
    int stage = open("limited/staging", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(stage >= 3);
    gstd_json_initialize();
    json_t *components = json_pack("[s]", "api");
#if defined(__aarch64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    json_t *manifest = gstd_asset_manifest(components, architecture);
    CHECK(manifest);
    gstd_provision_result result = {0};
    CHECK(gstd_provision_import(manifest, "api", source, stage, 134217728, 536870912, 1000, -1,
                                &result) == GSTD_PROVISION_QUOTA);
    CHECK(!result.downloaded_bytes && !result.merged_bytes && !result.staging_inode);
    struct stat unused;
    CHECK(fstatat(stage, "manifest.json", &unused, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);
    json_decref(manifest);
    json_decref(components);
    close(stage);
    close(source);
    int directory = open("limited/volumes", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory >= 3);
    gstd_volume_root owner = {.lock_fd = -1};
    gstd_volume_handle retained = {.data_fd = -1, .writer_fd = -1};
    gstd_volume_handle failed = {.data_fd = -1, .writer_fd = -1};
    gstd_volume_handle datastore = {.data_fd = -1, .writer_fd = -1};
    CHECK(gstd_volume_root_lock(directory, "0123456789abcdef0123456789abcdef", &owner) ==
          GSTD_VOLUME_OK);
    CHECK(gstd_volume_acquire(directory, &owner, "retained", "disk-operation", 1, NULL,
                              &retained) == GSTD_VOLUME_OK);
    int marker = openat(retained.data_fd, "retained", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(marker >= 3 && write(marker, "preserve", 8) == 8 && !fsync(marker));
    gstd_volume_identity identity = retained.identity;
    gstd_volume_handle_close(&retained);
    CHECK(gstd_scylla_volume_acquire(directory, &owner, "scylla-retained", "disk-operation", 1,
                                     NULL, &datastore) == GSTD_VOLUME_OK);
    int datastore_marker =
        openat(datastore.data_fd, "retained", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(datastore_marker >= 3 && write(datastore_marker, "database", 8) == 8 &&
          !fsync(datastore_marker));
    gstd_volume_identity datastore_identity = datastore.identity;
    gstd_volume_handle_close(&datastore);
    int fill = open("limited/filler", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(fill >= 3);
    char bytes[4096] = {0};
    size_t written = 0;
    ssize_t count;
    while ((count = write(fill, bytes, sizeof(bytes))) > 0) {
        written += (size_t)count;
        CHECK(written <= 2 * 1024 * 1024);
    }
    CHECK(count < 0 && errno == ENOSPC);
    close(fill);
    CHECK(gstd_volume_acquire(directory, &owner, "new", "new-operation", 1, NULL, &failed) !=
          GSTD_VOLUME_OK);
    CHECK(failed.data_fd == -1 && failed.writer_fd == -1);
    CHECK(gstd_scylla_volume_acquire(directory, &owner, "scylla-new", "new-operation", 1, NULL,
                                     &failed) != GSTD_VOLUME_OK);
    CHECK(failed.data_fd == -1 && failed.writer_fd == -1);
    CHECK(gstd_volume_acquire(directory, &owner, "retained", "disk-operation", 0, &identity,
                              &retained) == GSTD_VOLUME_OK);
    memset(bytes, 0, sizeof(bytes));
    CHECK(pread(marker, bytes, sizeof(bytes), 0) == 8 && !memcmp(bytes, "preserve", 8));
    CHECK(retained.identity.data_inode == identity.data_inode);
    CHECK(gstd_scylla_volume_acquire(directory, &owner, "scylla-retained", "disk-operation", 0,
                                     &datastore_identity, &datastore) == GSTD_VOLUME_OK);
    CHECK(datastore.identity.data_inode == datastore_identity.data_inode &&
          datastore.identity.writer_inode == datastore_identity.writer_inode);
    int reopened = openat(datastore.data_fd, "retained", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(reopened >= 3 && pread(reopened, bytes, sizeof(bytes), 0) == 8 &&
          !memcmp(bytes, "database", 8));
    close(reopened);
    close(datastore_marker);
    gstd_volume_handle_close(&datastore);
    close(marker);
    gstd_volume_handle_close(&retained);
    gstd_volume_root_close(&owner);
    close(directory);
    puts("{\"assetCapacityPreflight\":\"quota-before-transfer\",\"volumeFull\":\"allocation-"
         "rejected\",\"retainedBytes\":\"preserved\",\"datastoreFull\":\"allocation-rejected\","
         "\"datastoreRetainedBytes\":\"preserved\"}");
    /* Mount belongs only to this short-lived user/mount namespace. */
    return 0;
}
