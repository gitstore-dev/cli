#ifndef GSTD_OWNED_READ_H
#define GSTD_OWNED_READ_H
#include <stddef.h>
#include <stdint.h>
/* Read an already pinned owned regular file without changing its mode. Used
 * only when ordinary cache auditing gets EACCES. The fresh self-exec worker
 * receives no credentials, paths or unrelated FDs; its private user namespace
 * maps only the invoking UID/GID and retains DAC_READ_SEARCH alone. */
int gstd_owned_read(int file_fd, uint64_t bytes, int synchronize,
                    int (*consume)(void *, const unsigned char *, size_t), void *context);
/* Internal self-exec entry, before ordinary daemon argument/config handling.
 * -1 means not this mode; other values are process exit statuses. */
int gstd_owned_read_entry(int argc, char **argv);
#endif
