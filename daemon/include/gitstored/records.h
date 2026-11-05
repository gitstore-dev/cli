#ifndef GITSTORED_RECORDS_H
#define GITSTORED_RECORDS_H
#include <sqlite3.h>
/* Register bounded, innocuous schema functions before loading/validating schema. */
int gstd_records_functions(sqlite3 *db);
int gstd_records_create(sqlite3 *db);
int gstd_records_check(sqlite3 *db);
/* Exact version-1 schema only; caller owns the atomic store-upgrade transaction. */
int gstd_records_upgrade_v1(sqlite3 *db);
/* Exact version-2 schema only; preserves all existing operation/resource rows. */
int gstd_records_upgrade_v2(sqlite3 *db);
/* Exact version-3 schema only; permits bounded standalone migration attachments. */
int gstd_records_upgrade_v3(sqlite3 *db);
/* Atomic V4 migration adds distinct retained provider-volume ownership. */
int gstd_records_upgrade_v4(sqlite3 *db);
#endif
