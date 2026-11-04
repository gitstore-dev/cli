#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    char *root = gstd_test_dir();
    if (!root)
        return 1;
    struct stat st;
    if (lstat(root, &st) || (st.st_mode & 0777) != 0700 || st.st_uid != getuid())
        return 1;
    int dir = open(root, O_RDONLY | O_DIRECTORY);
    if (dir < 0)
        return 1;
    /* Cleanup must unlink an escape symlink without traversing its destination. */
    if (symlinkat("/", dir, "escape") || mkdirat(dir, "child", 0700))
        return 1;
    close(dir);
    return gstd_test_remove(root) ? 1 : 0;
}
