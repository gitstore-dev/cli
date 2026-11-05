#define _GNU_SOURCE 1
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "oidc_profile.h"
#include "oidc_setup.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC setup line %d\n", __LINE__);                                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int directory(int root, const char *name) {
    if (mkdirat(root, name, 0700))
        return -1;
    return openat(root, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}
static int condition(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    int valid = sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK &&
                sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 1;
    sqlite3_finalize(s);
    return valid;
}
static int operation(sqlite3 *db, gstd_deployment_target *target, const char *id, int generation) {
    char *sql = sqlite3_mprintf(
        "BEGIN IMMEDIATE;"
        "UPDATE operations SET state='succeeded',result='{}' WHERE state='running';"
        "UPDATE resource_ownership SET ownership_state='released',lifecycle_stage='retained';"
        "INSERT INTO operations VALUES('%q',%lld,'deployment.start','oidc-setup-fixture',NULL,"
        "printf('%%064d',0),'{\"profileId\":\"core\",\"profileVersion\":\"2\",\"components\":["
        "\"oidc\"]}',"
        "'2026-11-05T00:00:00Z','2026-11-05T00:00:00Z','running','normal',NULL,NULL);"
        "UPDATE deployments SET generation=%d,last_operation_id='%q' WHERE "
        "deployment_id='oidc-setup-fixture';"
        "COMMIT",
        id, (sqlite3_int64)geteuid(), generation, id);
    int valid = sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_free(sql);
    if (valid) {
        snprintf(target->operation_id, sizeof(target->operation_id), "%s", id);
        target->generation = (uint64_t)generation;
    }
    return valid ? 0 : -1;
}
static int finish(gstd_oidc_setup *setup, int cancel, int success) {
    int complete = 0;
    for (unsigned i = 0; i < 30000 && !(complete = gstd_oidc_setup_poll(setup, cancel)); i++)
        usleep(1000);
    if (complete != 1 || (success ? setup->outcome != 0 : setup->outcome >= 0)) {
        fprintf(stderr, "OIDC setup complete=%d outcome=%d detail=%d\n", complete, setup->outcome,
                setup->detail);
        return -1;
    }
    if (success) {
        for (unsigned i = 0; i < 4; i++) {
            struct stat info;
            int flags = fcntl(setup->files[i], F_GETFL);
            if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(setup->files[i], &info) ||
                !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
                (info.st_mode & 07777) != 0600 || info.st_nlink != 1 || info.st_size <= 0 ||
                info.st_size > 32768)
                return -1;
        }
        for (unsigned i = 0; i < 2; i++)
            if (setup->volumes[i].data_fd < 3 || setup->volumes[i].writer_fd < 3 ||
                !setup->volumes[i].identity.data_inode)
                return -1;
    }
    return setup->worker.worker ? -1 : 0;
}
int main(int argc, char **argv) {
    CHECK(argc == 3 && argv[1][0] == '/' && argv[2][0] == '/' && geteuid());
    alarm(90);
    gstd_json_initialize();
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
        broker = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    CHECK(root >= 3 && broker >= 3 && !fstat(root, &st) && st.st_uid == geteuid() &&
          (st.st_mode & 07777) == 0700);
    int resources = directory(root, "resources"), volumes = directory(root, "volumes"),
        credentials = directory(root, "credentials");
    CHECK(resources >= 3 && volumes >= 3 && credentials >= 3);
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/state.db", argv[1]);
    CHECK(n > 0 && n < (int)sizeof(path));
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(path, &db));
    gstd_deployment_target target = {0};
    strcpy(target.deployment_id, "oidc-setup-fixture");
    target.components = json_pack("[s]", "oidc");
#if defined(__aarch64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    target.manifest = gstd_oidc_core_manifest(target.components, architecture, "memdb");
    CHECK(target.manifest);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &s, NULL) == SQLITE_OK &&
          sqlite3_step(s) == SQLITE_ROW);
    snprintf(target.store_id, sizeof(target.store_id), "%s", sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    char *sql = sqlite3_mprintf(
        "INSERT INTO profiles "
        "VALUES('core','2','06152d193e854bc643fa884daa8b9f30b0d75d0f','%q','linux-%q');"
        "INSERT INTO deployments "
        "VALUES('oidc-setup-fixture','core','2','running',1,0,'pending','normal','held',"
        "'2026-11-05T00:00:00Z',NULL)",
        json_string_value(json_object_get(target.manifest, "profileDigest")), architecture);
    CHECK(sql);
    int registered = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    CHECK(registered == SQLITE_OK);
    gstd_runtime_settings settings = GSTD_RUNTIME_SETTINGS_INITIAL;
    n = snprintf(path, sizeof(path), "%s/credentials", argv[1]);
    CHECK(n > 0 && n < (int)sizeof(path));
    settings.oidc_credential_source =
        json_pack("{s:s,s:s}", "kind", "protected-directory", "path", path);
    settings.oidc_credential_parent_fd = fcntl(credentials, F_DUPFD_CLOEXEC, 3);
    CHECK(settings.oidc_credential_source && settings.oidc_credential_parent_fd >= 3);
    gstd_volume_root owner = {.root_fd = -1, .lock_fd = -1};
    CHECK(gstd_volume_root_lock(volumes, target.store_id, &owner) == GSTD_VOLUME_OK);
    gstd_deployment_environment environment = {.database = db,
                                               .settings = &settings,
                                               .state_fd = root,
                                               .cache_fd = -1,
                                               .resource_fd = resources,
                                               .native_fd = -1,
                                               .broker_fd = broker,
                                               .volume_root = owner};
    gstd_oidc_ports ports = {24001, 24002, 24003, 24004, 24005};
    gstd_oidc_setup setup = {0};
    CHECK(!operation(db, &target, "first", 1));
    gstd_deployment_environment invalid = environment;
    invalid.broker_fd = -1;
    CHECK(gstd_oidc_setup_begin(&invalid, &target, &ports, &setup) == -1 && !setup.environment);
    CHECK(condition(db, "SELECT count(*)=0 FROM resource_ownership"));
    CHECK(!gstd_oidc_setup_begin(&environment, &target, &ports, &setup) && !finish(&setup, -1, 1));
    CHECK(setup.initial == 1 && setup.fresh[0] && setup.fresh[1] && !setup.previous_identity);
    CHECK(condition(db, "SELECT count(*)=2 FROM oidc_provider_volumes WHERE allocated=1"));
    CHECK(
        condition(db, "SELECT count(*)=0 FROM resource_ownership WHERE resource_type='workload'"));
    CHECK(condition(db, "SELECT readiness='pending' AND observed_generation=0 FROM deployments "
                        "WHERE deployment_id='oidc-setup-fixture'"));
    gstd_volume_identity original[2] = {setup.volumes[0].identity, setup.volumes[1].identity};
    char ids[2][65];
    strcpy(ids[0], setup.volume_ids[0]);
    strcpy(ids[1], setup.volume_ids[1]);
    CHECK(!gstd_oidc_setup_close(&setup));
    CHECK(!operation(db, &target, "second", 2));
    CHECK(!gstd_oidc_setup_begin(&environment, &target, &ports, &setup) && !finish(&setup, -1, 1));
    CHECK(!setup.initial && !setup.fresh[0] && !setup.fresh[1] && setup.previous_identity &&
          !strcmp(setup.previous_id, "oidc-cfg-first"));
    for (unsigned i = 0; i < 2; i++)
        CHECK(!strcmp(setup.producers[i], "first") && !strcmp(ids[i], setup.volume_ids[i]) &&
              !memcmp(&original[i], &setup.volumes[i].identity, sizeof(original[i])));
    CHECK(!gstd_oidc_setup_close(&setup));
    CHECK(!operation(db, &target, "third", 3));
    int cancel[2];
    CHECK(!pipe2(cancel, O_CLOEXEC) && write(cancel[1], "x", 1) == 1);
    CHECK(!gstd_oidc_setup_begin(&environment, &target, &ports, &setup) &&
          !finish(&setup, cancel[0], 0));
    char byte;
    CHECK(read(cancel[0], &byte, 1) == 1 && byte == 'x');
    close(cancel[0]);
    close(cancel[1]);
    CHECK(condition(db, "SELECT backend_identity IS NULL AND ownership_state='held' FROM "
                        "resource_ownership WHERE resource_id='oidc-cfg-third'"));
    CHECK(!gstd_oidc_setup_close(&setup));
    gstd_volume_root_close(&owner);
    gstd_runtime_settings_clear(&settings);
    gstd_deployment_target_clear(&target);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    close(credentials);
    close(resources);
    close(volumes);
    close(broker);
    close(root);
    puts("{\"preparedGenerations\":2,\"privateConfigDescriptors\":4,\"providerVolumes\":2,"
         "\"retainedProducerAndInodes\":true,"
         "\"previousPreparationReopenedReadOnly\":true,\"cancellationRetainsOwners\":true,"
         "\"cancelMarkerUnconsumed\":true,"
         "\"secretsReadByControlOwner\":false,\"serviceReadinessClaimed\":false}");
    return 0;
}
