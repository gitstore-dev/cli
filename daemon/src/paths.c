#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/paths.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* Resolve every component through directory descriptors, never symlinks. */
int gstd_owned_directory(const char *path) {
    if (!path || path[0] != '/' || strlen(path) > 4096)
        return -1;
    char *copy = strdup(path);
    if (!copy)
        return -1;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *save = NULL, *part = strtok_r(copy, "/", &save);
    if (!part) {
        free(copy);
        close(fd);
        return -1;
    }
    while (part && fd >= 0) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) {
            close(fd);
            fd = -1;
            break;
        }
        char *next = strtok_r(NULL, "/", &save);
        if (!next && mkdirat(fd, part, 0700) != 0 && errno != EEXIST) {
            close(fd);
            fd = -1;
            break;
        }
        int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = child;
        part = next;
    }
    free(copy);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700)) {
        close(fd);
        fd = -1;
    }
    return fd;
}
