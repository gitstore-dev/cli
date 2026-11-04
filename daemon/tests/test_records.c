#include "gitstored/state.h"
#include "support.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define OK(sql) CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK)
#define BAD(sql) CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK)
int main(void) {
    int result = 0;
    char *root = gstd_test_dir();
    sqlite3 *db = NULL;
    char path[256];
    CHECK(root);
    CHECK(snprintf(path, sizeof(path), "%s/records.db", root) > 0);
    CHECK(!gstd_state_open(path, &db));
    OK("INSERT INTO profiles VALUES('core','v1','source','digest','linux-arm64')");
    BAD("INSERT INTO profiles VALUES('../unsafe','v1','source','digest','linux-arm64')");
    BAD("INSERT INTO profiles VALUES(CAST(x'610062' AS "
        "TEXT),'v1','source','digest','linux-arm64')");
    OK("INSERT INTO deployments "
       "VALUES('d1','core','v1','running',1,0,'pending','normal','held','2026-11-03T00:00:00Z',"
       "NULL)");
    BAD("UPDATE deployments SET readiness='ready' WHERE deployment_id='d1'");
    BAD("UPDATE deployments SET generation=0 WHERE deployment_id='d1'");
    BAD("UPDATE deployments SET profile_version='missing' WHERE deployment_id='d1'");
    OK("INSERT INTO operations "
       "VALUES('o1',1,'deployment.start','d1',NULL,'"
       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','{}','2026-11-03T00:00:"
       "00Z','2026-11-03T00:00:00Z','accepted','normal',NULL,NULL)");
    BAD("UPDATE operations SET result='{}' WHERE operation_id='o1'");
    BAD("UPDATE operations SET state='succeeded',result='{}',error='{}' WHERE operation_id='o1'");
    OK("UPDATE operations SET state='running' WHERE operation_id='o1'");
    OK("UPDATE operations SET state='succeeded',result='{}' WHERE operation_id='o1'");
    BAD("UPDATE operations SET result='{\"changed\":true}' WHERE operation_id='o1'");
    OK("UPDATE deployments SET observed_generation=1,readiness='ready',last_operation_id='o1' "
       "WHERE deployment_id='d1'");
    OK("INSERT INTO resource_ownership "
       "VALUES('volume','repository-volume','d1','o1','running','backend-identity','held','2026-11-"
       "03T00:00:00Z')");
    BAD("INSERT INTO resource_ownership "
        "VALUES('volume','repository-volume','d1','o1','reserved',NULL,'held','2026-11-03T00:00:"
        "00Z')");
    BAD("INSERT INTO components "
        "VALUES('d1','arbitrary',NULL,'digest','running','pending',1,'2026-11-03T00:00:00Z')");
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    OK("DROP TABLE components");
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(gstd_state_open(path, &db) != 0 && db == NULL);
done:
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
