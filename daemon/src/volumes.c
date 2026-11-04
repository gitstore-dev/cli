#define _GNU_SOURCE 1
#include "volumes.h"
#include "gitstored/protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
static int private(int fd, int directory, struct stat *output) {
    return fd >= 3 && !fstat(fd, output) && output->st_uid == geteuid() &&
           (output->st_mode & 07777) == (directory ? 0700 : 0600) &&
           (directory ? S_ISDIR(output->st_mode)
                      : (S_ISREG(output->st_mode) && output->st_nlink == 1));
}
static int child(int parent, const char *name) {
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    return (int)syscall(SYS_openat2, parent, name, &how, sizeof(how));
}
static int marker(int directory, const char *name, const char *expected, int create) {
    int flags = create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY;
    int fd = openat(directory, name, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
        return -1;
    struct stat before, after;
    size_t length = strlen(expected);
    int valid = private(fd, 0, &before);
    if (valid && create) {
        size_t sent = 0;
        while (sent < length) {
            ssize_t amount = write(fd, expected + sent, length - sent);
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount <= 0) {
                valid = 0;
                break;
            }
            sent += (size_t)amount;
        }
        valid = valid && !fsync(fd);
    } else if (valid) {
        char bytes[1024];
        ssize_t count = pread(fd, bytes, sizeof(bytes), 0);
        valid = before.st_size == (off_t)length && count == (ssize_t)length &&
                !memcmp(bytes, expected, length) && !fstat(fd, &after) &&
                before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                before.st_size == after.st_size && before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
                before.st_ctim.tv_nsec == after.st_ctim.tv_nsec &&
                before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
    }
    if (valid && !create)
        valid = !fsync(fd);
    close(fd);
    return valid ? 0 : -1;
}
static int store_identity(const char *text) {
    if (!text || strlen(text) != 32)
        return 0;
    for (size_t i = 0; i < 32; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return 1;
}
static int unbound_empty(int fd) {
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (copy < 0)
        return 0;
    DIR *directory = fdopendir(copy);
    if (!directory) {
        close(copy);
        return 0;
    }
    rewinddir(directory);
    int valid = 1;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
            strcmp(entry->d_name, ".gitstored-volume-lock")) {
            valid = 0;
            break;
        }
    if (errno)
        valid = 0;
    closedir(directory);
    return valid;
}
static int root_valid(int directory, const gstd_volume_root *owner) {
    struct stat root, lock, named;
    return owner && owner->lock_fd >= 3 && private(directory, 1, &root) &&
           (uint64_t)root.st_dev == owner->device && (uint64_t)root.st_ino == owner->inode &&
           private(owner->lock_fd, 0, &lock) && !lock.st_size &&
           !fstatat(directory, ".gitstored-volume-lock", &named, AT_SYMLINK_NOFOLLOW) &&
           lock.st_dev == named.st_dev && lock.st_ino == named.st_ino &&
           store_identity(owner->store_id);
}
static int lease(int directory, const char *id, int create) {
    char name[96];
    snprintf(name, sizeof(name), "writer-%s.lock", id);
    int fd = openat(directory, name,
                    O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (create ? O_CREAT : 0), 0600);
    if (fd < 0)
        return -1;
    struct stat info;
    if (!private(fd, 0, &info) || info.st_size) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}
#endif
static gstd_volume_status volume_root_open(int directory, const char *store,
                                           gstd_volume_root *owner, int allow_create) {
    if (!owner || owner->lock_fd != -1 || (allow_create != 0 && allow_create != 1))
        return GSTD_VOLUME_INVALID;
#ifdef __linux__
    struct stat root, info;
    if (!store_identity(store) || !private(directory, 1, &root))
        return GSTD_VOLUME_INVALID;
    int existing = fstatat(directory, ".gitstored-volume-root", &info, AT_SYMLINK_NOFOLLOW);
    if (existing && (!allow_create || errno != ENOENT || !unbound_empty(directory)))
        return GSTD_VOLUME_INTEGRITY;
    int lock =
        openat(directory, ".gitstored-volume-lock",
               (existing ? O_CREAT : 0) | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (lock < 0)
        return errno == ENOENT ? GSTD_VOLUME_INTEGRITY : GSTD_VOLUME_IO;
    if (!private(lock, 0, &info) || info.st_size) {
        close(lock);
        return GSTD_VOLUME_INTEGRITY;
    }
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        int held = errno == EWOULDBLOCK || errno == EAGAIN;
        close(lock);
        return held ? GSTD_VOLUME_HELD : GSTD_VOLUME_IO;
    }
    char expected[256];
    snprintf(expected, sizeof(expected), "gitstored-volume-root-v1\n%s\n%llu:%llu\n%llu:%llu\n",
             store, (unsigned long long)root.st_dev, (unsigned long long)root.st_ino,
             (unsigned long long)info.st_dev, (unsigned long long)info.st_ino);
    if (marker(directory, ".gitstored-volume-root", expected, existing != 0) ||
        (allow_create && fsync(directory))) {
        close(lock);
        return GSTD_VOLUME_INTEGRITY;
    }
    int retained_root = fcntl(directory, F_DUPFD_CLOEXEC, 3);
    if (retained_root < 0) {
        close(lock);
        return GSTD_VOLUME_IO;
    }
    *owner = (gstd_volume_root){.lock_fd = lock,
                                .root_fd = retained_root,
                                .device = (uint64_t)root.st_dev,
                                .inode = (uint64_t)root.st_ino};
    memcpy(owner->store_id, store, 33);
    return GSTD_VOLUME_OK;
#else
    (void)directory;
    (void)store;
    return GSTD_VOLUME_UNAVAILABLE;
#endif
}
gstd_volume_status gstd_volume_root_lock(int directory, const char *store,
                                         gstd_volume_root *owner) {
    return volume_root_open(directory, store, owner, 1);
}
gstd_volume_status gstd_volume_root_reopen(int directory, const char *store,
                                           gstd_volume_root *owner) {
    return volume_root_open(directory, store, owner, 0);
}
void gstd_volume_root_close(gstd_volume_root *owner) {
    if (!owner)
        return;
    if (owner->lock_fd >= 3)
        close(owner->lock_fd);
    if (owner->root_fd >= 3)
        close(owner->root_fd);
    memset(owner, 0, sizeof(*owner));
    owner->lock_fd = -1;
    owner->root_fd = -1;
}
void gstd_volume_handle_close(gstd_volume_handle *handle) {
    if (!handle)
        return;
    if (handle->data_fd >= 3)
        close(handle->data_fd);
    if (handle->writer_fd >= 3)
        close(handle->writer_fd);
    memset(handle, 0, sizeof(*handle));
    handle->data_fd = -1;
    handle->writer_fd = -1;
}
static gstd_volume_status volume_open(int directory, const gstd_volume_root *owner, const char *id,
                                      const char *producer, int create,
                                      const gstd_volume_identity *expected,
                                      gstd_volume_handle *output, int claim_writer) {
    if (!output || output->data_fd != -1 || output->writer_fd != -1 || !id || !producer ||
        !gstd_identifier(id, strlen(id)) || !gstd_identifier(producer, strlen(producer)) ||
        (create != 0 && create != 1) || (!claim_writer && (create || !expected)))
        return GSTD_VOLUME_INVALID;
#ifdef __linux__
    if (!root_valid(directory, owner))
        return GSTD_VOLUME_INVALID;
    char name[96];
    snprintf(name, sizeof(name), "volume-%s", id);
    int volume = child(directory, name), writer = -1, data = -1, created = 0;
    gstd_volume_status result = GSTD_VOLUME_INTEGRITY;
    if (volume < 0) {
        if (errno != ENOENT)
            return GSTD_VOLUME_INTEGRITY;
        if (!create)
            return GSTD_VOLUME_MISSING;
        if (expected)
            return GSTD_VOLUME_INTEGRITY;
        writer = lease(directory, id, 1);
        if (writer < 0)
            return GSTD_VOLUME_INTEGRITY;
        if (flock(writer, LOCK_EX | LOCK_NB)) {
            result = errno == EWOULDBLOCK || errno == EAGAIN ? GSTD_VOLUME_HELD : GSTD_VOLUME_IO;
            goto done;
        }
        if (mkdirat(directory, name, 0700))
            goto done;
        volume = child(directory, name);
        if (volume < 0 || mkdirat(volume, "data", 0700))
            goto done;
        created = 1;
    }
    struct stat outer, inner, locked;
    if (!private(volume, 1, &outer) || (data = child(volume, "data")) < 0 ||
        !private(data, 1, &inner))
        goto done;
    if (writer < 0)
        writer = lease(directory, id, 0);
    if (writer < 0 || !private(writer, 0, &locked))
        goto done;
    gstd_volume_identity identity = {(uint64_t)outer.st_dev,  (uint64_t)outer.st_ino,
                                     (uint64_t)inner.st_dev,  (uint64_t)inner.st_ino,
                                     (uint64_t)locked.st_dev, (uint64_t)locked.st_ino};
    if (expected && (identity.volume_device != expected->volume_device ||
                     identity.volume_inode != expected->volume_inode ||
                     identity.data_device != expected->data_device ||
                     identity.data_inode != expected->data_inode ||
                     identity.writer_device != expected->writer_device ||
                     identity.writer_inode != expected->writer_inode))
        goto done;
    char record[512];
    snprintf(record, sizeof(record),
             "gitstored-repository-volume-v1\n%s\n%s\n%s\n%llu:%llu\n%llu:%llu\n%llu:%llu\n",
             owner->store_id, id, producer, (unsigned long long)identity.volume_device,
             (unsigned long long)identity.volume_inode, (unsigned long long)identity.data_device,
             (unsigned long long)identity.data_inode, (unsigned long long)identity.writer_device,
             (unsigned long long)identity.writer_inode);
    if (marker(volume, ".gitstored-volume", record, created) ||
        (claim_writer && (fsync(data) || fsync(volume) || fsync(writer) || fsync(directory))))
        goto done;
    if (claim_writer && flock(writer, LOCK_EX | LOCK_NB)) {
        result = errno == EWOULDBLOCK || errno == EAGAIN ? GSTD_VOLUME_HELD : GSTD_VOLUME_IO;
        goto done;
    }
    *output = (gstd_volume_handle){
        .data_fd = data, .writer_fd = claim_writer ? writer : -1, .identity = identity};
    data = -1;
    if (claim_writer)
        writer = -1;
    result = GSTD_VOLUME_OK;
done:
    if (volume >= 0)
        close(volume);
    if (data >= 0)
        close(data);
    if (writer >= 0)
        close(writer);
    return result;
#else
    (void)directory;
    (void)owner;
    (void)expected;
    return GSTD_VOLUME_UNAVAILABLE;
#endif
}

gstd_volume_status gstd_volume_acquire(int root, const gstd_volume_root *owner, const char *id,
                                       const char *producer, int create,
                                       const gstd_volume_identity *expected,
                                       gstd_volume_handle *output) {
    return volume_open(root, owner, id, producer, create, expected, output, 1);
}
gstd_volume_status gstd_volume_observe_open(int root, const gstd_volume_root *owner, const char *id,
                                            const char *producer,
                                            const gstd_volume_identity *expected,
                                            gstd_volume_handle *output) {
    return volume_open(root, owner, id, producer, 0, expected, output, 0);
}
