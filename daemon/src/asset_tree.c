#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/asset_tree.h"
#include "owned_read.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif
static int beneath(int directory, const char *name, int flags) {
#if defined(__linux__) && defined(SYS_openat2)
    struct open_how how = {.flags = (uint64_t)flags,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS};
    return (int)syscall(SYS_openat2, directory, name, &how, sizeof(how));
#elif defined(__linux__)
    (void)directory;
    (void)name;
    (void)flags;
    return -1;
#else
    return openat(directory, name, flags);
#endif
}
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX hash;
static int begin(hash *h) { return CC_SHA256_Init(h) == 1 ? 0 : -1; }
static int update(hash *h, const void *bytes, size_t length) {
    return CC_SHA256_Update(h, bytes, (CC_LONG)length) == 1 ? 0 : -1;
}
static int finish(hash *h, unsigned char out[32]) { return CC_SHA256_Final(out, h) == 1 ? 0 : -1; }
static void release(hash *h) { (void)h; }
#else
#include <openssl/evp.h>
typedef EVP_MD_CTX *hash;
static int begin(hash *h) {
    *h = EVP_MD_CTX_new();
    if (*h && EVP_DigestInit_ex(*h, EVP_sha256(), NULL) == 1)
        return 0;
    EVP_MD_CTX_free(*h);
    *h = NULL;
    return -1;
}
static int update(hash *h, const void *bytes, size_t length) {
    return EVP_DigestUpdate(*h, bytes, length) == 1 ? 0 : -1;
}
static int finish(hash *h, unsigned char out[32]) {
    unsigned size = 0;
    return EVP_DigestFinal_ex(*h, out, &size) == 1 && size == 32 ? 0 : -1;
}
static void release(hash *h) { EVP_MD_CTX_free(*h); }
#endif
#define ENTRY_LIMIT 131072u
typedef struct {
    dev_t device;
    ino_t inode;
    nlink_t links;
} link_record;
typedef struct {
    hash digest;
    dev_t device;
    uint64_t maximum, bytes;
    size_t entries, link_count;
    link_record *links;
    int synchronize;
} tree;
static int consume_owned(void *context, const unsigned char *bytes, size_t length) {
    tree *t = context;
    return update(&t->digest, bytes, length);
}
static int number(tree *t, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < 8; i++)
        bytes[7 - i] = (unsigned char)(value >> (i * 8));
    return update(&t->digest, bytes, sizeof(bytes));
}
static int stable(const struct stat *a, const struct stat *b) {
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_size != b->st_size ||
        a->st_mode != b->st_mode || a->st_uid != b->st_uid || a->st_gid != b->st_gid ||
        a->st_nlink != b->st_nlink)
        return 0;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}
static int trusted(tree *t, const struct stat *st) {
    return st->st_dev == t->device && st->st_uid == geteuid() && st->st_nlink > 0 &&
           !(st->st_mode & 06000);
}
static int compare_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static int compare_link(const void *a, const void *b) {
    const link_record *x = a, *y = b;
    if (x->device != y->device)
        return x->device < y->device ? -1 : 1;
    return x->inode < y->inode ? -1 : x->inode > y->inode ? 1 : 0;
}
static int walk(tree *t, int directory, unsigned depth);
static int entry(tree *t, int directory, const char *name, unsigned depth) {
    struct stat before, after;
    if (fstatat(directory, name, &before, AT_SYMLINK_NOFOLLOW) || !trusted(t, &before))
        return -1;
    unsigned char kind = S_ISREG(before.st_mode)   ? 'F'
                         : S_ISDIR(before.st_mode) ? 'D'
                         : S_ISLNK(before.st_mode) ? 'L'
                                                   : 0;
    if (!kind || update(&t->digest, &kind, 1) || number(t, strlen(name)) ||
        update(&t->digest, name, strlen(name)) || number(t, (uint64_t)(before.st_mode & 07777)) ||
        number(t, (uint64_t)before.st_gid))
        return -1;
    int result = -1, fd = -1;
    if (kind == 'L') {
        char target[4097];
        ssize_t length = readlinkat(directory, name, target, sizeof(target));
        if (length <= 0 || length > 4096 || number(t, (uint64_t)length) ||
            update(&t->digest, target, (size_t)length))
            return -1;
        result = 0;
    } else {
        int delegated = 0;
        fd = beneath(directory, name,
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                         (kind == 'D' ? O_DIRECTORY : 0));
#ifdef __linux__
        if (fd < 0 && errno == EACCES && kind == 'F') {
            fd = beneath(directory, name, O_PATH | O_CLOEXEC | O_NOFOLLOW);
            delegated = fd >= 0;
        }
#endif
        struct stat opened;
        if (fd < 0 || fstat(fd, &opened) || !stable(&before, &opened))
            goto done;
        if (kind == 'D') {
            if (walk(t, fd, depth + 1))
                goto done;
        } else {
            if (before.st_size < 0 || (uint64_t)before.st_size > t->maximum - t->bytes ||
                number(t, (uint64_t)before.st_size))
                goto done;
            if (before.st_nlink > 1) {
                if (t->link_count == ENTRY_LIMIT)
                    goto done;
                t->links[t->link_count++] =
                    (link_record){before.st_dev, before.st_ino, before.st_nlink};
            }
            if (delegated) {
                if (gstd_owned_read(fd, (uint64_t)before.st_size, t->synchronize, consume_owned, t))
                    goto done;
            } else {
                uint64_t remaining = (uint64_t)before.st_size;
                unsigned char buffer[32768];
                while (remaining) {
                    size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
                    ssize_t length = read(fd, buffer, wanted);
                    if (length < 0 && errno == EINTR)
                        continue;
                    if (length <= 0 || update(&t->digest, buffer, (size_t)length))
                        goto done;
                    remaining -= (uint64_t)length;
                }
                unsigned char extra;
                if (read(fd, &extra, 1) != 0 || (t->synchronize && fsync(fd)))
                    goto done;
            }
            t->bytes += (uint64_t)before.st_size;
        }
        if (fstat(fd, &opened) || !stable(&before, &opened))
            goto done;
        result = 0;
    }
    if (fstatat(directory, name, &after, AT_SYMLINK_NOFOLLOW) || !stable(&before, &after))
        result = -1;
done:
    if (fd >= 0)
        close(fd);
    return result;
}
static int walk(tree *t, int directory, unsigned depth) {
    struct stat before, after;
    if (depth > 64 || fstat(directory, &before) || !S_ISDIR(before.st_mode) || !trusted(t, &before))
        return -1;
    /* A fresh description preserves the caller's directory enumeration offset. */
    int scan = beneath(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (scan < 0)
        return -1;
    DIR *stream = fdopendir(scan);
    if (!stream) {
        close(scan);
        return -1;
    }
    char **names = NULL;
    size_t count = 0, capacity = 0;
    int result = -1;
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
        if (t->entries == ENTRY_LIMIT || strlen(item->d_name) > 255)
            goto done;
        t->entries++;
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
    if (count > 1)
        qsort(names, count, sizeof(*names), compare_name);
    if (number(t, (uint64_t)(before.st_mode & 07777)) || number(t, (uint64_t)before.st_gid) ||
        number(t, count))
        goto done;
    for (size_t i = 0; i < count; i++)
        if (entry(t, directory, names[i], depth))
            goto done;
    if ((t->synchronize && fsync(directory)) || fstat(directory, &after) ||
        !stable(&before, &after))
        goto done;
    result = 0;
done:
    if (stream)
        closedir(stream);
    for (size_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return result;
}
int gstd_asset_tree_digest(int fd, uint64_t maximum_bytes, int synchronize, char output[65],
                           uint64_t *logical_bytes) {
    if (!output || !logical_bytes)
        return -1;
    output[0] = 0;
    *logical_bytes = 0;
    if (!maximum_bytes || maximum_bytes > UINT64_C(8) * 1024 * 1024 * 1024 ||
        (synchronize != 0 && synchronize != 1))
        return -1;
    struct stat root;
    if (fstat(fd, &root) || !S_ISDIR(root.st_mode) || root.st_uid != geteuid())
        return -1;
    tree t = {.device = root.st_dev, .maximum = maximum_bytes, .synchronize = synchronize};
    t.links = calloc(ENTRY_LIMIT, sizeof(*t.links));
    if (!t.links)
        return -1;
    int result = -1;
    if (begin(&t.digest))
        goto done;
    const char prefix[] = "gitstored-asset-tree-v1";
    if (update(&t.digest, prefix, sizeof(prefix)) || walk(&t, fd, 0))
        goto hash_done;
    if (t.link_count > 1)
        qsort(t.links, t.link_count, sizeof(*t.links), compare_link);
    for (size_t i = 0; i < t.link_count;) {
        size_t end = i + 1;
        while (end < t.link_count && !compare_link(&t.links[i], &t.links[end])) {
            if (t.links[i].links != t.links[end].links)
                goto hash_done;
            end++;
        }
        if ((uint64_t)(end - i) != (uint64_t)t.links[i].links)
            goto hash_done;
        i = end;
    }
    unsigned char digest[32];
    if (finish(&t.digest, digest))
        goto hash_done;
    const char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        output[2 * i] = alphabet[digest[i] >> 4];
        output[2 * i + 1] = alphabet[digest[i] & 15];
    }
    output[64] = 0;
    *logical_bytes = t.bytes;
    result = 0;
hash_done:
    release(&t.digest);
done:
    free(t.links);
    return result;
}
