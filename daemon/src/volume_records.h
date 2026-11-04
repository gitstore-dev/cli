#ifndef GSTD_VOLUME_RECORDS_H
#define GSTD_VOLUME_RECORDS_H
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
#endif
