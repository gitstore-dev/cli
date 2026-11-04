#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "support.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#define GSTD_TEST_PREFIX "/private/tmp/gitstored-test-"
#else
#define GSTD_TEST_PREFIX "/tmp/gitstored-test-"
#endif
char *gstd_test_dir(void) {
    /* Deliberately independent of HOME and the user's runtime/config directories. */
    char *name = strdup(GSTD_TEST_PREFIX "XXXXXX");
    if (name == NULL)
        return NULL;
    if (mkdtemp(name) == NULL) {
        free(name);
        return NULL;
    }
    return name;
}
static int clear_dir(int fd) {
    DIR *dir = fdopendir(dup(fd));
    if (dir == NULL)
        return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            result = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (child < 0) {
                result = -1;
                break;
            }
            int status = clear_dir(child);
            close(child);
            if (status || unlinkat(fd, entry->d_name, AT_REMOVEDIR)) {
                result = -1;
                break;
            }
        } else if (unlinkat(fd, entry->d_name, 0)) {
            result = -1;
            break;
        }
    }
    closedir(dir);
    return result;
}
int gstd_test_remove(char *root) {
    if (root == NULL || strncmp(root, GSTD_TEST_PREFIX, strlen(GSTD_TEST_PREFIX)) != 0)
        return -1;
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    int result = clear_dir(fd);
    close(fd);
    if (!result)
        result = rmdir(root);
    free(root);
    return result;
}
