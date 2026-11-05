#define _GNU_SOURCE 1
#include "gitstored/asset_tree.h"
#include "owned_read.h"
#include "support.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "owned read line %d\n", __LINE__);                                     \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
typedef struct {
    char bytes[64];
    size_t length;
} contents;
static int consume(void *context, const unsigned char *bytes, size_t length) {
    contents *value = context;
    if (length > sizeof(value->bytes) - value->length)
        return -1;
    memcpy(value->bytes + value->length, bytes, length);
    value->length += length;
    return 0;
}
static int copied_execution(int sealed) {
    int source = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int copy = (int)syscall(SYS_memfd_create, "owned-read-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (source < 0 || copy < 0)
        return -1;
    char buffer[32768];
    ssize_t length;
    while ((length = read(source, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < length) {
            ssize_t written = write(copy, buffer + offset, (size_t)(length - offset));
            if (written <= 0)
                return -1;
            offset += written;
        }
    }
    close(source);
    const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if (length < 0 || fchmod(copy, 0777) || (sealed && fcntl(copy, F_ADD_SEALS, seals)))
        return -1;
    pid_t child = fork();
    if (!child) {
        char *arguments[] = {"owned-read-test", sealed ? "sealed" : "unsealed", NULL};
        char *environment[] = {"PATH=/nonexistent", "LANG=C", NULL};
        syscall(SYS_execveat, copy, "", arguments, environment, AT_EMPTY_PATH);
        _exit(126);
    }
    close(copy);
    int status = 0;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                   !WEXITSTATUS(status)
               ? 0
               : -1;
}
int main(int argc, char **argv) {
    /* Keep this executable independent of libcrun's automatic self-cloning:
     * both sealed and unsealed current executables must reach the reader. */
    if (!geteuid())
        return 77;
    pid_t probe = fork();
    if (!probe)
        _exit(unshare(CLONE_NEWUSER) ? 77 : 0);
    int status;
    if (probe < 0 || waitpid(probe, &status, 0) != probe || !WIFEXITED(status))
        return 1;
    if (WEXITSTATUS(status) == 77) {
        fputs("SKIP: rootless user namespaces unavailable\n", stderr);
        return 77;
    }
    if ((argc != 1 && (argc != 2 || (strcmp(argv[1], "sealed") && strcmp(argv[1], "unsealed")))) ||
        (argc == 1 && (copied_execution(1) || copied_execution(0))))
        return 1;
    int result = 0, rootfd = -1, treefd = -1, file = -1, pinned = -1, link = -1;
    char *root = gstd_test_dir();
    CHECK(root);
    rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(rootfd >= 3 && !mkdirat(rootfd, "tree", 0700));
    treefd = openat(rootfd, "tree", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    file = openat(treefd, "shadow", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    const char expected[] = "owned-shadow";
    CHECK(file >= 3 && write(file, expected, sizeof(expected) - 1) == sizeof(expected) - 1 &&
          !fchmod(file, 0000));
    struct stat before, after;
    CHECK(!fstat(file, &before));
    int denied = openat(treefd, "shadow", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(denied == -1 && errno == EACCES);
    pinned = openat(treefd, "shadow", O_PATH | O_CLOEXEC | O_NOFOLLOW);
    contents value = {0};
    CHECK(!prctl(PR_SET_DUMPABLE, 0));
    if (argc == 2 && !strcmp(argv[1], "unsealed")) {
        CHECK(pinned >= 3 &&
              gstd_owned_read(pinned, sizeof(expected) - 1, 1, consume, &value) < 0 &&
              !value.length && prctl(PR_GET_DUMPABLE) == 0);
        CHECK(!fstat(file, &after) && after.st_mode == before.st_mode &&
              after.st_ino == before.st_ino && after.st_ctim.tv_sec == before.st_ctim.tv_sec &&
              after.st_ctim.tv_nsec == before.st_ctim.tv_nsec);
        goto done;
    }
    CHECK(pinned >= 3 && !gstd_owned_read(pinned, sizeof(expected) - 1, 1, consume, &value));
    CHECK(prctl(PR_GET_DUMPABLE) == 0);
    CHECK(value.length == sizeof(expected) - 1 && !memcmp(value.bytes, expected, value.length));
    CHECK(!fstat(file, &after) && after.st_mode == before.st_mode &&
          after.st_uid == before.st_uid && after.st_gid == before.st_gid &&
          after.st_ino == before.st_ino && after.st_mtim.tv_sec == before.st_mtim.tv_sec &&
          after.st_mtim.tv_nsec == before.st_mtim.tv_nsec &&
          after.st_ctim.tv_sec == before.st_ctim.tv_sec &&
          after.st_ctim.tv_nsec == before.st_ctim.tv_nsec);
    char first[65], again[65];
    uint64_t bytes = 0;
    CHECK(!gstd_asset_tree_digest(treefd, 1024, 1, first, &bytes) && bytes == sizeof(expected) - 1);
    CHECK(!gstd_asset_tree_digest(treefd, 1024, 0, again, &bytes) && !strcmp(first, again));
    CHECK(!fstat(file, &after) && (after.st_mode & 07777) == 0);
    CHECK(!linkat(treefd, "shadow", rootfd, "outside-alias", 0));
    CHECK(gstd_asset_tree_digest(treefd, 1024, 0, again, &bytes) < 0);
    CHECK(!unlinkat(rootfd, "outside-alias", 0));
    CHECK(!symlinkat("shadow", treefd, "symlink"));
    link = openat(treefd, "symlink", O_PATH | O_CLOEXEC | O_NOFOLLOW);
    CHECK(link >= 3 && gstd_owned_read(link, sizeof(expected) - 1, 0, consume, &value) < 0);
    CHECK(gstd_owned_read(pinned, sizeof(expected), 0, consume, &value) < 0);
done:
    if (file >= 0) {
        (void)fchmod(file, 0600);
        close(file);
    }
    if (link >= 0)
        close(link);
    if (pinned >= 0)
        close(pinned);
    if (treefd >= 0)
        close(treefd);
    if (rootfd >= 0)
        close(rootfd);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
