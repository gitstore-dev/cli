#define _GNU_SOURCE 1
#include "deployment_records.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "native_exit.h"
#include "oidc_profile.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC history %d: %s\n", __LINE__, #x);                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#define SQL(x) CHECK(sqlite3_exec(db, x, NULL, NULL, NULL) == SQLITE_OK)
int main(int argc, char **argv) {
    CHECK(argc == 2);
    gstd_json_initialize();
    char *root = gstd_test_dir(), path[4096];
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(path, &db));
    SQL("INSERT INTO profiles VALUES('core','2','controlled','controlled','linux-arm64');"
        "INSERT INTO deployments VALUES('one','core','2','running',2,0,'pending','normal',"
        "'held','2026-11-05T00:00:00Z',NULL)");
    char *sql = sqlite3_mprintf(
        "INSERT INTO operations VALUES('old',%lld,'deployment.start','one',NULL,"
        "printf('%%064d',0),'{}','2026-11-05T00:00:00Z','2026-11-05T00:00:00Z',"
        "'failed','normal',NULL,NULL); INSERT INTO resource_ownership VALUES("
        "'migrate-oidc-hydra-old','workload','one','old','retained',NULL,'released',"
        "'2026-11-05T00:00:00Z')",
        (sqlite3_int64)geteuid());
    CHECK(sql);
    SQL(sql);
    sqlite3_free(sql);
    json_t *components = json_pack("[s]", "oidc");
    json_t *manifest = gstd_oidc_core_manifest(components, "arm64", "memdb");
    CHECK(manifest);
    gstd_deployment_target target = {.generation = 2, .manifest = manifest};
    strcpy(target.operation_id, "new");
    strcpy(target.deployment_id, "one");
    strcpy(target.store_id, "store");
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 3);
    const char *boot = "00000000-0000-0000-0000-000000000001";
    gstd_native_context context = {.component = "oidc-hydra", .boot_id = boot, .state_fd = fd};
    json_t *binding = json_pack("{s:s,s:s}", "role", "oidc-hydra", "volumeId", "hydra-volume");
    json_t *record = NULL;
    int expected = 1;
    if (strcmp(argv[1], "unmaterialized")) {
        record = json_pack("{s:i,s:s,s:s,s:s,s:s,s:i,s:O}", "schemaVersion", 1, "kind", "workload",
                           "storeId", "store", "deploymentId", "one", "operationId", "old",
                           "generation", 1, "executionManifest", manifest);
        CHECK(record);
        if (!strcmp(argv[1], "prepared")) {
            CHECK(!json_object_set_new(record, "execution",
                                       json_pack("{s:s,s:s,s:s,s:s}", "rootfsDevice", "1",
                                                 "rootfsInode", "2", "bundleDevice", "1",
                                                 "bundleInode", "3")));
        }
        if (!strcmp(argv[1], "dispatched")) {
            CHECK(!json_object_set_new(record, "helperControl",
                                       json_pack("{s:s}", "stage", "dispatch")));
            expected = -1;
        } else if (!strcmp(argv[1], "corrupt")) {
            CHECK(!json_object_set_new(record, "operationId", json_string("foreign")));
            expected = -1;
        } else if (!strcmp(argv[1], "previous_boot") || !strcmp(argv[1], "same_boot")) {
            uint64_t start = 1;
#ifdef __linux__
            CHECK(!gstd_native_process_start(getpid(), &start));
#endif
            if (!strcmp(argv[1], "same_boot")) {
#ifndef __linux__
                return 77;
#endif
                expected = -1;
            }
            json_t *native = json_pack(
                "{s:s,s:s,s:s,s:s,s:i,s:i,s:I}", "kind", "libcrun", "id", "migrate-oidc-hydra-old",
                "component", "oidc-hydra", "bootId",
                !strcmp(argv[1], "same_boot") ? boot : "00000000-0000-0000-0000-000000000000",
                "generation", 1, "pid", (int)getpid(), "startTime", (json_int_t)start);
            json_t *termination = json_pack(
                "{s:s,s:O,s:s,s:s,s:i,s:b}", "kind", "helper-native-delete", "native", native,
                "cgroupDevice", "1", "cgroupInode", "2", "exitCode", 0, "signalled", 0);
            json_t *control =
                json_pack("{s:i,s:i,s:s,s:b,s:s,s:i,s:O}", "purpose", GSTD_EXEC_OIDC_MIGRATE,
                          "attempt", 1, "sourceProviderVolume", "hydra-volume", "unsafe", 0,
                          "stage", "deleted", "result", 0, "termination", termination);
            CHECK(native && termination && control);
            CHECK(!json_object_set(record, "native", native));
            CHECK(!json_object_set(record, "helperControl", control));
            CHECK(!json_object_set(record, "oidcMigration", binding));
            CHECK(!json_object_set_new(
                record, "population",
                json_pack("{s:s,s:s}", "cgroupDevice", "1", "cgroupInode", "2")));
            json_decref(native);
            json_decref(termination);
            json_decref(control);
        }
        char *bytes = gstd_json_dump(record);
        sql = sqlite3_mprintf("UPDATE resource_ownership SET backend_identity='%q'", bytes);
        CHECK(sql && bytes);
        SQL(sql);
        sqlite3_free(sql);
        free(bytes);
    }
    CHECK(gstd_deployment_oidc_helper_history(db, &target, binding, &context) == expected);
    if (!strcmp(argv[1], "previous_boot")) {
        /* Neither stale native state nor a mismatched deletion receipt is safe. */
        CHECK(!mkdirat(fd, "migrate-oidc-hydra-old", 0700));
        CHECK(gstd_deployment_oidc_helper_history(db, &target, binding, &context) == -1);
        CHECK(!unlinkat(fd, "migrate-oidc-hydra-old", AT_REMOVEDIR));
        SQL("UPDATE resource_ownership SET backend_identity=json_set(backend_identity,"
            "'$.helperControl.termination.native.bootId','00000000-0000-0000-0000-000000000002')");
        CHECK(gstd_deployment_oidc_helper_history(db, &target, binding, &context) == -1);
    }
    SQL("UPDATE resource_ownership SET ownership_state='uncertain'");
    CHECK(gstd_deployment_oidc_helper_history(db, &target, binding, &context) == -1);
    json_decref(record);
    json_decref(binding);
    json_decref(manifest);
    json_decref(components);
    close(fd);
    sqlite3_close(db);
    gstd_test_remove(root);
    return 0;
}
