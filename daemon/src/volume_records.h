#ifndef GSTD_VOLUME_RECORDS_H
#define GSTD_VOLUME_RECORDS_H
#include "gitstored/operations.h"
#include "volumes.h"
#include <sqlite3.h>
/* Parent-only short metadata operations, within a caller transaction. Commit
 * begin before filesystem work; pass only the verified root lifetime owner.
 * Returns 0=reserved/new, 1=previously allocated, -1=invalid/conflict/limit.
 * Output producer is immutable across starts. An allocated receipt is mandatory
 * for reopening; a missing allocated directory must never be recreated. */
int gstd_volume_record_begin(sqlite3 *, const gstd_volume_root *, const char *volume_id,
                             const char *operation_id, char producer[65],
                             gstd_volume_identity *expected);
/* After the managed filesystem worker completed, use its retained descriptors
 * and receipt. Never accepts user metadata as allocation proof. Does not release
 * ownership or advance any deployment/job; only the control owner does that. */
int gstd_volume_record_ready(sqlite3 *, const gstd_volume_root *, const char *volume_id,
                             const char *operation_id, const gstd_volume_handle *);
/* Parent-only read for an unreleased original start. No creation/rebinding or
 * writer grant; returns the immutable allocated producer/inode snapshot. */
int gstd_volume_record_recovery_read(sqlite3 *, const gstd_volume_root *, const char *operation_id,
                                     char volume_id[65], char producer_id[65],
                                     gstd_volume_identity *);
/* Reserve the canonical datastore identity for the current accepted/running
 * Scylla START inside the caller transaction. A prior held/uncertain owner is
 * never rebound; same-operation replay preserves its stage/receipt. This does
 * not grant a writer lease or native execution. Commit before filesystem work. */
gstd_accept_status gstd_scylla_volume_reserve(sqlite3 *, const char *operation, char volume_id[65]);
/* Running admitted standalone migration, caller transaction. Requires the
 * allocated canonical datastore from its released source operation; no new
 * ledger/data allocation or writer authority. Same-op replay preserves receipts. */
gstd_accept_status gstd_scylla_migration_volume_reserve(sqlite3 *, const char *, char[65]);
/* Same trusted transaction/worker boundaries, with a distinct datastore ledger
 * and fixed Scylla disk/schema compatibility. No public capability is enabled. */
int gstd_scylla_volume_record_begin(sqlite3 *, const gstd_volume_root *, const char *, const char *,
                                    char[65], gstd_volume_identity *);
int gstd_scylla_volume_record_ready(sqlite3 *, const gstd_volume_root *, const char *, const char *,
                                    const gstd_volume_handle *);
int gstd_scylla_volume_record_recovery_read(sqlite3 *, const gstd_volume_root *, const char *,
                                            char[65], char[65], gstd_volume_identity *);
/* Private core/2 OIDC coordinator primitives. Separate canonical volume IDs and
 * per-provider immutable ledgers; no public capability or migration authority.
 * Same caller transaction, committed reservation and filesystem-worker rules. */
gstd_accept_status gstd_oidc_volume_reserve(sqlite3 *, gstd_oidc_volume_role, const char *,
                                            char[65]);
int gstd_oidc_volume_record_begin(sqlite3 *, const gstd_volume_root *, gstd_oidc_volume_role,
                                  const char *, const char *, char[65], gstd_volume_identity *);
int gstd_oidc_volume_record_ready(sqlite3 *, const gstd_volume_root *, gstd_oidc_volume_role,
                                  const char *, const char *, const gstd_volume_handle *);
int gstd_oidc_volume_record_recovery_read(sqlite3 *, const gstd_volume_root *,
                                          gstd_oidc_volume_role, const char *, char[65], char[65],
                                          gstd_volume_identity *);
#endif
