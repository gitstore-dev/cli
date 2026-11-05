#define _GNU_SOURCE 1
#include "deployment_records.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "native_recovery.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "native recovery line %d: %s\n", __LINE__, #x);                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    const char *boot = "00000000-0000-0000-0000-000000000000";
    json_t *record = json_object();
    gstd_native_observation observation;
    CHECK(record);
    CHECK(gstd_native_recovery_read(record, "reserved", "workload", "git", boot, 1, &observation) ==
          GSTD_RECOVERY_UNDISPATCHED);
    CHECK(gstd_native_recovery_read(record, "allocated", "workload", "git", boot, 1,
                                    &observation) == -1);
    CHECK(!json_object_set_new(record, "population", json_object()));
    CHECK(gstd_native_recovery_read(record, "reserved", "workload", "git", boot, 1, &observation) ==
          -1);
    CHECK(!json_object_del(record, "population"));
    json_t *native = json_pack("{s:s,s:s,s:s,s:s,s:I,s:I,s:I}", "kind", "libcrun", "id", "workload",
                               "component", "git", "bootId", boot, "generation", (json_int_t)1,
                               "pid", (json_int_t)0, "startTime", (json_int_t)0);
    CHECK(native && !json_object_set(record, "native", native));
    CHECK(gstd_native_recovery_read(record, "allocated", "workload", "git", boot, 1,
                                    &observation) == GSTD_RECOVERY_NATIVE_DISCOVER);
    CHECK(gstd_native_recovery_read(record, "reserved", "workload", "git", boot, 1, &observation) ==
          -1);
    CHECK(gstd_native_recovery_read(record, "running", "workload", "git", boot, 1, &observation) ==
          -1);
    CHECK(gstd_native_recovery_read(record, "allocated", "foreign", "git", boot, 1, &observation) ==
          -1);
    CHECK(gstd_native_recovery_read(record, "allocated", "workload", "api", boot, 1,
                                    &observation) == -1);
    CHECK(gstd_native_recovery_read(record, "allocated", "workload", "git", "foreign-boot", 1,
                                    &observation) == -1);
    CHECK(gstd_native_recovery_read(record, "allocated", "workload", "git", boot, 2,
                                    &observation) == -1);
    json_object_set_new(native, "pid", json_integer(123));
    CHECK(gstd_native_recovery_read(record, "allocated", "workload", "git", boot, 1,
                                    &observation) == -1);
    json_object_set_new(native, "startTime", json_integer(456));
    CHECK(gstd_native_recovery_read(record, "running", "workload", "git", boot, 1, &observation) ==
          GSTD_RECOVERY_NATIVE_KNOWN);
    CHECK(observation.pid == 123 && observation.start_time == 456);
    CHECK(gstd_native_recovery_read(record, "stopping", "workload", "git", boot, 1, &observation) ==
          GSTD_RECOVERY_NATIVE_KNOWN);
    CHECK(gstd_native_recovery_read(record, "retained", "workload", "git", boot, 1, &observation) ==
          GSTD_RECOVERY_NATIVE_KNOWN);
    CHECK(gstd_native_recovery_read(record, "reserved", "workload", "git", boot, 1, &observation) ==
          -1);
    json_object_set_new(native, "pid", json_integer(-1));
    CHECK(gstd_native_recovery_read(record, "allocated", "workload", "git", boot, 1,
                                    &observation) == -1);
    json_object_set_new(native, "pid", json_integer(123));
    json_object_set_new(native, "extra", json_true());
    CHECK(gstd_native_recovery_read(record, "running", "workload", "git", boot, 1, &observation) ==
          -1);
    /* Metadata alone never grants discovered-owner persistence authority. */
    gstd_deployment deployment = {0};
    gstd_deployment_workload workload = {.death_fd = -1,
                                         .population = GSTD_NATIVE_POPULATION_INITIAL};
    CHECK(gstd_native_recovery_record(&deployment, &workload, record) == -1);
    json_decref(native);
    json_decref(record);
    json_t *components = json_pack("[s,s]", "api", "git");
    gstd_deployment_target target = {.generation = 4};
    strcpy(target.store_id, "store");
    strcpy(target.deployment_id, "deployment");
    strcpy(target.operation_id, "operation");
    target.manifest = gstd_asset_manifest_for_datastore(components, "arm64", "scylladb");
    CHECK(target.manifest);
    record =
        json_pack("{s:O,s:{s:i,s:i,s:b,s:i,s:s,s:b,s:b}}", "executionManifest", target.manifest,
                  "graph", "generation", 4, "stage", GSTD_BOOT_COMPLETE, "controller", 0, "outcome",
                  0, "controllerUid", "", "persistent", 1, "freshDatastore", 1);
    gstd_bootstrap_graph graph;
    CHECK(record && !gstd_deployment_graph_recovery_read(&target, record, &graph));
    CHECK(graph.stage == GSTD_BOOT_COMPLETE && graph.persistent && graph.fresh_datastore);
    json_t *saved = json_deep_copy(record), *value = json_object_get(record, "graph");
    CHECK(saved && !json_object_set_new(value, "generation", json_integer(3)));
    CHECK(gstd_deployment_graph_recovery_read(&target, record, &graph) < 0);
    CHECK(!json_object_set_new(value, "generation", json_integer(4)));
    CHECK(!json_object_set_new(value, "stage", json_integer(GSTD_BOOT_STAGE_COUNT)));
    CHECK(gstd_deployment_graph_recovery_read(&target, record, &graph) < 0);
    CHECK(!json_object_set_new(value, "stage", json_integer(GSTD_BOOT_COMPLETE)));
    CHECK(!json_object_set_new(value, "freshDatastore", json_integer(1)));
    CHECK(gstd_deployment_graph_recovery_read(&target, record, &graph) < 0);
    CHECK(!json_object_set_new(value, "freshDatastore", json_true()));
    CHECK(!json_object_set_new(value, "controllerUid", json_string("foreign\nuid")));
    CHECK(gstd_deployment_graph_recovery_read(&target, record, &graph) < 0);
    CHECK(!json_object_set_new(value, "controllerUid", json_string("")));
    CHECK(!gstd_deployment_graph_recovery_read(&target, record, &graph));
    CHECK(!json_object_set_new(record, "executionManifest", json_object()));
    CHECK(gstd_deployment_graph_recovery_read(&target, record, &graph) < 0);
    json_decref(record);
    json_decref(saved);
#ifdef __linux__
    uint64_t birth = 0;
    CHECK(!gstd_native_process_start(getpid(), &birth) && birth > 0);
    char *directory = gstd_test_dir();
    CHECK(directory);
    int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory_fd >= 3);
    native = json_pack("{s:s,s:s,s:s,s:s,s:i,s:i,s:I}", "kind", "libcrun", "id",
                       "run-schema-operation", "component", "api", "bootId", boot, "generation", 4,
                       "pid", (int)getpid(), "startTime", (json_int_t)birth);
    json_t *termination =
        json_pack("{s:s,s:O,s:s,s:s,s:i,s:b}", "kind", "helper-native-delete", "native", native,
                  "cgroupDevice", "1", "cgroupInode", "2", "exitCode", 0, "signalled", 0);
    json_t *control =
        json_pack("{s:i,s:i,s:s,s:b,s:s,s:i,s:O}", "purpose", GSTD_EXEC_SCHEMA_MIGRATE, "attempt",
                  1, "sourceDatastore", "run-scylladb-operation", "unsafe", 0, "stage", "deleted",
                  "result", 0, "termination", termination);
    record = json_pack("{s:O,s:O,s:O,s:s,s:s,s:s,s:i}", "executionManifest", target.manifest,
                       "native", native, "helperControl", control, "storeId", "store",
                       "deploymentId", "deployment", "operationId", "operation", "generation", 4);
    CHECK(record && native && control && termination);
    /* A claimed DELETE cannot hide the original still-live process. */
    CHECK(gstd_deployment_helper_receipt_read(&target, "run-schema-operation", record, boot,
                                              directory_fd) < 0);
    CHECK(!json_object_set_new(native, "startTime", json_integer((json_int_t)birth + 1)));
    CHECK(!gstd_deployment_helper_receipt_read(&target, "run-schema-operation", record, boot,
                                               directory_fd));
    CHECK(!mkdirat(directory_fd, "run-schema-operation", 0700));
    CHECK(gstd_deployment_helper_receipt_read(&target, "run-schema-operation", record, boot,
                                              directory_fd) < 0);
    CHECK(!unlinkat(directory_fd, "run-schema-operation", AT_REMOVEDIR));
    CHECK(!json_object_set_new(control, "unsafe", json_true()));
    CHECK(gstd_deployment_helper_receipt_read(&target, "run-schema-operation", record, boot,
                                              directory_fd) < 0);
    CHECK(!json_object_set_new(control, "unsafe", json_false()));
    CHECK(!json_object_set_new(termination, "cgroupInode", json_string("18446744073709551616")));
    CHECK(gstd_deployment_helper_receipt_read(&target, "run-schema-operation", record, boot,
                                              directory_fd) < 0);
    json_decref(record);
    json_decref(control);
    json_decref(termination);
    json_decref(native);
    close(directory_fd);
    gstd_test_remove(directory);
#endif
    json_decref(target.manifest);
    json_decref(components);
    return 0;
}
