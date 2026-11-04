#include <jansson.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    if (strcmp(jansson_version_str(), "2.15.1") != 0 || sqlite3_libversion_number() != 3051003 ||
        JSON_PARSER_MAX_DEPTH != 33 || !sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") ||
        sqlite3_threadsafe() != 0) {
        fputs("Unexpected native dependency build\n", stderr);
        return 1;
    }
    json_error_t error;
    json_t *value = json_loads("{\"a\":1,\"a\":2}", JSON_REJECT_DUPLICATES, &error);
    if (value != NULL) {
        json_decref(value);
        return 1;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK)
        return 1;
    int rc = sqlite3_exec(db, "CREATE TABLE t(n);BEGIN;INSERT INTO t VALUES(1);ROLLBACK;", NULL,
                          NULL, NULL);
    sqlite3_stmt *statement = NULL;
    if (rc != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT count(*) FROM t", -1, &statement, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 1;
    }
    int valid = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 0;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return valid ? 0 : 1;
}
