#ifndef GITSTORED_ASSET_RECORDS_H
#define GITSTORED_ASSET_RECORDS_H
#include "gitstored/asset_cache.h"
#include "gitstored/provisioning.h"
#include <sqlite3.h>
typedef enum {
    GSTD_ASSET_RECORD_ERROR = -1,
    GSTD_ASSET_RECORD_NEW = 0,
    GSTD_ASSET_RECORD_ACTIVE = 1,
    GSTD_ASSET_RECORD_HELD = 2,
    GSTD_ASSET_RECORD_CONSENT = 3,
    GSTD_ASSET_RECORD_LIMIT = 4
} gstd_asset_record_status;
/* Read expected store/cache identity without creating a cache: 0=bound, 1=new,
 * -1=invalid. Output lock_fd=-1. Use it with cache_lock, retaining that lifetime
 * lock; then bind the verified owner in a caller transaction before assets. */
int gstd_asset_record_cache_state(sqlite3 *db, gstd_cache_owner *expected);
int gstd_asset_record_cache_bind(sqlite3 *db, const gstd_cache_owner *owner);
/* Parent-only short metadata operations. Mutations require an existing caller
 * transaction; rollback the entire transaction on error. No network/unpack/hash
 * or other long worker work is performed here. Ordinary reads never create assets.
 * begin binds a registered component to an accepted/running owned start intent and
 * already-reserved asset resource, reserves cache bytes and pins its reference.
 * NEW alone authorizes creating the generated staging name, after commit. ACTIVE
 * requires fresh worker content verification before mounting. HELD is no takeover. */
gstd_asset_record_status gstd_asset_record_begin(sqlite3 *db, const json_t *manifest,
                                                 const char *component, const char *operation_id,
                                                 const char *resource_id, uint64_t download_limit,
                                                 uint64_t merge_limit, uint64_t cache_limit,
                                                 char asset_key[65], char staging_name[138]);
/* Explicit owner offline input may reserve without network provisioning consent.
 * The source kind and borrowed directory identity are immutable in the ledger;
 * no path or broad boolean can turn this authorization into a registry download.
 * Caller holds the source FD through the managed import worker. */
gstd_asset_record_status gstd_asset_record_begin_offline(
    sqlite3 *db, const json_t *manifest, int source_fd, const char *component,
    const char *operation_id, const char *resource_id, uint64_t download_limit,
    uint64_t merge_limit, uint64_t cache_limit, char asset_key[65], char staging_name[138]);
/* Mandatory parent pre-dispatch recheck: -1 source means registry, otherwise the
 * exact offline directory. Changed/missing source never falls back to network. */
int gstd_asset_record_source(sqlite3 *db, const char *asset_key, const char *operation_id,
                             int source_fd);
/* Read a managed active receipt only for this held reference and caller-owned
 * start. Does not verify current content: a worker must recheck the named inode
 * and hash before exposing any root to a workload. */
int gstd_asset_record_receipt(sqlite3 *db, const char *asset_key, const char *operation_id,
                              const char *resource_id, gstd_provision_result *);
int gstd_asset_record_identity(sqlite3 *db, const char *asset_key, const char *operation_id,
                               int staging_fd);
/* Only a managed SDK worker receipt can authorize verification, never external
 * bundle metadata or a self-checksummed descriptor. */
int gstd_asset_record_verified(sqlite3 *db, const char *asset_key, const char *operation_id,
                               const gstd_provision_result *proof);
/* After successful managed filesystem promotion, check the named active inode
 * and absent staging name before making its metadata active. Linux only. */
int gstd_asset_record_active(sqlite3 *db, const char *asset_key, const char *operation_id,
                             int cache_fd);
/* Removal is permitted only after recorded resource release/observed cleanup. */
int gstd_asset_record_unpin(sqlite3 *db, const char *resource_id);
int gstd_asset_record_refcount(sqlite3 *db, const char *asset_key, uint64_t *count);
/* Parent transaction: mark exactly one active unreferenced asset evicting and
 * return its immutable managed receipt. Blocks subsequent acquisition/reference
 * attachment. Commit before the separate filesystem worker. Repeated calls may
 * recover the same eviction; failure/uncertainty retains the row and disk budget.
 * This does not delete metadata or authorize retained-volume cleanup. */
int gstd_asset_record_evict_begin(sqlite3 *db, const char *asset_key, gstd_provision_result *proof);
/* Bounded metadata selection: recover one pending eviction first, otherwise one
 * unreferenced cache entry. 0=candidate, 1=none, -1=error. Does not authorize an
 * effect: caller must still transact evict_begin before dispatch. */
int gstd_asset_record_evict_candidate(sqlite3 *db, char asset_key[65]);
/* After observed successful worker removal/directory sync, require the bound
 * cache and absence of both recorded names before dropping the evicting row and
 * releasing budget. No reference may exist. Caller transaction required. */
int gstd_asset_record_evict_finish(sqlite3 *db, const char *asset_key, int cache_fd);
#endif
