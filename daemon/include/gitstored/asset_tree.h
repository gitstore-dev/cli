#ifndef GITSTORED_ASSET_TREE_H
#define GITSTORED_ASSET_TREE_H
#include <stdint.h>
/* Internal rootfs snapshot, not upstream authenticity by itself. Compute only
 * after trusted SDK verification; retain expected digest in manager-owned state.
 * Borrowed directory FD; no caller path is reopened or symlink followed. Bounds:
 * 131072 entries, depth 64, 4096-byte symlink target, <=8 GiB logical file bytes.
 * Hard links must all reside in this tree. Logical bytes count each file name.
 * synchronize=1 additionally fsyncs every regular file/directory before success.
 * Output is cleared on failure. Caller holds exclusive staging/cache ownership. */
int gstd_asset_tree_digest(int directory_fd, uint64_t maximum_bytes, int synchronize,
                           char digest[65], uint64_t *logical_bytes);
#endif
