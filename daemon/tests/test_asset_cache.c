#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/asset_cache.h"
#include "gitstored/asset_tree.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int main(void) {
    int result = 0, rootfd = -1, stage = -1, file = -1;
    char *root = gstd_test_dir(), name[138], key[65];
    gstd_cache_owner owner = {.lock_fd = -1}, contender = {.lock_fd = -1};
    memset(key, 'a', 64);
    key[64] = 0;
    CHECK(!gstd_asset_stage_name(key, "op", name));
    CHECK(gstd_asset_stage_name(key, "../bad", name) < 0);
    CHECK(!gstd_asset_stage_name(key, "op", name));
    CHECK(root);
    rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(rootfd >= 0);
    const char store[] = "0123456789abcdef0123456789abcdef";
    CHECK(gstd_asset_cache_lock(rootfd, store, 0, 0, 1, &owner) == GSTD_CACHE_OK);
    CHECK(gstd_asset_cache_lock(rootfd, store, owner.device, owner.inode, 0, &contender) ==
          GSTD_CACHE_CONFLICT);
    CHECK(gstd_asset_cache_lock(rootfd, store, owner.device, owner.inode + 1, 0, &contender) ==
          GSTD_CACHE_INTEGRITY);
    uint64_t device = owner.device, inode = owner.inode;
    gstd_asset_cache_unlock(&owner);
    CHECK(gstd_asset_cache_lock(rootfd, "ffffffffffffffffffffffffffffffff", device, inode, 0,
                                &contender) == GSTD_CACHE_INTEGRITY);
    CHECK(!linkat(rootfd, ".gitstored-lock", rootfd, "lock-alias", 0));
    CHECK(gstd_asset_cache_lock(rootfd, store, device, inode, 0, &contender) ==
          GSTD_CACHE_INTEGRITY);
    CHECK(!unlinkat(rootfd, "lock-alias", 0));
    CHECK(gstd_asset_cache_lock(rootfd, store, device, inode, 0, &owner) == GSTD_CACHE_OK);
    CHECK(!mkdirat(rootfd, "foreign", 0700));
    int foreign = openat(rootfd, "foreign", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(foreign >= 0);
    int unknown = openat(foreign, "unknown", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(unknown >= 0);
    close(unknown);
    CHECK(gstd_asset_cache_lock(foreign, store, 0, 0, 1, &contender) == GSTD_CACHE_INTEGRITY);
    CHECK(!faccessat(foreign, "unknown", F_OK, 0));
    close(foreign);
#ifdef __linux__
    CHECK(!mkdirat(rootfd, name, 0700));
    stage = openat(rootfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(stage >= 0);
    file = openat(stage, "payload", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(file >= 0);
    CHECK(write(file, "verified", 8) == 8);
    struct stat st;
    CHECK(!fstat(stage, &st));
    gstd_provision_result proof = {.staging_device = (uint64_t)st.st_dev,
                                   .staging_inode = (uint64_t)st.st_ino};
    CHECK(!gstd_asset_tree_digest(stage, 100, 1, proof.tree_digest, &proof.logical_bytes));
    CHECK(pwrite(file, "!", 1, 0) == 1);
    CHECK(gstd_asset_promote(rootfd, name, key, &proof, -1) == GSTD_CACHE_INTEGRITY);
    CHECK(faccessat(rootfd, name, F_OK, 0) == 0 && faccessat(rootfd, key, F_OK, 0) < 0);
    CHECK(pwrite(file, "v", 1, 0) == 1);
    CHECK(!mkdirat(rootfd, key, 0700));
    CHECK(gstd_asset_promote(rootfd, name, key, &proof, -1) == GSTD_CACHE_CONFLICT);
    CHECK(!unlinkat(rootfd, key, AT_REMOVEDIR));
    int signal[2];
    CHECK(!pipe(signal));
    CHECK(write(signal[1], "x", 1) == 1);
    CHECK(gstd_asset_promote(rootfd, name, key, &proof, signal[0]) == GSTD_CACHE_CANCELLED);
    close(signal[0]);
    close(signal[1]);
    CHECK(gstd_asset_promote(rootfd, name, key, &proof, -1) == GSTD_CACHE_OK);
    CHECK(faccessat(rootfd, name, F_OK, 0) < 0 && !fstatat(rootfd, key, &st, AT_SYMLINK_NOFOLLOW));
    CHECK((uint64_t)st.st_ino == proof.staging_inode);
    /* Cleanup removes only the recorded staging role and never follows links
     * into retained data or an already-active root. */
    CHECK(!mkdirat(rootfd, name, 0700));
    close(stage);
    stage = openat(rootfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(stage >= 0);
    CHECK(!fstat(stage, &st));
    uint64_t cleanup_device = (uint64_t)st.st_dev, cleanup_inode = (uint64_t)st.st_ino;
    CHECK(!mkdirat(stage, "readonly", 0700));
    int readonly = openat(stage, "readonly", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(readonly >= 0);
    int partial = openat(readonly, "partial", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(partial >= 0);
    close(partial);
    CHECK(!fchmod(readonly, 0555));
    close(readonly);
    CHECK(!symlinkat("../retained", stage, "outside-link"));
    CHECK(!mkdirat(rootfd, "retained", 0700));
    CHECK(gstd_asset_discard_staging(rootfd, name, key, cleanup_device, cleanup_inode + 1, -1) ==
          GSTD_CACHE_INTEGRITY);
    CHECK(gstd_asset_discard_staging(rootfd, key, key, cleanup_device, cleanup_inode, -1) ==
          GSTD_CACHE_INVALID);
    CHECK(gstd_asset_discard_staging(rootfd, name, key, cleanup_device, cleanup_inode, -1) ==
          GSTD_CACHE_OK);
    CHECK(faccessat(rootfd, name, F_OK, 0) < 0 && faccessat(rootfd, "retained", F_OK, 0) == 0 &&
          faccessat(rootfd, key, F_OK, 0) == 0);
    gstd_provision_result wrong = proof;
    wrong.staging_inode++;
    CHECK(gstd_asset_evict(rootfd, key, &wrong, -1) == GSTD_CACHE_INTEGRITY);
    CHECK(pwrite(file, "!", 1, 0) == 1);
    CHECK(gstd_asset_evict(rootfd, key, &proof, -1) == GSTD_CACHE_INTEGRITY);
    CHECK(pwrite(file, "v", 1, 0) == 1);
    CHECK(gstd_asset_evict(rootfd, key, &proof, -1) == GSTD_CACHE_OK);
    CHECK(faccessat(rootfd, key, F_OK, 0) < 0 && faccessat(rootfd, "retained", F_OK, 0) == 0);
    CHECK(gstd_asset_evict(rootfd, key, &proof, -1) == GSTD_CACHE_OK);
    /* Crash after eviction rename: resume only the persisted inode's private
     * eviction role, even when a previous cleanup already removed some bytes. */
    CHECK(!mkdirat(rootfd, key, 0700));
    int active = openat(rootfd, key, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(active >= 0 && !fstat(active, &st));
    proof.staging_device = (uint64_t)st.st_dev;
    proof.staging_inode = (uint64_t)st.st_ino;
    CHECK(!gstd_asset_tree_digest(active, 100, 1, proof.tree_digest, &proof.logical_bytes));
    close(active);
    char evict[71];
    CHECK(snprintf(evict, sizeof(evict), "evict-%s", key) == 70);
    CHECK(!renameat(rootfd, key, rootfd, evict));
    CHECK(!mkdirat(rootfd, key, 0700));
    CHECK(gstd_asset_evict(rootfd, key, &proof, -1) == GSTD_CACHE_CONFLICT);
    CHECK(!unlinkat(rootfd, key, AT_REMOVEDIR));
    CHECK(gstd_asset_evict(rootfd, key, &proof, -1) == GSTD_CACHE_OK);
    CHECK(faccessat(rootfd, evict, F_OK, 0) < 0 && faccessat(rootfd, "retained", F_OK, 0) == 0);
#endif
done:
    gstd_asset_cache_unlock(&owner);
    gstd_asset_cache_unlock(&contender);
    if (file >= 0)
        close(file);
    if (stage >= 0)
        close(stage);
    if (rootfd >= 0)
        close(rootfd);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
