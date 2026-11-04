#ifndef GITSTORED_RECORDS_H
#define GITSTORED_RECORDS_H
#include <sqlite3.h>
/* Register bounded, innocuous schema functions before loading/validating schema. */
int gstd_records_functions(sqlite3 *db);
int gstd_records_create(sqlite3 *db);
int gstd_records_check(sqlite3 *db);
#endif
