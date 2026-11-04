#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
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
    int result = 0, rootfd = -1, file = -1;
    char *root = gstd_test_dir(), path[1024], outside[1024];
    char empty[65], original[65], changed[65];
    uint64_t bytes = 0;
    CHECK(root);
    CHECK(snprintf(path, sizeof(path), "%s/tree", root) > 0 && !mkdir(path, 0700));
    rootfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(rootfd >= 0);
    CHECK(!gstd_asset_tree_digest(rootfd, 100, 0, empty, &bytes) && !bytes);
    file = openat(rootfd, "file", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    CHECK(file >= 0 && write(file, "hello", 5) == 5);
    CHECK(!gstd_asset_tree_digest(rootfd, 100, 1, original, &bytes) && bytes == 5 &&
          strcmp(original, empty));
    CHECK(gstd_asset_tree_digest(rootfd, 4, 0, changed, &bytes) < 0);
    CHECK(!gstd_asset_tree_digest(rootfd, 100, 0, changed, &bytes) && !strcmp(original, changed));
    CHECK(pwrite(file, "!", 1, 0) == 1);
    CHECK(!gstd_asset_tree_digest(rootfd, 100, 0, changed, &bytes) && strcmp(original, changed));
    CHECK(pwrite(file, "h", 1, 0) == 1 && !fchmod(file, 0644));
    CHECK(!gstd_asset_tree_digest(rootfd, 100, 0, changed, &bytes) && strcmp(original, changed));
    CHECK(!fchmod(file, 0600));
    CHECK(snprintf(outside, sizeof(outside), "%s/outside-alias", root) > 0);
    CHECK(!linkat(rootfd, "file", AT_FDCWD, outside, 0));
    CHECK(gstd_asset_tree_digest(rootfd, 100, 0, changed, &bytes) < 0);
    CHECK(!linkat(rootfd, "file", rootfd, "alias", 0) && !unlink(outside));
    CHECK(!gstd_asset_tree_digest(rootfd, 100, 0, changed, &bytes) && bytes == 10);
    CHECK(!unlinkat(rootfd, "alias", 0));
    CHECK(!symlinkat(outside, rootfd, "link"));
    CHECK(!gstd_asset_tree_digest(rootfd, 100, 0, changed, &bytes) && bytes == 5);
    CHECK(!unlinkat(rootfd, "link", 0));
    CHECK(!mkfifoat(rootfd, "fifo", 0600));
    CHECK(gstd_asset_tree_digest(rootfd, 100, 0, changed, &bytes) < 0);
    CHECK(!unlinkat(rootfd, "fifo", 0));
    CHECK(!gstd_asset_tree_digest(rootfd, 100, 0, changed, &bytes) && !strcmp(original, changed));
done:
    if (file >= 0)
        close(file);
    if (rootfd >= 0)
        close(rootfd);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
