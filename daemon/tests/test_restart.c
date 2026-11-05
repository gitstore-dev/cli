#include "gitstored/operations.h"
#include "gitstored/state.h"
#include "restart.h"
#include "support.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "case %d line %d: %s\n", fault, __LINE__, #x);                         \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
typedef struct {
    sqlite3 *db;
    unsigned calls;
    int no_provision;
} fixture;
static int submit(void *context, gstd_method method, json_t *params, const char *key, char id[65]) {
    fixture *f = context;
    f->calls++;
    if (method == GSTD_START && !json_is_false(json_object_get(params, "allowProvision")))
        f->no_provision = 0;
    gstd_request r = {
        .method = method, .params = params, .idempotency_key = key, .request_id = "restart-test"};
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller");
    gstd_accept_status status =
        gstd_accept(f->db, &r, components, method == GSTD_START ? "repo-volume" : NULL, id);
    json_decref(components);
    return status == GSTD_ACCEPT_NEW || status == GSTD_ACCEPT_REPLAY ? 0 : -1;
}
static int sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static int complete(sqlite3 *db, const char *id, int failed) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db, "UPDATE operations SET state='running' WHERE operation_id=? AND state='accepted'",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!ok || sqlite3_prepare_v2(db,
                                  failed ? "UPDATE operations SET state='failed',error='{}' WHERE "
                                           "operation_id=? AND state='running'"
                                         : "UPDATE operations SET state='succeeded',result='{}' "
                                           "WHERE operation_id=? AND state='running'",
                                  -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok ? 0 : -1;
}
static int stopped(sqlite3 *db, const char *id, int release) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
                           "UPDATE deployments SET "
                           "generation=generation+1,desired_state='stopped',readiness='pending',"
                           "last_operation_id=?",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!ok ||
        sql(db,
            "UPDATE deployments SET observed_generation=generation,writer_ownership='released'"))
        return -1;
    return release ? sql(db, "UPDATE resource_ownership SET ownership_state='released'") : 0;
}
static int scenario(int fault) {
    int result = 0;
    char *root = gstd_test_dir(), path[256], initial[65];
    sqlite3 *db = NULL;
    json_t *params = NULL;
    gstd_restart r = {0};
    CHECK(root && snprintf(path, sizeof(path), "%s/restart.db", root) > 0);
    CHECK(!gstd_state_open(path, &db));
    CHECK(!sql(db, "INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64')"));
    fixture f = {.db = db, .no_provision = 1};
    params = json_pack("{s:s,s:s,s:s,s:b}", "deploymentId", "one", "profileId", "core",
                       "profileVersion", "1", "allowProvision", 1);
    CHECK(params && !submit(&f, GSTD_START, params, "initial", initial));
    CHECK(!complete(db, initial, 0));
    CHECK(!sql(db, "UPDATE jobs SET state='done',worker_slot=NULL; UPDATE deployments SET "
                   "observed_generation=generation,readiness='failed'"));
    f.calls = 0;
    f.no_provision = 1;
    if (fault == 5) {
        CHECK(!sql(db, "UPDATE deployments SET recovery='uncertain'"));
        CHECK(gstd_restart_departed(db, initial, &r, submit, &f) == -1 && !f.calls);
        goto done;
    }
    char current[65];
    memcpy(current, initial, sizeof(current));
    int64_t tick = 1000;
    for (unsigned attempt = 1; attempt <= 4; attempt++) {
        CHECK(!gstd_restart_departed(db, current, &r, submit, &f));
        CHECK(r.phase == 2 && r.attempts == attempt && f.calls == 2 * attempt - 1);
        gstd_restart_poll(db, &r, tick, submit,
                          &f); /* Accepted stop still retains source generation. */
        CHECK(r.phase == 2 && f.calls == 2 * attempt - 1);
        if (fault == 1) {
            char statement[512];
            snprintf(
                statement, sizeof(statement),
                "INSERT INTO resource_ownership "
                "VALUES('held','network','one','%s','running',NULL,'held','2026-11-05T00:00:00Z')",
                current);
            CHECK(!sql(db, statement));
        }
        CHECK(!stopped(db, r.stop, fault != 1));
        CHECK(!complete(db, r.stop, fault == 3));
        if (fault == 2)
            CHECK(!sql(db, "UPDATE deployments SET last_operation_id=NULL"));
        gstd_restart_poll(db, &r, tick, submit, &f);
        if (fault >= 1 && fault <= 3) {
            CHECK(r.phase == 5 &&
                  f.calls == 1); /* Held owner, superseding intent, or failed stop. */
            goto done;
        }
        CHECK(r.phase == 3 && r.due_ms > tick);
        gstd_restart_poll(db, &r, r.due_ms - 1, submit, &f);
        CHECK(r.phase == 3 && f.calls == 2 * attempt - 1);
        tick = r.due_ms;
        gstd_restart_poll(db, &r, tick, submit, &f);
        CHECK(r.phase == 4 && f.calls == 2 * attempt && f.no_provision);
        gstd_restart_poll(db, &r, tick, submit, &f);
        CHECK(r.phase == 4); /* Acceptance alone is not readiness. */
        CHECK(!complete(db, r.current, fault == 4));
        CHECK(!sql(db, "UPDATE jobs SET state='done',worker_slot=NULL; UPDATE deployments SET "
                       "observed_generation=generation,readiness='ready'"));
        gstd_restart_poll(db, &r, tick, submit, &f);
        if (fault == 4) {
            CHECK(r.phase == 5 && f.calls == 2);
            goto done;
        }
        CHECK(r.phase == 1);
        memcpy(current, r.current, sizeof(current));
    }
    CHECK(!gstd_restart_departed(db, current, &r, submit, &f));
    CHECK(r.phase == 5 && r.attempts == 4 && r.delayed_ms == 3750 && f.calls == 8);
    /* The successful initial outcome remains immutable throughout the chain. */
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT state,result,error FROM operations WHERE operation_id=?",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, initial, -1, SQLITE_TRANSIENT);
    int preserved = sqlite3_step(s) == SQLITE_ROW &&
                    !strcmp((const char *)sqlite3_column_text(s, 0), "succeeded") &&
                    !strcmp((const char *)sqlite3_column_text(s, 1), "{}") &&
                    sqlite3_column_type(s, 2) == SQLITE_NULL;
    sqlite3_finalize(s);
    CHECK(preserved);
done:
    gstd_restart_clear(&r);
    json_decref(params);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
int main(void) {
    gstd_json_initialize();
    for (int fault = 0; fault <= 5; fault++)
        if (scenario(fault))
            return 1;
    return 0;
}
