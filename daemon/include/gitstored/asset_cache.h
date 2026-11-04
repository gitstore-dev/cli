#ifndef GITSTORED_ASSET_CACHE_H
#define GITSTORED_ASSET_CACHE_H
#include "gitstored/provisioning.h"
typedef enum {
    GSTD_CACHE_OK = 0,
    GSTD_CACHE_INVALID = 1,
    GSTD_CACHE_INTEGRITY = 2,
    GSTD_CACHE_CANCELLED = 3,
    GSTD_CACHE_CONFLICT = 4,
    GSTD_CACHE_UNAVAILABLE = 5,
    GSTD_CACHE_IO = 6,
    GSTD_CACHE_UNCERTAIN = 7
} gstd_cache_status;
typedef struct {
    int lock_fd;
    uint64_t device, inode;
    char store_id[33];
} gstd_cache_owner;
/* Authorized manager startup only; never read-only validation. Caller initializes
 * lock_fd=-1 and retains this lifetime lock until all cache workers terminate.
 * initialize permits only an empty private cache (or its own two metadata files).
 * Existing bindings require exact expected directory device/inode and store ID.
 * Never removes/replaces lock or owner files, adopts nonempty roots, or repairs
 * malformed ownership metadata. Caller persists identity before creating assets. */
gstd_cache_status gstd_asset_cache_lock(int cache_fd, const char *store_id,
                                        uint64_t expected_device, uint64_t expected_inode,
                                        int initialize, gstd_cache_owner *owner);
void gstd_asset_cache_unlock(gstd_cache_owner *owner);
int gstd_asset_stage_name(const char *asset_key, const char *operation_id, char output[138]);
/* Native worker filesystem phase only; never call in the control loop. Caller
 * must first persist verified ownership/receipt, hold exclusive cache ownership
 * and enforce worker memory/deadline limits. This performs no DB changes and does
 * not authorize arbitrary staged bytes: proof must come from the managed SDK job.
 * Fresh content check and fsync precede no-replace atomic rename. Cache FD borrowed.
 * Cancellation FD borrowed/unconsumed; -1 means absent. UNCERTAIN means rename
 * happened but durability/identity could not be confirmed; never assume rollback.
 * Only the parent can mark active after observing a successful worker result.
 * Linux openat2/renameat2 required; no insecure or cross-filesystem fallback. */
gstd_cache_status gstd_asset_promote(int cache_fd, const char *staging_name, const char *asset_key,
                                     const gstd_provision_result *proof, int cancellation_fd);
/* Worker-only reverse cleanup of a recorded staging directory, never an active
 * root/retained volume. Parent must observe all workers stopped, release dependent
 * resources, require zero references and persist cleanup intent before dispatch.
 * Every child is opened without following links/crossing mounts; no active-name
 * fallback. Cancellation/failure can leave partial cleanup. Keep metadata until
 * successful removal/durability is observed. Same worker limits as promotion. */
gstd_cache_status gstd_asset_discard_staging(int cache_fd, const char *staging_name,
                                             const char *asset_key, uint64_t device, uint64_t inode,
                                             int cancellation_fd);
/* One recorded unreferenced asset per worker call. Parent must first commit
 * evicting state with zero references under exclusive cache ownership; no worker
 * or workload may retain the root. Fresh verification precedes moving the active
 * name to its private evict role. Repeated calls finish only that recorded inode.
 * Never selects assets, inspects DB, or traverses retained volumes. Same limits
 * and uncertain-result rules as promotion. */
gstd_cache_status gstd_asset_evict(int cache_fd, const char *asset_key,
                                   const gstd_provision_result *proof, int cancellation_fd);
#endif
