#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/asset_cache.h"
#include "gitstored/asset_tree.h"
#include "gitstored/protocol.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif
static int digest(const char *text) {
    if (!text || strnlen(text, 65) != 64)
        return 0;
    for (size_t i = 0; i < 64; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return 1;
}
static int store_identity(const char *text) {
    if (!text || strnlen(text, 33) != 32)
        return 0;
    for (size_t i = 0; i < 32; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return 1;
}
static int cache_file(int root, const char *name, int flags, mode_t mode) {
#ifdef __linux__
    struct open_how how = {.flags = (uint64_t)(flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
                           .mode = mode,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    return (int)syscall(SYS_openat2, root, name, &how, sizeof(how));
#else
    return openat(root, name, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, mode);
#endif
}
static int private_file(int root, int fd, const char *name, off_t size) {
    struct stat current, named;
    return !fstat(fd, &current) && !fstatat(root, name, &named, AT_SYMLINK_NOFOLLOW) &&
           S_ISREG(current.st_mode) && current.st_uid == geteuid() &&
           (current.st_mode & 07777) == 0600 && current.st_nlink == 1 && current.st_size == size &&
           current.st_dev == named.st_dev && current.st_ino == named.st_ino;
}
gstd_cache_status gstd_asset_cache_lock(int root, const char *store, uint64_t device,
                                        uint64_t inode, int initialize, gstd_cache_owner *owner) {
    if (!owner || owner->lock_fd != -1 || !store_identity(store) ||
        (initialize != 0 && initialize != 1) || (initialize ? (device || inode) : !inode))
        return GSTD_CACHE_INVALID;
    struct stat directory;
    if (fstat(root, &directory) || !S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() ||
        (directory.st_mode & 07777) != 0700 ||
        (!initialize &&
         ((uint64_t)directory.st_dev != device || (uint64_t)directory.st_ino != inode)))
        return GSTD_CACHE_INTEGRITY;
    int lock = cache_file(root, ".gitstored-lock", O_RDWR, 0);
    if (lock < 0 && initialize && errno == ENOENT)
        lock = cache_file(root, ".gitstored-lock", O_RDWR | O_CREAT | O_EXCL, 0600);
    if (lock < 0)
        return GSTD_CACHE_IO;
    gstd_cache_status result = GSTD_CACHE_INTEGRITY;
    int marker = -1;
    if (!private_file(root, lock, ".gitstored-lock", 0))
        goto done;
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        result = errno == EWOULDBLOCK || errno == EAGAIN ? GSTD_CACHE_CONFLICT : GSTD_CACHE_IO;
        goto done;
    }
    if (!private_file(root, lock, ".gitstored-lock", 0))
        goto done;
    if (initialize) {
        int scan = cache_file(root, ".", O_RDONLY | O_DIRECTORY, 0);
        DIR *stream = scan < 0 ? NULL : fdopendir(scan);
        if (!stream) {
            if (scan >= 0)
                close(scan);
            goto done;
        }
        int empty = 1;
        for (;;) {
            errno = 0;
            struct dirent *item = readdir(stream);
            if (!item) {
                if (errno)
                    empty = 0;
                break;
            }
            if (strcmp(item->d_name, ".") && strcmp(item->d_name, "..") &&
                strcmp(item->d_name, ".gitstored-lock") &&
                strcmp(item->d_name, ".gitstored-owner")) {
                empty = 0;
                break;
            }
        }
        if (closedir(stream))
            empty = 0;
        if (!empty)
            goto done;
    }
    marker = cache_file(root, ".gitstored-owner", O_RDONLY, 0);
    if (marker < 0 && initialize && errno == ENOENT) {
        marker = cache_file(root, ".gitstored-owner", O_RDWR | O_CREAT | O_EXCL, 0600);
        if (marker >= 0) {
            char bytes[33];
            memcpy(bytes, store, 32);
            bytes[32] = '\n';
            if (write(marker, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes) || fsync(marker) ||
                fsync(root)) {
                result = GSTD_CACHE_UNCERTAIN;
                goto done;
            }
        }
    }
    char bytes[34];
    if (marker < 0 || !private_file(root, marker, ".gitstored-owner", 33) ||
        pread(marker, bytes, sizeof(bytes), 0) != 33 || memcmp(bytes, store, 32) ||
        bytes[32] != '\n' || !private_file(root, marker, ".gitstored-owner", 33))
        goto done;
    close(marker);
    owner->lock_fd = lock;
    owner->device = (uint64_t)directory.st_dev;
    owner->inode = (uint64_t)directory.st_ino;
    memcpy(owner->store_id, store, 33);
    return GSTD_CACHE_OK;
done:
    if (marker >= 0)
        close(marker);
    close(lock);
    return result;
}
void gstd_asset_cache_unlock(gstd_cache_owner *owner) {
    if (!owner)
        return;
    if (owner->lock_fd >= 0)
        close(owner->lock_fd);
    memset(owner, 0, sizeof(*owner));
    owner->lock_fd = -1;
}
static int staging_role(const char *name, const char *key) {
    size_t length = name ? strnlen(name, 138) : 0;
    return digest(key) && length >= 74 && length <= 137 && !strncmp(name, "staging-", 8) &&
           !strncmp(name + 8, key, 64) && name[72] == '-' &&
           gstd_identifier(name + 73, length - 73);
}
int gstd_asset_stage_name(const char *key, const char *operation, char output[138]) {
    if (!output || !digest(key) || !operation || !gstd_identifier(operation, strlen(operation)))
        return -1;
    int length = snprintf(output, 138, "staging-%s-%s", key, operation);
    return length > 0 && length < 138 ? 0 : -1;
}
#if defined(__linux__) && defined(SYS_openat2) && defined(SYS_renameat2)
static int cancelled(int fd) {
    if (fd < 0)
        return 0;
    struct pollfd signal = {fd, POLLIN, 0};
    int ready;
    do {
        ready = poll(&signal, 1, 0);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0 || (signal.revents & (POLLERR | POLLNVAL)))
        return -1;
    return signal.revents ? 1 : 0;
}
static int identity(const struct stat *st, const gstd_provision_result *proof) {
    return S_ISDIR(st->st_mode) && st->st_uid == geteuid() && (st->st_mode & 07777) == 0700 &&
           (uint64_t)st->st_dev == proof->staging_device &&
           (uint64_t)st->st_ino == proof->staging_inode;
}
#endif
gstd_cache_status gstd_asset_promote(int cache, const char *name, const char *key,
                                     const gstd_provision_result *proof, int cancellation_fd) {
    if (!proof || !staging_role(name, key) || !digest(proof->tree_digest) ||
        proof->logical_bytes > UINT64_C(8) * 1024 * 1024 * 1024)
        return GSTD_CACHE_INVALID;
#if defined(__linux__) && defined(SYS_openat2) && defined(SYS_renameat2)
    struct stat directory, st;
    if (fstat(cache, &directory) || !S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() ||
        (directory.st_mode & 07777) != 0700)
        return GSTD_CACHE_INVALID;
    int signal = cancelled(cancellation_fd);
    if (signal)
        return signal > 0 ? GSTD_CACHE_CANCELLED : GSTD_CACHE_INVALID;
    if (!fstatat(cache, key, &st, AT_SYMLINK_NOFOLLOW))
        return GSTD_CACHE_CONFLICT;
    if (errno != ENOENT)
        return GSTD_CACHE_IO;
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    int stage = (int)syscall(SYS_openat2, cache, name, &how, sizeof(how));
    if (stage < 0)
        return errno == ENOSYS ? GSTD_CACHE_UNAVAILABLE : GSTD_CACHE_IO;
    gstd_cache_status result = GSTD_CACHE_INTEGRITY;
    char observed[65];
    uint64_t bytes = 0;
    if (fstat(stage, &st) || !identity(&st, proof) ||
        gstd_asset_tree_digest(stage, proof->logical_bytes ? proof->logical_bytes : 1, 1, observed,
                               &bytes) ||
        strcmp(observed, proof->tree_digest) || bytes != proof->logical_bytes ||
        fstatat(cache, name, &st, AT_SYMLINK_NOFOLLOW) || !identity(&st, proof))
        goto done;
    signal = cancelled(cancellation_fd);
    if (signal) {
        result = signal > 0 ? GSTD_CACHE_CANCELLED : GSTD_CACHE_INVALID;
        goto done;
    }
    if (syscall(SYS_renameat2, cache, name, cache, key, RENAME_NOREPLACE)) {
        result = errno == EEXIST   ? GSTD_CACHE_CONFLICT
                 : errno == ENOSYS ? GSTD_CACHE_UNAVAILABLE
                                   : GSTD_CACHE_IO;
        goto done;
    }
    result = GSTD_CACHE_UNCERTAIN;
    if (fsync(cache) || fstatat(cache, key, &st, AT_SYMLINK_NOFOLLOW) || !identity(&st, proof))
        goto done;
    result = GSTD_CACHE_OK;
done:
    close(stage);
    return result;
#else
    (void)cache;
    (void)cancellation_fd;
    return GSTD_CACHE_UNAVAILABLE;
#endif
}
#if defined(__linux__) && defined(SYS_openat2) && defined(SYS_renameat2)
static int child_directory(int parent, const char *name, const struct stat *expected) {
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    int fd = (int)syscall(SYS_openat2, parent, name, &how, sizeof(how)), path_only = 0;
    if (fd < 0 && errno == EACCES) {
        how.flags = O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        fd = (int)syscall(SYS_openat2, parent, name, &how, sizeof(how));
        path_only = 1;
    }
    struct stat actual;
    if (fd < 0)
        return -1;
    if (fstat(fd, &actual) || !S_ISDIR(actual.st_mode) || actual.st_uid != geteuid() ||
        actual.st_dev != expected->st_dev || actual.st_ino != expected->st_ino) {
        close(fd);
        return -1;
    }
    if (path_only) {
#ifdef SYS_fchmodat2
        if (syscall(SYS_fchmodat2, fd, "", 0700, AT_EMPTY_PATH)) {
            close(fd);
            return -1;
        }
#else
        close(fd);
        errno = ENOSYS;
        return -1;
#endif
        close(fd);
        how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        fd = (int)syscall(SYS_openat2, parent, name, &how, sizeof(how));
        if (fd < 0)
            return -1;
        if (fstat(fd, &actual) || actual.st_dev != expected->st_dev ||
            actual.st_ino != expected->st_ino || actual.st_uid != geteuid()) {
            close(fd);
            return -1;
        }
    }
    if (fchmod(fd, 0700)) {
        close(fd);
        return -1;
    }
    return fd;
}
static gstd_cache_status clear_tree(int directory, dev_t device, unsigned depth, size_t *entries,
                                    int cancel) {
    if (depth > 64)
        return GSTD_CACHE_INTEGRITY;
    int signal = cancelled(cancel);
    if (signal)
        return signal > 0 ? GSTD_CACHE_CANCELLED : GSTD_CACHE_INVALID;
    struct stat current;
    if (fstat(directory, &current) || current.st_uid != geteuid() || current.st_dev != device)
        return GSTD_CACHE_INTEGRITY;
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    int scan = (int)syscall(SYS_openat2, directory, ".", &how, sizeof(how));
    if (scan < 0)
        return GSTD_CACHE_IO;
    DIR *stream = fdopendir(scan);
    if (!stream) {
        close(scan);
        return GSTD_CACHE_IO;
    }
    char **names = NULL;
    size_t count = 0, capacity = 0;
    gstd_cache_status result = GSTD_CACHE_IO;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(stream);
        if (!item) {
            if (errno)
                goto done;
            break;
        }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
            continue;
        if (*entries >= 131072 || strlen(item->d_name) > 255) {
            result = GSTD_CACHE_INTEGRITY;
            goto done;
        }
        (*entries)++;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            char **grown = realloc(names, next * sizeof(*names));
            if (!grown)
                goto done;
            names = grown;
            capacity = next;
        }
        names[count] = strdup(item->d_name);
        if (!names[count])
            goto done;
        count++;
    }
    int closed = closedir(stream);
    stream = NULL;
    if (closed)
        goto done;
    for (size_t i = 0; i < count; i++) {
        signal = cancelled(cancel);
        if (signal) {
            result = signal > 0 ? GSTD_CACHE_CANCELLED : GSTD_CACHE_INVALID;
            goto done;
        }
        struct stat st, after;
        if (fstatat(directory, names[i], &st, AT_SYMLINK_NOFOLLOW) || st.st_uid != geteuid() ||
            st.st_dev != device) {
            result = GSTD_CACHE_INTEGRITY;
            goto done;
        }
        if (S_ISDIR(st.st_mode)) {
            int child = child_directory(directory, names[i], &st);
            if (child < 0) {
                result = errno == ENOSYS ? GSTD_CACHE_UNAVAILABLE : GSTD_CACHE_IO;
                goto done;
            }
            result = clear_tree(child, device, depth + 1, entries, cancel);
            close(child);
            if (result != GSTD_CACHE_OK)
                goto done;
            if (fstatat(directory, names[i], &after, AT_SYMLINK_NOFOLLOW) ||
                st.st_dev != after.st_dev || st.st_ino != after.st_ino) {
                result = GSTD_CACHE_INTEGRITY;
                goto done;
            }
            if (unlinkat(directory, names[i], AT_REMOVEDIR)) {
                result = GSTD_CACHE_IO;
                goto done;
            }
        } else if (unlinkat(directory, names[i], 0)) {
            result = GSTD_CACHE_IO;
            goto done;
        }
    }
    result = fsync(directory) ? GSTD_CACHE_UNCERTAIN : GSTD_CACHE_OK;
done:
    if (stream)
        closedir(stream);
    for (size_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return result;
}
#endif
static gstd_cache_status discard_role(int cache, const char *name, uint64_t device, uint64_t inode,
                                      int cancel) {
#if defined(__linux__) && defined(SYS_openat2) && defined(SYS_renameat2)
    int signal = cancelled(cancel);
    if (signal)
        return signal > 0 ? GSTD_CACHE_CANCELLED : GSTD_CACHE_INVALID;
    struct stat owner, st;
    gstd_provision_result proof = {.staging_device = device, .staging_inode = inode};
    if (fstat(cache, &owner) || !S_ISDIR(owner.st_mode) || owner.st_uid != geteuid() ||
        (owner.st_mode & 07777) != 0700)
        return GSTD_CACHE_INVALID;
    if (fstatat(cache, name, &st, AT_SYMLINK_NOFOLLOW) || !identity(&st, &proof))
        return GSTD_CACHE_INTEGRITY;
    int stage = child_directory(cache, name, &st);
    if (stage < 0)
        return errno == ENOSYS ? GSTD_CACHE_UNAVAILABLE : GSTD_CACHE_IO;
    size_t entries = 0;
    gstd_cache_status result = clear_tree(stage, st.st_dev, 0, &entries, cancel);
    close(stage);
    if (result != GSTD_CACHE_OK)
        return result;
    if (fstatat(cache, name, &st, AT_SYMLINK_NOFOLLOW) || !identity(&st, &proof))
        return GSTD_CACHE_INTEGRITY;
    signal = cancelled(cancel);
    if (signal)
        return signal > 0 ? GSTD_CACHE_CANCELLED : GSTD_CACHE_INVALID;
    if (unlinkat(cache, name, AT_REMOVEDIR))
        return GSTD_CACHE_IO;
    return fsync(cache) ? GSTD_CACHE_UNCERTAIN : GSTD_CACHE_OK;
#else
    (void)cache;
    (void)name;
    (void)device;
    (void)inode;
    (void)cancel;
    return GSTD_CACHE_UNAVAILABLE;
#endif
}

gstd_cache_status gstd_asset_discard_staging(int cache, const char *name, const char *key,
                                             uint64_t device, uint64_t inode, int cancel) {
    if (!staging_role(name, key))
        return GSTD_CACHE_INVALID;
    return discard_role(cache, name, device, inode, cancel);
}
gstd_cache_status gstd_asset_evict(int cache, const char *key, const gstd_provision_result *proof,
                                   int cancel) {
    if (!digest(key) || !proof || !digest(proof->tree_digest) ||
        proof->logical_bytes > UINT64_C(8) * 1024 * 1024 * 1024)
        return GSTD_CACHE_INVALID;
#if defined(__linux__) && defined(SYS_openat2) && defined(SYS_renameat2)
    struct stat owner, st;
    if (fstat(cache, &owner) || !S_ISDIR(owner.st_mode) || owner.st_uid != geteuid() ||
        (owner.st_mode & 07777) != 0700)
        return GSTD_CACHE_INVALID;
    int signal = cancelled(cancel);
    if (signal)
        return signal > 0 ? GSTD_CACHE_CANCELLED : GSTD_CACHE_INVALID;
    char name[71];
    snprintf(name, sizeof(name), "evict-%s", key);
    if (!fstatat(cache, name, &st, AT_SYMLINK_NOFOLLOW)) {
        /* Recorded eviction may have been interrupted during owned cleanup. */
        if (!identity(&st, proof))
            return GSTD_CACHE_INTEGRITY;
        struct stat active;
        if (!fstatat(cache, key, &active, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
            return GSTD_CACHE_CONFLICT;
    } else {
        if (errno != ENOENT)
            return GSTD_CACHE_IO;
        if (fstatat(cache, key, &st, AT_SYMLINK_NOFOLLOW))
            return errno == ENOENT ? (fsync(cache) ? GSTD_CACHE_UNCERTAIN : GSTD_CACHE_OK)
                                   : GSTD_CACHE_IO;
        if (!identity(&st, proof))
            return GSTD_CACHE_INTEGRITY;
        struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                               .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                          RESOLVE_NO_MAGICLINKS};
        int fd = (int)syscall(SYS_openat2, cache, key, &how, sizeof(how));
        if (fd < 0)
            return errno == ENOSYS ? GSTD_CACHE_UNAVAILABLE : GSTD_CACHE_IO;
        char observed[65];
        uint64_t bytes = 0;
        int valid = !fstat(fd, &st) && identity(&st, proof) &&
                    !gstd_asset_tree_digest(fd, proof->logical_bytes ? proof->logical_bytes : 1, 1,
                                            observed, &bytes) &&
                    !strcmp(observed, proof->tree_digest) && bytes == proof->logical_bytes &&
                    !fstatat(cache, key, &st, AT_SYMLINK_NOFOLLOW) && identity(&st, proof);
        close(fd);
        if (!valid)
            return GSTD_CACHE_INTEGRITY;
        signal = cancelled(cancel);
        if (signal)
            return signal > 0 ? GSTD_CACHE_CANCELLED : GSTD_CACHE_INVALID;
        if (syscall(SYS_renameat2, cache, key, cache, name, RENAME_NOREPLACE))
            return errno == EEXIST ? GSTD_CACHE_CONFLICT : GSTD_CACHE_IO;
        if (fsync(cache))
            return GSTD_CACHE_UNCERTAIN;
    }
    return discard_role(cache, name, proof->staging_device, proof->staging_inode, cancel);
#else
    (void)cache;
    (void)cancel;
    return GSTD_CACHE_UNAVAILABLE;
#endif
}
