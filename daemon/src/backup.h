#ifndef GSTD_BACKUP_H
#define GSTD_BACKUP_H
#include <jansson.h>
#include <stdint.h>
/* Private worker-only payload inspection, outside SQLite/control-loop work.
 * Directory is an explicitly selected owned mode-0700 backup payload, never a
 * caller mount path. Only ordinary owned files/directories, no links/devices,
 * at most 131072 entries/depth 64/8 GiB. No credentials or native identities are
 * serialized. Returned metadata describes bytes; it grants no ownership.
 * Source volume must already be quiescent under its canonical writer lease.
 * After copying, inspect the independent payload, not a live datastore tree. */
json_t *gstd_backup_describe(int payload_fd, const char *registered_target, uint64_t maximum_bytes);
/* Read-only compatibility/integrity check, not restore execution authority.
 * A later restore owner must independently prove its destination's immutable
 * receipt, held writer lease and old workload termination; copied DB/backup
 * metadata cannot prove those. Native schema validation is still required after
 * opening restored data. This function never copies, overwrites or launches. */
int gstd_backup_restore_validate(int payload_fd, const json_t *manifest,
                                 const char *registered_target, uint64_t maximum_bytes);
#endif
