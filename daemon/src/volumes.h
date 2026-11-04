#ifndef GSTD_VOLUMES_H
#define GSTD_VOLUMES_H
#include <stdint.h>
typedef enum {
    GSTD_VOLUME_OK,
    GSTD_VOLUME_INVALID,
    GSTD_VOLUME_UNAVAILABLE,
    GSTD_VOLUME_HELD,
    GSTD_VOLUME_MISSING,
    GSTD_VOLUME_INTEGRITY,
    GSTD_VOLUME_IO
} gstd_volume_status;
typedef struct {
    uint64_t volume_device, volume_inode, data_device, data_inode;
    uint64_t writer_device, writer_inode;
} gstd_volume_identity;
typedef struct {
    int lock_fd, root_fd;
    uint64_t device, inode;
    char store_id[33];
} gstd_volume_root;
typedef struct {
    int data_fd, writer_fd;
    gstd_volume_identity identity;
} gstd_volume_handle;
/* Private Linux managed root, bound to exactly one persistent store and actual
 * directory identity. Initial lock_fd=-1. Never adopt an unmarked nonempty root.
 * Parent holds this lifetime lock; it does not replace the durable writer ledger. */
gstd_volume_status gstd_volume_root_lock(int root_fd, const char *store_id, gstd_volume_root *);
/* Existing protected marker and lock required; never create or repair them. */
gstd_volume_status gstd_volume_root_reopen(int root_fd, const char *store_id, gstd_volume_root *);
void gstd_volume_root_close(gstd_volume_root *);
/* Blocking filesystem worker primitive, outside control-loop/SQLite transactions.
 * Parent MUST commit the named repository-volume reservation before any creation.
 * IDs are internal recorded identities, never caller mount paths. The immutable
 * producer identity persists across later starts; it is not a new operation ID.
 * Optional expected identity is the parent's durable receipt, not input authority.
 * Incomplete/foreign/moved roots are retained and fail; no recursive cleanup,
 * adoption of unmarked data, datastore role, or volume deletion is provided.
 * Initial handle descriptors=-1; retain writer_fd until observed safe release,
 * including an intentional native-workload descriptor across manager failure.
 * Merely acquiring a free flock never proves an old workload terminated. */
gstd_volume_status gstd_volume_acquire(int root_fd, const gstd_volume_root *, const char *volume_id,
                                       const char *producer_operation_id, int allow_create,
                                       const gstd_volume_identity *expected, gstd_volume_handle *);
/* Recovery filesystem worker only. Verify exact existing marker/inodes and
 * open data without taking a writer lease, creating files or syncing them.
 * Mandatory durable receipt; output writer_fd remains -1 even if another writer
 * is alive. This is observation, never native CREATE/mount/write permission. */
gstd_volume_status gstd_volume_observe_open(int root_fd, const gstd_volume_root *,
                                            const char *volume_id,
                                            const char *producer_operation_id,
                                            const gstd_volume_identity *, gstd_volume_handle *);
/* Closes this process's handles only. Never deletes data or releases a DB owner. */
void gstd_volume_handle_close(gstd_volume_handle *);
#endif
