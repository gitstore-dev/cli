#ifndef GSTD_MIGRATION_HISTORY_H
#define GSTD_MIGRATION_HISTORY_H
#include <jansson.h>
#include <stddef.h>
/* Private image-native CQL result only, never caller wire observations. Validate
 * exact registered file/checksum/statement identities. Preflight permits a
 * completed prefix and at most its final incomplete entry; complete mode needs
 * the entire completed bundle. No mutation, retry or lease authority. */
int gstd_migration_history_validate(const char *target_version, const json_t *rows,
                                    int require_complete);
/* Decode complete pinned cqlsh JSON result sets from private helper stdout.
 * At most 64 KiB, two sets and 13 rows/set. Every header/separator/footer is
 * required; truncated, extra or malformed output grants no observation. */
json_t *gstd_migration_cql_decode(const void *, size_t, unsigned result_sets);
/* Exact two-query inspection result. Missing keyspace is a precondition
 * failure; an existing keyspace may have no migration ledger yet. */
int gstd_migration_schema_inspection(const json_t *, int *ledger_exists);
/* Linux private helper stdout, regular anonymous memory file (no SIGPIPE when
 * the manager exits). Pre-sized, growth-sealed storage bounds writes to 64 KiB
 * independently of helper scratch files. Shared output offset records length. Read/seal
 * only after observed helper exit, empty cgroup and committed native deletion. */
int gstd_migration_output_create(void);
json_t *gstd_migration_output_rows(int descriptor, unsigned result_sets);
#endif
