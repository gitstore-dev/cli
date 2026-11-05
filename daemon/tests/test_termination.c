#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "retention.h"
#include "support.h"
#include "termination.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "termination line %d: %s\n", __LINE__, #x);                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(x) CHECK(sqlite3_exec(db, x, NULL, NULL, NULL) == SQLITE_OK)
int main(void) {
    gstd_json_initialize();
    int result = 1;
    char *root = gstd_test_dir(), path[512], source[65], stop[65], scheduled[65];
    sqlite3 *db = NULL;
    json_t *start = NULL, *message = NULL, *native = NULL, *receipt = NULL, *value = NULL;
    gstd_deployment_target target = {.generation = 1};
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    SQL("INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64')");
    start =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1, "minor", 0,
                  "requestId", "start", "method", "deployment.start", "idempotencyKey", "start",
                  "params", "deploymentId", "one", "profileId", "core", "profileVersion", "1");
    gstd_request request;
    json_t *components = json_pack("[s]", "git");
    CHECK(start && components && !gstd_request_parse(start, &request));
    gstd_accept_status accepted = gstd_accept(db, &request, components, NULL, source);
    json_decref(components);
    CHECK(accepted == GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, source));
    SQL("UPDATE operations SET state='succeeded',result='{}'");
    CHECK(!gstd_schedule_finish(db, source));
    message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor", 0,
                        "requestId", "stop", "method", "deployment.stop", "idempotencyKey", "stop",
                        "params", "deploymentId", "one");
    CHECK(message && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, NULL, NULL, stop) == GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, stop));
    char *sql = sqlite3_mprintf(
        "UPDATE deployments SET generation=2,last_operation_id=%Q WHERE deployment_id='one'", stop);
    CHECK(sql);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    CHECK(rc == SQLITE_OK);
    memcpy(target.operation_id, source, strlen(source) + 1);
    memcpy(target.deployment_id, "one", 4);
    native =
        json_pack("{s:s,s:s,s:s,s:I,s:I,s:s,s:I}", "kind", "libcrun", "id", "workload", "bootId",
                  "00000000-0000-0000-0000-000000000000", "pid", (json_int_t)123, "startTime",
                  (json_int_t)456, "component", "git", "generation", (json_int_t)1);
    receipt =
        json_pack("{s:s,s:s,s:I,s:I,s:O,s:s,s:s,s:s}", "kind", "libcrun-delete", "operationId",
                  stop, "generation", (json_int_t)1, "desiredGeneration", (json_int_t)2, "native",
                  native, "cgroupDevice", "1", "cgroupInode", "2", "eventsInode", "3");
    value = json_pack("{s:O,s:O}", "native", native, "termination", receipt);
    CHECK(value && !gstd_termination_read(db, &target, "workload", value));
    CHECK(!json_object_set_new(native, "component", json_string("scylladb")));
    CHECK(gstd_termination_read(db, &target, "workload", value) == -1);
    target.manifest = json_pack("{s:s}", "datastore", "scylladb");
    CHECK(target.manifest && !gstd_termination_read(db, &target, "workload", value));
    CHECK(!json_object_set_new(target.manifest, "datastore", json_string("memdb")));
    CHECK(gstd_termination_read(db, &target, "workload", value) == -1);
    json_decref(target.manifest);
    target.manifest = NULL;
    CHECK(!json_object_set_new(native, "component", json_string("git")));
    CHECK(gstd_termination_read(db, &target, "other", value) == -1);
    json_object_set_new(receipt, "desiredGeneration", json_integer(1));
    CHECK(gstd_termination_read(db, &target, "workload", value) == -1);
    json_object_set_new(receipt, "desiredGeneration", json_integer(2));
    json_object_set_new(receipt, "cgroupInode", json_string("0"));
    CHECK(gstd_termination_read(db, &target, "workload", value) == -1);
    json_object_set_new(receipt, "cgroupInode", json_string("2"));
    json_t *wrong = json_deep_copy(native);
    CHECK(wrong);
    json_object_set_new(wrong, "startTime", json_integer(999));
    json_object_set(value, "native", wrong);
    json_decref(wrong);
    CHECK(gstd_termination_read(db, &target, "workload", value) == -1);
    json_object_set(value, "native", native);
    SQL("UPDATE operations SET state='failed',error='{\"code\":\"OWNERSHIP_UNCERTAIN\"}' WHERE "
        "state='running'");
    CHECK(!gstd_schedule_finish(db, stop));
    CHECK(!gstd_termination_read(db, &target, "workload", value));
    /* Synthetic metadata never grants record permission without live held
     * kernel evidence. The native partial-stop scenario proves actual recording. */
    gstd_deployment_environment env = {.database = db};
    gstd_deployment deployment = {.environment = &env, .target = target};
    gstd_deployment_workload workload = {
        .launched = 1, .exited = 1, .death_fd = -1, .population = GSTD_NATIVE_POPULATION_INITIAL};
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_termination_record(&deployment, &workload, stop, 2) == -1);
    SQL("ROLLBACK");
    sql = sqlite3_mprintf(
        "INSERT INTO resource_ownership "
        "VALUES('workload','workload','one',%Q,'retained',NULL,'held','2000-01-01T00:00:00Z');"
        "UPDATE deployments SET last_operation_id=%Q;UPDATE operations SET "
        "accepted_at='2000-01-01T00:00:00Z',updated_at='2000-01-01T00:00:00Z';UPDATE dedupe SET "
        "expires_after=0",
        source, source);
    CHECK(sql);
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    CHECK(rc == SQLITE_OK);
    SQL("BEGIN IMMEDIATE");
    unsigned removed;
    CHECK(!gstd_retention_collect(db, &removed) && removed == 0);
    SQL("COMMIT");
    CHECK(!gstd_termination_read(db, &target, "workload", value));
    SQL("UPDATE resource_ownership SET ownership_state='released';BEGIN IMMEDIATE");
    CHECK(!gstd_retention_collect(db, &removed) && removed == 1);
    SQL("COMMIT");
    CHECK(gstd_termination_read(db, &target, "workload", value) == -1);
    result = 0;
done:
    json_decref(target.manifest);
    json_decref(value);
    json_decref(receipt);
    json_decref(native);
    json_decref(message);
    json_decref(start);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
