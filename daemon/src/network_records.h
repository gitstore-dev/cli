#ifndef GSTD_NETWORK_RECORDS_H
#define GSTD_NETWORK_RECORDS_H
#include "network_worker.h"
#include <sqlite3.h>
/* Parent-only, caller-owned transaction. Resource reservation and accepted start
 * must already exist for this UID/store/operation. Commit bind BEFORE permit.
 * These functions never commit, permit, signal, create or release a resource. */
int gstd_network_record_bind(sqlite3 *, const gstd_network_worker_request *,
                             const gstd_network_worker *);
int gstd_network_record_running(sqlite3 *, const gstd_network_worker *,
                                const gstd_network_worker_observation *);
/* Record the exact exported namespace receipt BEFORE workload CREATE. Both
 * resources must belong to the same accepted start. Never reassign an identity. */
int gstd_network_attachment_reserve(sqlite3 *, const gstd_network_worker *,
                                    const gstd_network_worker_observation *, const char *);
/* Read-only, caller transaction: match held ownership, current start and live
 * namespace capabilities before workload dispatch. */
int gstd_network_attachment_matches(sqlite3 *, const char *operation_id, const char *workload_id,
                                    const gstd_network_namespace *);
#endif
