#ifndef GSTD_MIGRATION_SCHEMA_H
#define GSTD_MIGRATION_SCHEMA_H
#include "datastore.h"
typedef struct {
    gstd_datastore *datastore;
    const gstd_network_namespace *network;
    int resolver_fd, output_fd;
    int stage, outcome, ledger_exists;
    int64_t readiness_deadline_ms, next_readiness_ms;
} gstd_migration_schema;
/* Borrow a prepared standalone datastore and its exact running network/service.
 * The parent retains all capabilities through close. No public booleans, custom
 * CQL, caller command or keyspace-creation path. Zero-initialized actor. */
int gstd_migration_schema_begin(gstd_datastore *, const gstd_network_namespace *, int resolver_fd,
                                gstd_migration_schema *);
/* Nonblocking 0 pending,1 finished,-1 invalid. Outcome0 means full post-helper
 * schema validation and durable cleanup stage, not migration completion. Source,
 * network and writer cleanup must still be observed before finish(success).
 * Unknown helper outcomes keep their journal and prohibit source shutdown. */
int gstd_migration_schema_poll(gstd_migration_schema *, int cancellation_fd);
/* Local output observer closure only after finish; never closes helper owners,
 * stops the source or releases a reservation. */
int gstd_migration_schema_close(gstd_migration_schema *);
#endif
