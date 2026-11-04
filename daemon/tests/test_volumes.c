#define _GNU_SOURCE 1
#include "support.h"
#include "volumes.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
    int result = 0, directory = -1, data = -1, outer = -1;
    int release[2] = {-1, -1};
    pid_t lease_child = -1;
    char *root = gstd_test_dir();
    gstd_volume_root owner = {.lock_fd = -1};
    gstd_volume_handle first = {.data_fd = -1, .writer_fd = -1},
                       second = {.data_fd = -1, .writer_fd = -1};
    CHECK(root);
    directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory >= 3);
#ifdef __linux__
    gstd_volume_identity receipt = {0};
    const char *store = "0123456789abcdef0123456789abcdef";
    CHECK(gstd_volume_root_reopen(directory, store, &owner) == GSTD_VOLUME_INTEGRITY);
    CHECK(faccessat(directory, ".gitstored-volume-root", F_OK, AT_SYMLINK_NOFOLLOW) < 0 &&
          faccessat(directory, ".gitstored-volume-lock", F_OK, AT_SYMLINK_NOFOLLOW) < 0);
    CHECK(gstd_volume_root_lock(directory, store, &owner) == GSTD_VOLUME_OK);
    gstd_volume_root rival = {.lock_fd = -1};
    CHECK(gstd_volume_root_lock(directory, store, &rival) == GSTD_VOLUME_HELD);
    CHECK(gstd_volume_root_reopen(directory, store, &rival) == GSTD_VOLUME_HELD);
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, NULL,
                              &first) == GSTD_VOLUME_MISSING);
    CHECK(gstd_volume_acquire(directory, &owner, "../escape", "initial-operation", 1, NULL,
                              &first) == GSTD_VOLUME_INVALID);
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 1, NULL,
                              &first) == GSTD_VOLUME_OK);
    receipt = first.identity;
    data = openat(first.data_fd, "retained", O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0600);
    CHECK(data >= 3 && write(data, "retained-bytes", 14) == 14 && !fsync(data));
    close(data);
    data = -1;
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_HELD);
    /* The workload inherits this same open-file-description lease. Losing the
     * manager's copy must not admit another writer while the workload lives. */
    CHECK(!pipe(release));
    lease_child = fork();
    CHECK(lease_child >= 0);
    if (!lease_child) {
        close(release[1]);
        char command;
        int received = read(release[0], &command, 1);
        gstd_volume_handle_close(&first);
        _exit(received == 1 && command == 'x' ? 0 : 1);
    }
    close(release[0]);
    release[0] = -1;
    gstd_volume_handle_close(&first);
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_HELD);
    CHECK(gstd_volume_observe_open(directory, &owner, "repositories", "initial-operation", NULL,
                                   &second) == GSTD_VOLUME_INVALID);
    CHECK(gstd_volume_observe_open(directory, &owner, "repositories", "initial-operation", &receipt,
                                   &second) == GSTD_VOLUME_OK);
    CHECK(second.data_fd >= 3 && second.writer_fd == -1);
    data = openat(second.data_fd, "retained", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char observed[14];
    CHECK(data >= 3 && read(data, observed, sizeof(observed)) == sizeof(observed) &&
          !memcmp(observed, "retained-bytes", 14));
    close(data);
    data = -1;
    gstd_volume_handle_close(&second);
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_HELD);
    CHECK(write(release[1], "x", 1) == 1);
    close(release[1]);
    release[1] = -1;
    int child_status = 0;
    CHECK(waitpid(lease_child, &child_status, 0) == lease_child);
    lease_child = -1;
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "different-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_INTEGRITY);
    gstd_volume_identity changed = receipt;
    changed.data_inode++;
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &changed,
                              &second) == GSTD_VOLUME_INTEGRITY);
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_OK);
    data = openat(second.data_fd, "retained", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char bytes[32] = {0};
    CHECK(data >= 3 && read(data, bytes, sizeof(bytes)) == 14 && !strcmp(bytes, "retained-bytes"));
    close(data);
    data = -1;
    gstd_volume_handle_close(&second);
    CHECK(!linkat(directory, "writer-repositories.lock", directory, "writer-linked.lock", 0));
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_INTEGRITY);
    CHECK(!unlinkat(directory, "writer-linked.lock", 0));
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &first) == GSTD_VOLUME_OK);
    CHECK(!renameat(directory, "writer-repositories.lock", directory, "old-writer.lock"));
    data = openat(directory, "writer-repositories.lock", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
                  0600);
    CHECK(data >= 3);
    close(data);
    data = -1;
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_INTEGRITY);
    CHECK(!unlinkat(directory, "writer-repositories.lock", 0));
    CHECK(!renameat(directory, "old-writer.lock", directory, "writer-repositories.lock"));
    gstd_volume_handle_close(&first);
    CHECK(!symlinkat("volume-repositories", directory, "volume-alias"));
    CHECK(gstd_volume_acquire(directory, &owner, "alias", "initial-operation", 0, NULL, &second) ==
          GSTD_VOLUME_INTEGRITY);
    outer =
        openat(directory, "volume-repositories", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(outer >= 3 && !renameat(outer, "data", outer, "saved-data") &&
          !mkdirat(outer, "data", 0700));
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_INTEGRITY);
    CHECK(!unlinkat(outer, "data", AT_REMOVEDIR) && !renameat(outer, "saved-data", outer, "data"));
    close(outer);
    outer = -1;
    gstd_volume_root_close(&owner);
    CHECK(gstd_volume_root_lock(directory, "fedcba9876543210fedcba9876543210", &owner) ==
          GSTD_VOLUME_INTEGRITY);
    CHECK(gstd_volume_root_lock(directory, store, &owner) == GSTD_VOLUME_OK);
    CHECK(gstd_volume_acquire(directory, &owner, "repositories", "initial-operation", 0, &receipt,
                              &second) == GSTD_VOLUME_OK);
    gstd_volume_handle_close(&second);
    CHECK(!mkdirat(directory, "volume-incomplete", 0700));
    CHECK(gstd_volume_acquire(directory, &owner, "incomplete", "initial-operation", 1, NULL,
                              &second) == GSTD_VOLUME_INTEGRITY);
#else
    CHECK(gstd_volume_root_lock(directory, "0123456789abcdef0123456789abcdef", &owner) ==
          GSTD_VOLUME_UNAVAILABLE);
#endif
done:
    if (release[0] >= 0)
        close(release[0]);
    if (release[1] >= 0)
        close(release[1]);
    if (lease_child > 0)
        waitpid(lease_child, NULL, 0);
    if (outer >= 0)
        close(outer);
    if (data >= 0)
        close(data);
    gstd_volume_handle_close(&first);
    gstd_volume_handle_close(&second);
    gstd_volume_root_close(&owner);
    if (directory >= 0)
        close(directory);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
