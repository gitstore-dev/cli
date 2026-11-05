#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/operations.h"
#include "gitstored/state.h"
#include "retention.h"
#include "support.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #x,                                    \
                    db ? sqlite3_errmsg(db) : "no db");                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static sqlite3_int64 count(sqlite3 *db, const char *query) {
    sqlite3_stmt *s = NULL;
    sqlite3_int64 result = -1;
    if (sqlite3_prepare_v2(db, query, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        result = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return result;
}
static int run(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
int main(void) {
    gstd_json_initialize();
    alarm(40);
    int result = 1;
    char *root = gstd_test_dir(), path[512], source[65], replay[65], sql[256];
    sqlite3 *db = NULL;
    json_t *message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1,
                                "minor", 0, "requestId", "retention", "method", "deployment.start",
                                "idempotencyKey", "original", "params", "deploymentId",
                                "deployment", "profileId", "core", "profileVersion", "1");
    json_t *components = json_pack("[s]", "git");
    gstd_request request;
    CHECK(root && message && components);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    CHECK(!run(db, "INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64')"));
    CHECK(!gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, NULL, source) == GSTD_ACCEPT_NEW);
    CHECK(!run(
        db,
        "UPDATE operations SET state='running';UPDATE operations SET "
        "state='succeeded',result='{}',accepted_at='2000-01-01T00:00:00Z',"
        "updated_at='2000-01-01T00:00:00Z';UPDATE jobs SET state='done',worker_slot=NULL;"
        "UPDATE dedupe SET expires_after=0;"
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<300) "
        "INSERT INTO operations SELECT 'expired-'||x,0,'operation.cancel',NULL,NULL,"
        "printf('%064d',0),'{}','2000-01-01T00:00:00Z','2000-01-01T00:00:00Z',"
        "'succeeded','normal','{}',NULL FROM n;"
        "INSERT INTO operations SELECT 'keep-'||column1,0,'operation.cancel',"
        "'deployment',NULL,printf('%064d',0),'{}','2000-01-01T00:00:00Z',"
        "'2000-01-01T00:00:00Z','succeeded','normal','{}',NULL FROM "
        "(VALUES('resource'),('last'),('target'),('lease'),('dedupe'),('recent'),('active'),('"
        "uncertain'));"
        "UPDATE operations SET updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
        "operation_id='keep-recent';"
        "UPDATE operations SET recovery='uncertain' WHERE operation_id='keep-uncertain';"
        "INSERT INTO operations VALUES('active',0,'operation.cancel',NULL,'keep-target',"
        "printf('%064d',0),'{}','2000-01-01T00:00:00Z','2000-01-01T00:00:00Z',"
        "'running','normal',NULL,NULL);"
        "UPDATE deployments SET last_operation_id='keep-last';"
        "INSERT INTO resource_ownership VALUES('cfg','configuration','deployment','keep-resource',"
        "'retained',NULL,'released','2000-01-01T00:00:00Z');"
        "INSERT INTO jobs VALUES('keep-lease','runtime','running',0);"
        "INSERT INTO dedupe VALUES(0,'operation.cancel','protected',printf('%064d',0),"
        "'keep-dedupe',unixepoch('now')+1000)"));
    /* Terminal age, outstanding observers, ownership tombstones, start identity,
     * referenced targets and unexpired replay keys all survive collection. */
    unsigned removed = 999;
    CHECK(gstd_retention_collect(db, &removed) == -1);
    CHECK(!run(db, "BEGIN IMMEDIATE"));
    CHECK(!gstd_retention_collect(db, &removed) && removed == 256);
    CHECK(count(db, "SELECT count(*) FROM operations WHERE operation_id LIKE 'expired-%'") == 44);
    CHECK(!run(db, "ROLLBACK"));
    CHECK(count(db, "SELECT count(*) FROM operations WHERE operation_id LIKE 'expired-%'") == 300);
    CHECK(!run(db, "BEGIN IMMEDIATE"));
    CHECK(!gstd_retention_collect(db, &removed) && removed == 256);
    CHECK(!gstd_retention_collect(db, &removed) && removed == 45);
    /* keep-active is a terminal fixture with no active dependency, so it expires. */
    CHECK(!run(db, "COMMIT"));
    CHECK(count(db, "SELECT count(*) FROM operations WHERE operation_id LIKE 'keep-%'") == 7);
    CHECK(count(db, "SELECT count(*) FROM operations WHERE operation_id='active'") == 1);
    CHECK(count(db, "SELECT count(*) FROM operations WHERE method='deployment.start'") == 1);
    CHECK(gstd_accept(db, &request, components, NULL, replay) == GSTD_ACCEPT_REPLAY &&
          !strcmp(source, replay));
    CHECK(count(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    /* Exhaust only the disposable database's page quota. Replay still works;
     * a fresh mutation is refused without deployment/job/resource changes. */
    sqlite3_int64 pages = count(db, "PRAGMA page_count");
    CHECK(pages > 0);
    snprintf(sql, sizeof(sql), "PRAGMA max_page_count=%lld", (long long)pages + 32);
    CHECK(!run(db, sql));
    json_object_set_new(message, "idempotencyKey", json_string("no-headroom"));
    json_object_set_new(json_object_get(message, "params"), "deploymentId", json_string("new"));
    CHECK(!gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, "new-volume", replay) == GSTD_ACCEPT_LIMIT);
    CHECK(count(db, "SELECT count(*) FROM deployments WHERE deployment_id='new'") == 0);
    CHECK(count(db, "SELECT count(*) FROM resource_ownership WHERE resource_id='new-volume'") == 0);
    CHECK(!run(db, "PRAGMA max_page_count=32000"));
    /* Reserve the last 128 record positions for stop/cancellation. Recent
     * history cannot be evicted merely to admit another normal start. */
    CHECK(!run(db, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n "
                   "WHERE x<99872-(SELECT count(*) FROM operations)) INSERT INTO operations "
                   "SELECT 'recent-'||x,0,'operation.cancel',NULL,NULL,printf('%064d',0),'{}',"
                   "strftime('%Y-%m-%dT%H:%M:%SZ','now'),strftime('%Y-%m-%dT%H:%M:%SZ','now'),"
                   "'cancelled','normal','{}',NULL FROM n"));
    CHECK(count(db, "SELECT count(*) FROM operations") == 99872);
    CHECK(gstd_accept(db, &request, components, NULL, replay) == GSTD_ACCEPT_LIMIT);
    CHECK(!run(db, "BEGIN IMMEDIATE"));
    CHECK(gstd_retention_admit(db, 1, 100) == 0);
    CHECK(!run(db, "ROLLBACK"));
    CHECK(count(db, "SELECT count(*) FROM operations") == 99872);
    json_t *stop = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor", 0,
                             "requestId", "stop", "method", "deployment.stop", "idempotencyKey",
                             "reserved-stop", "params", "deploymentId", "deployment");
    CHECK(stop);
    gstd_request stop_request;
    int parsed = gstd_request_parse(stop, &stop_request);
    gstd_accept_status stopped =
        parsed ? GSTD_ACCEPT_INVALID : gstd_accept(db, &stop_request, NULL, NULL, replay);
    json_decref(stop);
    CHECK(!parsed && stopped == GSTD_ACCEPT_NEW);
    CHECK(count(db, "SELECT count(*) FROM operations") == 99873);
    CHECK(!run(
        db, "UPDATE operations SET accepted_at='2000-01-01T00:00:00Z',"
            "updated_at='2000-01-01T00:00:00Z' WHERE operation_id IN "
            "(SELECT operation_id FROM operations WHERE operation_id LIKE 'recent-%' LIMIT 300)"));
    CHECK(gstd_accept(db, &request, components, NULL, replay) == GSTD_ACCEPT_NEW);
    CHECK(count(db, "SELECT count(*) FROM operations") == 99618);
    CHECK(count(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    result = 0;
done:
    json_decref(message);
    json_decref(components);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
