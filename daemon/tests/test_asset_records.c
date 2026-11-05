#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/asset_cache.h"
#include "gitstored/asset_records.h"
#include "gitstored/asset_tree.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(s) CHECK(sqlite3_exec(db, s, NULL, NULL, NULL) == SQLITE_OK)
int main(void) {
    int result = 0, rootfd = -1, stage = -1, file = -1;
    sqlite3 *db = NULL;
    char *root = gstd_test_dir(), path[1024], operation[65], key[65], name[138], again_key[65],
         again_name[138];
    json_t *components = NULL, *manifest = NULL, *message = NULL, *persistent = NULL;
    gstd_cache_owner owner = {.lock_fd = -1}, expected;
    gstd_json_initialize();
    CHECK(root);
    CHECK(snprintf(path, sizeof(path), "%s/state.db", root) > 0 && !gstd_state_open(path, &db));
    char cache_path[1024];
    CHECK(snprintf(cache_path, sizeof(cache_path), "%s/cache", root) > 0);
    CHECK(!mkdir(cache_path, 0700));
    rootfd = open(cache_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(rootfd >= 0 && gstd_asset_record_cache_state(db, &expected) == 1);
    CHECK(gstd_asset_cache_lock(rootfd, expected.store_id, 0, 0, 1, &owner) == GSTD_CACHE_OK);
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_cache_bind(db, &owner));
    SQL("COMMIT");
    CHECK(!gstd_asset_record_cache_state(db, &expected));
    CHECK(expected.device == owner.device && expected.inode == owner.inode);
    CHECK(sqlite3_exec(db, "UPDATE asset_cache_binding SET inode='1'", NULL, NULL, NULL) !=
          SQLITE_OK);
    CHECK(sqlite3_exec(db, "DELETE FROM asset_cache_binding", NULL, NULL, NULL) != SQLITE_OK);
    const char start[] =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"asset-start\",\"params\":{\"deploymentId\":\"one\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\",\"components\":[\"git\"],"
        "\"allowProvision\":true}}";
    components = json_pack("[s,s,s]", "api", "git", "controller");
    CHECK(components);
    manifest = gstd_asset_manifest(components, "arm64");
    CHECK(manifest);
    sqlite3_stmt *profile = NULL;
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,'linux-arm64')", -1,
                             &profile, NULL) == SQLITE_OK);
    sqlite3_bind_text(profile, 1, json_string_value(json_object_get(manifest, "serverRevision")),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(profile, 2, json_string_value(json_object_get(manifest, "profileDigest")), -1,
                      SQLITE_TRANSIENT);
    int profile_result = sqlite3_step(profile);
    sqlite3_finalize(profile);
    CHECK(profile_result == SQLITE_DONE);
    message = gstd_json_parse((const unsigned char *)start, sizeof(start) - 1);
    CHECK(message);
    gstd_request request;
    CHECK(!gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, "repository-one", operation) == GSTD_ACCEPT_NEW);
    CHECK(sqlite3_exec(db, "UPDATE profiles SET server_revision='changed-source'", NULL, NULL,
                       NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(db, "UPDATE profiles SET manifest_digest='changed-plan'", NULL, NULL,
                       NULL) != SQLITE_OK);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db,
                             "INSERT INTO resource_ownership "
                             "VALUES('asset-api','asset-root','one',?,'reserved',NULL,'held','2026-"
                             "11-03T00:00:00Z')",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    int inserted = sqlite3_step(s);
    sqlite3_finalize(s);
    CHECK(inserted == SQLITE_DONE);
    CHECK(gstd_asset_record_begin(db, manifest, "api", operation, "asset-api", 1024, 1024,
                                  UINT64_C(4) * 1024 * 1024, key, name) == GSTD_ASSET_RECORD_ERROR);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, manifest, "controller", operation, "asset-api", 1024, 1024,
                                  UINT64_C(4) * 1024 * 1024, key, name) == GSTD_ASSET_RECORD_ERROR);
    SQL("ROLLBACK");
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, manifest, "api", operation, "asset-api", 1024, 1024, 1024,
                                  key, name) == GSTD_ASSET_RECORD_LIMIT);
    SQL("ROLLBACK");
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, manifest, "api", operation, "asset-api", 1024, 1024,
                                  UINT64_C(4) * 1024 * 1024, key, name) == GSTD_ASSET_RECORD_NEW);
    SQL("COMMIT");
    uint64_t count = 0;
    CHECK(!gstd_asset_record_refcount(db, key, &count) && count == 1);
    char candidate[65];
    CHECK(gstd_asset_record_evict_candidate(db, candidate) == 1 && !*candidate);
    CHECK(!gstd_asset_record_source(db, key, operation, -1));
    CHECK(gstd_asset_record_source(db, key, operation, rootfd) < 0);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, manifest, "api", operation, "asset-api", 1024, 1024,
                                  UINT64_C(4) * 1024 * 1024, again_key,
                                  again_name) == GSTD_ASSET_RECORD_HELD);
    SQL("COMMIT");
    CHECK(!strcmp(key, again_key) && !*again_name);
    CHECK(rootfd >= 0 && !mkdirat(rootfd, name, 0700));
    stage = openat(rootfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(stage >= 0);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_identity(db, key, operation, rootfd) < 0);
    CHECK(!gstd_asset_record_identity(db, key, operation, stage));
    SQL("COMMIT");
    file = openat(stage, "controlled-payload", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(file >= 0 && write(file, "verified", 8) == 8);
    struct stat st;
    CHECK(!fstat(stage, &st));
    /* Controlled parent-metadata fixture, not an upstream authenticity claim. */
    gstd_provision_result proof = {.staging_device = (uint64_t)st.st_dev,
                                   .staging_inode = (uint64_t)st.st_ino,
                                   .downloaded_bytes = 16,
                                   .merged_bytes = 1024};
    CHECK(!gstd_asset_tree_digest(stage, 100, 1, proof.tree_digest, &proof.logical_bytes));
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_verified(db, key, operation, &proof));
    SQL("COMMIT");
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    CHECK(!gstd_asset_record_refcount(db, key, &count) && count == 1);
#ifdef __linux__
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_active(db, key, operation, rootfd) < 0);
    SQL("ROLLBACK");
    CHECK(gstd_asset_promote(rootfd, name, key, &proof, -1) == GSTD_CACHE_OK);
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_active(db, key, operation, rootfd));
    SQL("COMMIT");
    gstd_provision_result loaded = {0};
    CHECK(!gstd_asset_record_receipt(db, key, operation, "asset-api", &loaded));
    CHECK(loaded.staging_inode == proof.staging_inode &&
          loaded.logical_bytes == proof.logical_bytes &&
          !strcmp(loaded.tree_digest, proof.tree_digest));
    CHECK(gstd_asset_record_receipt(db, key, "foreign-operation", "asset-api", &loaded) < 0);
    CHECK(gstd_asset_record_receipt(db, key, operation, "foreign-resource", &loaded) < 0);
#endif
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_unpin(db, "asset-api") < 0);
    gstd_provision_result eviction = {0};
    CHECK(gstd_asset_record_evict_begin(db, key, &eviction) < 0);
    SQL("ROLLBACK");
    CHECK(sqlite3_exec(db, "DELETE FROM asset_refs", NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(db, "DELETE FROM assets", NULL, NULL, NULL) != SQLITE_OK);
    SQL("BEGIN IMMEDIATE;UPDATE resource_ownership SET "
        "ownership_state='released',lifecycle_stage='released' WHERE resource_id='asset-api'");
    CHECK(!gstd_asset_record_unpin(db, "asset-api"));
    SQL("COMMIT");
    CHECK(!gstd_asset_record_refcount(db, key, &count) && !count);
    CHECK(gstd_asset_record_receipt(db, key, operation, "asset-api", &eviction) < 0);
#ifdef __linux__
    CHECK(!gstd_asset_record_evict_candidate(db, candidate) && !strcmp(key, candidate));
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_evict_begin(db, key, &eviction));
    CHECK(!strcmp(eviction.tree_digest, proof.tree_digest));
    CHECK(
        sqlite3_prepare_v2(
            db,
            "INSERT INTO resource_ownership SELECT 'evict-repin',"
            "'asset-root',deployment_id,operation_id,'reserved',NULL,'held','2026-11-03T00:00:00Z' "
            "FROM operations WHERE operation_id=?",
            -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO asset_refs VALUES('evict-repin',?)", -1, &s, NULL) ==
          SQLITE_OK);
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) != SQLITE_DONE);
    sqlite3_finalize(s);
    SQL("COMMIT");
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_evict_begin(db, key, &eviction));
    SQL("COMMIT");
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_evict_finish(db, key, rootfd) < 0);
    SQL("ROLLBACK");
    CHECK(gstd_asset_evict(rootfd, key, &eviction, -1) == GSTD_CACHE_OK);
    SQL("BEGIN IMMEDIATE");
    int foreign_cache = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(foreign_cache >= 0);
    CHECK(gstd_asset_record_evict_finish(db, key, foreign_cache) < 0);
    close(foreign_cache);
    CHECK(!gstd_asset_record_evict_finish(db, key, rootfd));
    SQL("COMMIT");
#endif
    /* A distinct accepted cold start without authorization cannot reserve bytes
     * or reference a not-yet-cached image. No network worker is invoked. */
    json_object_set_new(message, "idempotencyKey", json_string("cold-start"));
    json_t *params = json_object_get(message, "params");
    json_object_set_new(params, "deploymentId", json_string("cold"));
    json_object_set_new(params, "allowProvision", json_false());
    CHECK(!gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, "repository-cold", operation) == GSTD_ACCEPT_NEW);
    CHECK(sqlite3_prepare_v2(db,
                             "INSERT INTO resource_ownership "
                             "VALUES('cold-asset','asset-root','cold',?,'reserved',NULL,'held','"
                             "2026-11-03T00:00:00Z')",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    inserted = sqlite3_step(s);
    sqlite3_finalize(s);
    CHECK(inserted == SQLITE_DONE);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, manifest, "git", operation, "cold-asset", 1024, 1024,
                                  UINT64_C(4) * 1024 * 1024, again_key,
                                  again_name) == GSTD_ASSET_RECORD_CONSENT);
    SQL("COMMIT");
    CHECK(!gstd_asset_record_refcount(db, again_key, &count) && !count && !*again_name);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin_offline(db, manifest, rootfd, "git", operation, "cold-asset",
                                          1024, 1024, UINT64_C(4) * 1024 * 1024, again_key,
                                          again_name) == GSTD_ASSET_RECORD_NEW);
    SQL("COMMIT");
    CHECK(!gstd_asset_record_source(db, again_key, operation, rootfd));
    CHECK(gstd_asset_record_source(db, again_key, operation, -1) < 0);
    CHECK(sqlite3_exec(db,
                       "UPDATE assets SET acquisition_mode='registry',source_device=NULL,"
                       "source_inode=NULL WHERE acquisition_mode='offline'",
                       NULL, NULL, NULL) != SQLITE_OK);
    persistent = gstd_asset_manifest_for_datastore(components, "arm64", "scylladb");
    CHECK(persistent);
    /* A datastore image is implicit in persistent core, never authorized merely
     * because a caller supplies a valid registered image descriptor. */
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, persistent, "scylladb", operation, "cold-asset", 1024, 1024,
                                  UINT64_C(8) * 1024 * 1024, again_key,
                                  again_name) == GSTD_ASSET_RECORD_ERROR);
    SQL("ROLLBACK");
    json_object_set_new(message, "idempotencyKey", json_string("persistent-start"));
    json_object_set_new(params, "deploymentId", json_string("persistent"));
    json_object_set_new(params, "datastore", json_string("scylladb"));
    json_object_set_new(params, "allowProvision", json_true());
    CHECK(!gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, "repository-persistent", operation) ==
          GSTD_ACCEPT_NEW);
    CHECK(sqlite3_prepare_v2(db,
                             "INSERT INTO resource_ownership "
                             "VALUES('scylla-asset','asset-root','persistent',?,'reserved',NULL,"
                             "'held','2026-11-03T00:00:00Z')",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    inserted = sqlite3_step(s);
    sqlite3_finalize(s);
    CHECK(inserted == SQLITE_DONE);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, manifest, "scylladb", operation, "scylla-asset", 1024, 1024,
                                  UINT64_C(8) * 1024 * 1024, again_key,
                                  again_name) == GSTD_ASSET_RECORD_ERROR);
    SQL("ROLLBACK");
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, persistent, "scylladb", operation, "scylla-asset", 1024, 1024,
                                  UINT64_C(8) * 1024 * 1024, again_key,
                                  again_name) == GSTD_ASSET_RECORD_NEW);
    SQL("COMMIT");
    CHECK(*again_key && *again_name && !gstd_asset_record_refcount(db, again_key, &count) &&
          count == 1);
    char datastore_key[65];
    memcpy(datastore_key, again_key, sizeof(datastore_key));
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_asset_record_begin(db, persistent, "scylladb", operation, "scylla-asset", 1024, 1024,
                                  UINT64_C(8) * 1024 * 1024, again_key,
                                  again_name) == GSTD_ASSET_RECORD_HELD);
    SQL("COMMIT");
    CHECK(!strcmp(datastore_key, again_key) && !*again_name);
    SQL("DROP TRIGGER asset_ref_release");
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(gstd_state_open(path, &db) < 0 && !db);
done:
    gstd_asset_cache_unlock(&owner);
    if (file >= 0)
        close(file);
    if (stage >= 0)
        close(stage);
    if (rootfd >= 0)
        close(rootfd);
    json_decref(message);
    json_decref(manifest);
    json_decref(persistent);
    json_decref(components);
    if (db)
        sqlite3_close(db);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
