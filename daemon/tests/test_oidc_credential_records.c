#define _GNU_SOURCE 1
#include "deployment_records.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "oidc_profile.h"
#include "support.h"
#include "volume_records.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC credential record line %d\n", __LINE__);                         \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(x) CHECK(sqlite3_exec(db, x, NULL, NULL, NULL) == SQLITE_OK)
static int condition(sqlite3 *db, const char *text) {
    sqlite3_stmt *s = NULL;
    int valid = sqlite3_prepare_v2(db, text, -1, &s, NULL) == SQLITE_OK &&
                sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 1;
    sqlite3_finalize(s);
    return valid;
}
static int operation(sqlite3 *db, gstd_deployment_target *target, const char *operation,
                     int generation) {
    char *sql = sqlite3_mprintf(
        "INSERT INTO operations "
        "VALUES('%q',%lld,'deployment.start','oidc-one',NULL,printf('%%064d',0),"
        "'{\"profileId\":\"core\",\"profileVersion\":\"2\",\"components\":[\"oidc\"]}',"
        "'2026-11-05T00:00:00Z','2026-11-05T00:00:00Z','running','normal',NULL,NULL);"
        "UPDATE deployments SET generation=%d,last_operation_id='%q' WHERE "
        "deployment_id='oidc-one';",
        operation, (sqlite3_int64)geteuid(), generation, operation);
    int ok = sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_free(sql);
    if (ok) {
        snprintf(target->operation_id, sizeof(target->operation_id), "%s", operation);
        target->generation = (uint64_t)generation;
    }
    return ok ? 0 : -1;
}
static int configuration(sqlite3 *db, const gstd_deployment_target *target, int root,
                         const char *id) {
    if (gstd_deployment_resource_reserve(db, target, id, "configuration") ||
        mkdirat(root, id, 0700))
        return -1;
    int wrapper = openat(root, id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (wrapper < 3)
        return -1;
    if (mkdirat(wrapper, "data", 0700)) {
        close(wrapper);
        return -1;
    }
    int data = openat(wrapper, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    int result = -1;
    if (data >= 3 && !fstat(wrapper, &info)) {
        gstd_filesystem_reply receipt = {.status = 0,
                                         .descriptor_count = 2,
                                         .descriptors = {wrapper, data},
                                         .scope_device = (uint64_t)info.st_dev,
                                         .scope_inode = (uint64_t)info.st_ino};
        result = gstd_deployment_scope_record(db, target, id, &receipt);
    }
    if (data >= 3)
        close(data);
    close(wrapper);
    return result;
}
static int prepared_files(int root, const char *id, gstd_filesystem_reply *reply) {
    int wrapper = openat(root, id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int data = wrapper >= 3
                   ? openat(wrapper, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
                   : -1;
    if (wrapper >= 3)
        close(wrapper);
    if (data < 3)
        return -1;
    for (unsigned i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "synthetic-%u", i);
        int output = openat(data, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (output < 3 || write(output, "fixture", 7) != 7) {
            if (output >= 3)
                close(output);
            close(data);
            return -1;
        }
        close(output);
        reply->descriptors[i] = openat(data, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (reply->descriptors[i] < 3) {
            close(data);
            return -1;
        }
        reply->descriptor_count++;
    }
    return data;
}
/* Synthetic ledger policy only: these receipts are not live native, cgroup,
 * writer or namespace authority. Actual kernel/provider checks have separate
 * disposable Linux fixtures. No provider or HTTP mutation executes here. */
static int provider_records(sqlite3 *db, const gstd_deployment_target *target, int root) {
    char volume[65], provider[65];
    const char prefix[] = "run-oidc-hydra-";
    size_t length = strnlen(target->operation_id, sizeof(target->operation_id));
    if (length >= sizeof(provider) - (sizeof(prefix) - 1))
        return -1;
    memcpy(provider, prefix, sizeof(prefix) - 1);
    memcpy(provider + sizeof(prefix) - 1, target->operation_id, length + 1);
    gstd_accept_status accepted =
        gstd_oidc_volume_reserve(db, GSTD_OIDC_VOLUME_HYDRA, target->operation_id, volume);
    if (accepted != GSTD_ACCEPT_NEW && accepted != GSTD_ACCEPT_REPLAY)
        return -1;
    char *sql = sqlite3_mprintf(
        "INSERT INTO oidc_provider_volumes(volume_id,role,owner_uid,producer_operation_id,"
        "store_id,root_device,root_inode,compatibility) "
        "SELECT '%q','oidc-hydra',%lld,'%q','%q','600','604','%q' WHERE NOT EXISTS "
        "(SELECT 1 FROM oidc_provider_volumes WHERE volume_id='%q');"
        "UPDATE oidc_provider_volumes SET allocated=1,volume_device='600',volume_inode='601',"
        "data_device='600',data_inode='602',writer_device='600',writer_inode='603' "
        "WHERE volume_id='%q' AND allocated=0;"
        "UPDATE resource_ownership SET lifecycle_stage='allocated' WHERE resource_id='%q';",
        volume, (sqlite3_int64)geteuid(), target->operation_id, target->store_id,
        gstd_oidc_volume_compatibility(GSTD_OIDC_VOLUME_HYDRA), volume, volume, volume);
    int valid = sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_free(sql);
    if (!valid || gstd_deployment_resource_reserve(db, target, provider, "workload") ||
        mkdirat(root, provider, 0700))
        return -1;
    int wrapper = openat(root, provider, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (wrapper < 3)
        return -1;
    if (mkdirat(wrapper, "data", 0700) || mkdirat(wrapper, "bundle", 0700)) {
        close(wrapper);
        return -1;
    }
    int data = openat(wrapper, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC),
        bundle = openat(wrapper, "bundle", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat info;
    gstd_filesystem_reply reply = {
        .status = 0, .descriptor_count = 3, .descriptors = {wrapper, data, bundle}};
    valid = data >= 3 && bundle >= 3 && !fstat(wrapper, &info);
    if (valid) {
        reply.scope_device = (uint64_t)info.st_dev;
        reply.scope_inode = (uint64_t)info.st_ino;
        valid = !gstd_deployment_scope_record(db, target, provider, &reply);
    }
    gstd_filesystem_reply_close(&reply);
    if (!valid)
        return -1;
    const char *boot = "00000000-0000-4000-8000-000000000000";
    json_t *native = json_pack("{s:s,s:s,s:s,s:s,s:i,s:i,s:I}", "kind", "libcrun", "id", provider,
                               "component", "oidc-hydra", "bootId", boot, "pid", 123, "startTime",
                               456, "generation", (json_int_t)target->generation),
           *population = json_pack("{s:i,s:s,s:s,s:s,s:s,s:s}", "schemaVersion", 1, "bootId", boot,
                                   "scopePath", "/synthetic-cgroup-policy-only", "cgroupDevice",
                                   "25", "cgroupInode", "100", "eventsInode", "101");
    char *a = native ? gstd_json_dump(native) : NULL,
         *b = population ? gstd_json_dump(population) : NULL;
    sql = a && b ? sqlite3_mprintf("UPDATE resource_ownership SET lifecycle_stage='running',"
                                   "backend_identity=json_set(backend_identity,'$.native',json('%q'"
                                   "),'$.population',json('%q')) "
                                   "WHERE resource_id='%q'",
                                   a, b, provider)
                 : NULL;
    valid = sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_free(sql);
    free(a);
    free(b);
    json_decref(native);
    json_decref(population);
    return valid ? 0 : -1;
}
static int enrollment_records(sqlite3 *db, const gstd_deployment_target *target,
                              const char *configuration) {
    int result = 0;
    gstd_oidc_client client = {"fixture-client", "gitstore", "http://127.0.0.1:3000/callback"};
    json_t *prepared = NULL, *dispatched = NULL, *reply = NULL;
    CHECK(!gstd_deployment_oidc_enrollment_begin(db, target, configuration, &client, &prepared));
    CHECK(!strcmp(json_string_value(json_object_get(prepared, "state")), "prepared"));
    CHECK(!gstd_deployment_oidc_enrollment_dispatch(db, target, configuration, &client, prepared,
                                                    &dispatched));
    CHECK(gstd_deployment_oidc_enrollment_dispatch(db, target, configuration, &client, prepared,
                                                   &reply) == -1 &&
          !reply);
    SQL("SAVEPOINT recovered_dispatch");
    CHECK(!gstd_deployment_oidc_enrollment_begin(db, target, configuration, &client, &reply));
    CHECK(!strcmp(json_string_value(json_object_get(reply, "state")), "uncertain"));
    json_decref(reply);
    reply = NULL;
    CHECK(gstd_deployment_oidc_enrollment_observe(db, target, configuration, &client, dispatched,
                                                  GSTD_OIDC_CLIENT_CREATED, &reply) == -1 &&
          !reply);
    SQL("ROLLBACK TO recovered_dispatch;RELEASE recovered_dispatch");
    SQL("SAVEPOINT unknown_reply");
    CHECK(!gstd_deployment_oidc_enrollment_observe(db, target, configuration, &client, dispatched,
                                                   GSTD_OIDC_PUBLIC_MATCH, &reply));
    CHECK(!strcmp(json_string_value(json_object_get(reply, "state")), "uncertain"));
    json_decref(reply);
    reply = NULL;
    CHECK(gstd_deployment_oidc_enrollment_observe(db, target, configuration, &client, dispatched,
                                                  GSTD_OIDC_CLIENT_CREATED, &reply) == -1 &&
          !reply);
    SQL("ROLLBACK TO unknown_reply;RELEASE unknown_reply;SAVEPOINT changed_intent");
    SQL("UPDATE deployments SET "
        "desired_state='stopped',generation=generation+1,recovery='uncertain';"
        "UPDATE resource_ownership SET ownership_state='uncertain' WHERE resource_type IN "
        "('configuration','datastore-volume')");
    CHECK(!gstd_deployment_oidc_enrollment_observe(db, target, configuration, &client, dispatched,
                                                   GSTD_OIDC_CLIENT_CREATED, &reply));
    CHECK(!strcmp(json_string_value(json_object_get(reply, "state")), "created"));
    CHECK(condition(db, "SELECT desired_state='stopped' AND recovery='uncertain' AND generation=2 "
                        "FROM deployments WHERE deployment_id='oidc-one'"));
    CHECK(condition(db, "SELECT ownership_state='uncertain' FROM resource_ownership "
                        "WHERE resource_id='oidc-cfg-first'"));
    json_decref(reply);
    reply = NULL;
    SQL("ROLLBACK TO changed_intent;RELEASE changed_intent");
    CHECK(!gstd_deployment_oidc_enrollment_observe(db, target, configuration, &client, dispatched,
                                                   GSTD_OIDC_CLIENT_CREATED, &reply));
    json_decref(reply);
    reply = NULL;
    CHECK(!gstd_deployment_oidc_enrollment_observe(db, target, configuration, &client, dispatched,
                                                   GSTD_OIDC_CLIENT_CREATED, &reply));
    CHECK(!strcmp(json_string_value(json_object_get(reply, "state")), "created"));
done:
    json_decref(prepared);
    json_decref(dispatched);
    json_decref(reply);
    return result;
}
int main(void) {
    int result = 0, root = -1, prepared_directory = -1;
    gstd_filesystem_reply prepared = {.descriptors = {-1, -1, -1, -1, -1, -1, -1, -1}};
    gstd_oidc_ports ports = {24001, 24002, 24003, 24004, 24005};
    json_t *prior_scope = NULL, *retained = NULL, *duplicate = NULL;
    char prior_id[65];
    sqlite3 *db = NULL;
    char *directory = gstd_test_dir(), path[512];
    gstd_json_initialize();
    gstd_deployment_target target = {0};
    json_t *selection = json_pack("[s]", "oidc");
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    target.manifest = selection ? gstd_oidc_core_manifest(selection, architecture, "memdb") : NULL;
    target.components = selection;
    CHECK(directory && target.manifest);
    strcpy(target.deployment_id, "oidc-one");
    snprintf(path, sizeof(path), "%s/state.db", directory);
    CHECK(!gstd_state_open(path, &db));
    root = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(root >= 3);
    char *sql = sqlite3_mprintf(
        "INSERT INTO profiles "
        "VALUES('core','2','06152d193e854bc643fa884daa8b9f30b0d75d0f','%q','linux-%q');"
        "INSERT INTO deployments "
        "VALUES('oidc-one','core','2','running',1,0,'pending','normal','held','2026-11-05T00:00:"
        "00Z',NULL);",
        json_string_value(json_object_get(target.manifest, "profileDigest")), architecture);
    CHECK(sql);
    int inserted = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    CHECK(inserted == SQLITE_OK);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &s, NULL) == SQLITE_OK &&
          sqlite3_step(s) == SQLITE_ROW);
    snprintf(target.store_id, sizeof(target.store_id), "%s", sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    CHECK(!operation(db, &target, "first", 1));
    char reference[65], original[65];
    int initial = 99;
    CHECK(gstd_deployment_oidc_credential_reserve(db, &target, "oidc-cfg-first", reference,
                                                  &initial) == -1 &&
          initial == 0 && !reference[0]);
    SQL("BEGIN IMMEDIATE");
    CHECK(!configuration(db, &target, root, "oidc-cfg-first"));
    prepared_directory = prepared_files(root, "oidc-cfg-first", &prepared);
    CHECK(prepared_directory >= 3);
    CHECK(gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", prepared_directory,
                                               &ports, &prepared) == -1);
    CHECK(!gstd_deployment_oidc_credential_reserve(db, &target, "oidc-cfg-first", reference,
                                                   &initial) &&
          initial == 1);
    strcpy(original, reference);
    CHECK(gstd_deployment_oidc_preparation_source(db, &target, "oidc-cfg-first", prior_id,
                                                  &prior_scope) == 1 &&
          !*prior_id && !prior_scope);
    CHECK(gstd_deployment_oidc_preparation_source(db, &target, "oidc-cfg-first", prior_id,
                                                  &prior_scope) == 1 &&
          !*prior_id && !prior_scope);
    CHECK(!gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", prepared_directory,
                                                &ports, &prepared));
    CHECK(!provider_records(db, &target, root));
    CHECK(!enrollment_records(db, &target, "oidc-cfg-first"));
    CHECK(!gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", prepared_directory,
                                                &ports, &prepared));
    ports.issuer++;
    CHECK(gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", prepared_directory,
                                               &ports, &prepared) == -1);
    ports.issuer--;
    int saved = prepared.descriptors[0];
    prepared.descriptors[0] = prepared.descriptors[1];
    CHECK(gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", prepared_directory,
                                               &ports, &prepared) == -1);
    prepared.descriptors[0] = saved;
    CHECK(!linkat(prepared_directory, "synthetic-0", prepared_directory, "alias", 0));
    CHECK(gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", prepared_directory,
                                               &ports, &prepared) == -1);
    CHECK(!unlinkat(prepared_directory, "alias", 0));
    CHECK(gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", root, &ports,
                                               &prepared) == -1);
    CHECK(!gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", prepared_directory,
                                                &ports, &prepared));
    CHECK(!gstd_deployment_oidc_credential_reserve(db, &target, "oidc-cfg-first", reference,
                                                   &initial) &&
          initial == 0 && !strcmp(original, reference));
    SQL("COMMIT");
    CHECK(gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-first", prepared_directory,
                                               &ports, &prepared) == -1);
    SQL("BEGIN IMMEDIATE");
    target.generation++;
    CHECK(gstd_deployment_oidc_credential_reserve(db, &target, "oidc-cfg-first", reference,
                                                  &initial) == -1 &&
          initial == 0);
    target.generation--;
    SQL("UPDATE deployments SET recovery='uncertain'");
    CHECK(gstd_deployment_oidc_credential_reserve(db, &target, "oidc-cfg-first", reference,
                                                  &initial) == -1);
    SQL("ROLLBACK");
    SQL("BEGIN IMMEDIATE;UPDATE operations SET state='succeeded',result='{}';UPDATE "
        "resource_ownership SET ownership_state='released',lifecycle_stage='retained'");
    CHECK(!operation(db, &target, "second", 2) &&
          !configuration(db, &target, root, "oidc-cfg-second"));
    CHECK(!gstd_deployment_oidc_credential_reserve(db, &target, "oidc-cfg-second", reference,
                                                   &initial) &&
          initial == 0 && !strcmp(original, reference));
    CHECK(!gstd_deployment_oidc_preparation_source(db, &target, "oidc-cfg-second", prior_id,
                                                   &prior_scope) &&
          !strcmp(prior_id, "oidc-cfg-first") && prior_scope &&
          json_integer_value(json_object_get(prior_scope, "generation")) == 1);
    json_decref(prior_scope);
    prior_scope = NULL;
    gstd_filesystem_reply second_prepared = {.descriptors = {-1, -1, -1, -1, -1, -1, -1, -1}};
    int second_directory = prepared_files(root, "oidc-cfg-second", &second_prepared);
    CHECK(second_directory >= 3 &&
          !gstd_deployment_oidc_prepared_record(db, &target, "oidc-cfg-second", second_directory,
                                                &ports, &second_prepared));
    close(second_directory);
    gstd_filesystem_reply_close(&second_prepared);
    CHECK(!provider_records(db, &target, root));
    gstd_oidc_client retained_client = {"fixture-client", "gitstore",
                                        "http://127.0.0.1:3000/callback"};
    SQL("SAVEPOINT unknown_retained;UPDATE resource_ownership SET "
        "backend_identity=json_set(backend_identity,'$.oidcEnrollment.state','uncertain') "
        "WHERE resource_id='oidc-cfg-first'");
    CHECK(gstd_deployment_oidc_enrollment_begin(db, &target, "oidc-cfg-second", &retained_client,
                                                &retained) == -1 &&
          !retained);
    SQL("ROLLBACK TO unknown_retained;RELEASE unknown_retained");
    CHECK(!gstd_deployment_oidc_enrollment_begin(db, &target, "oidc-cfg-second", &retained_client,
                                                 &retained));
    CHECK(!strcmp(json_string_value(json_object_get(retained, "state")), "created"));
    CHECK(gstd_deployment_oidc_enrollment_dispatch(db, &target, "oidc-cfg-second", &retained_client,
                                                   retained, &duplicate) == -1 &&
          !duplicate);
    json_decref(retained);
    retained = NULL;
    CHECK(!gstd_deployment_oidc_preparation_source(db, &target, "oidc-cfg-second", prior_id,
                                                   &prior_scope));
    json_decref(prior_scope);
    prior_scope = NULL;
    SQL("COMMIT");
    CHECK(gstd_deployment_oidc_preparation_source(db, &target, "oidc-cfg-second", prior_id,
                                                  &prior_scope) == -1 &&
          !*prior_id && !prior_scope);
    SQL("BEGIN IMMEDIATE;UPDATE resource_ownership SET "
        "backend_identity=json_set(backend_identity,'$.oidcPrepared',NULL) WHERE "
        "resource_id='oidc-cfg-first'");
    CHECK(gstd_deployment_oidc_preparation_source(db, &target, "oidc-cfg-second", prior_id,
                                                  &prior_scope) == -1 &&
          !*prior_id && !prior_scope);
    SQL("ROLLBACK;BEGIN IMMEDIATE;UPDATE resource_ownership SET "
        "backend_identity=json_set(backend_identity,'$.oidcPrepared.files[0].inode','123456789') "
        "WHERE resource_id='oidc-cfg-first'");
    CHECK(gstd_deployment_oidc_preparation_source(db, &target, "oidc-cfg-second", prior_id,
                                                  &prior_scope) == -1 &&
          !*prior_id && !prior_scope);
    SQL("ROLLBACK");
    SQL("BEGIN IMMEDIATE;UPDATE operations SET state='succeeded',result='{}' WHERE "
        "state='running';UPDATE resource_ownership SET "
        "ownership_state='released',lifecycle_stage='retained'");
    CHECK(!operation(db, &target, "third", 3) &&
          !configuration(db, &target, root, "oidc-cfg-third"));
    SQL("UPDATE resource_ownership SET "
        "backend_identity=json_set(backend_identity,'$.oidcCredentialReservation',false) WHERE "
        "resource_id='oidc-cfg-first'");
    CHECK(gstd_deployment_oidc_credential_reserve(db, &target, "oidc-cfg-third", reference,
                                                  &initial) == -1 &&
          initial == 0 && !reference[0]);
    SQL("ROLLBACK");
    SQL("BEGIN IMMEDIATE");
    CHECK(!operation(db, &target, "fourth", 4) &&
          !configuration(db, &target, root, "oidc-cfg-fourth"));
    SQL("UPDATE resource_ownership SET "
        "backend_identity=json_set(backend_identity,'$.oidcCredentialRef','foreign') WHERE "
        "resource_id='oidc-cfg-fourth'");
    CHECK(gstd_deployment_oidc_credential_reserve(db, &target, "oidc-cfg-fourth", reference,
                                                  &initial) == -1 &&
          initial == 0);
    SQL("ROLLBACK");
done:
    json_decref(retained);
    json_decref(duplicate);
    json_decref(prior_scope);
    if (prepared_directory >= 3)
        close(prepared_directory);
    gstd_filesystem_reply_close(&prepared);
    gstd_deployment_target_clear(&target);
    if (root >= 3)
        close(root);
    if (db)
        sqlite3_close(db);
    if (directory)
        gstd_test_remove(directory);
    return result;
}
