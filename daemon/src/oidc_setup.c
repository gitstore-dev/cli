#define _POSIX_C_SOURCE 200809L
#include "oidc_setup.h"
#include "gitstored/protocol.h"
#include "oidc_profile.h"
#include "volume_records.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
enum { CONFIG_SCOPE = 1, PRIOR_SCOPE, MATERIAL, PROVIDER_VOLUME, FINISHED };
static int sql(gstd_oidc_setup *s, const char *text) {
    return sqlite3_exec(s->environment->database, text, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static int commit(gstd_oidc_setup *s, int failed) {
    if (failed || sql(s, "COMMIT")) {
        sql(s, "ROLLBACK");
        return -1;
    }
    return 0;
}
static int finish(gstd_oidc_setup *s, int outcome, int detail) {
    s->stage = FINISHED;
    s->outcome = outcome;
    s->detail = detail;
    return 1;
}
static gstd_filesystem_input input(gstd_oidc_setup *s, gstd_filesystem_action action, int first,
                                   int second) {
    return (gstd_filesystem_input){.action = action,
                                   .manifest = s->target->manifest,
                                   .store_id = s->target->store_id,
                                   .deployment_id = s->target->deployment_id,
                                   .operation_id = s->target->operation_id,
                                   .generation = s->target->generation,
                                   .resource_id = s->configuration_id,
                                   .resource_kind = "configuration",
                                   .descriptors = {first, second, -1}};
}
static int material(gstd_oidc_setup *s) {
    gstd_filesystem_input request =
        input(s, GSTD_FS_OIDC_PREPARE, s->configuration.descriptors[1], s->environment->broker_fd);
    request.oidc_ports = s->ports;
    request.credential_source = s->source;
    request.credential_initial = s->initial;
    request.credential_prior_required = s->previous_identity != NULL;
    request.descriptors[2] = s->previous_identity ? s->previous.descriptors[1] : -1;
    s->stage = MATERIAL;
    return gstd_filesystem_launch(&request, 20000, &s->worker) ? finish(s, -1, 0) : 0;
}
static int volume(gstd_oidc_setup *s) {
    unsigned i = s->index;
    gstd_oidc_volume_role role = (gstd_oidc_volume_role)i;
    if (i >= 2 || sql(s, "BEGIN IMMEDIATE"))
        return finish(s, -2, 0);
    gstd_accept_status reserved = gstd_oidc_volume_reserve(
        s->environment->database, role, s->target->operation_id, s->volume_ids[i]);
    int existing =
        (reserved == GSTD_ACCEPT_NEW || reserved == GSTD_ACCEPT_REPLAY)
            ? gstd_oidc_volume_record_begin(s->environment->database, &s->environment->volume_root,
                                            role, s->volume_ids[i], s->target->operation_id,
                                            s->producers[i], &s->expected)
            : -1;
    if (commit(s, existing < 0))
        return finish(s, -2, 0);
    s->fresh[i] = existing == 0;
    gstd_filesystem_input request =
        input(s, GSTD_FS_OIDC_VOLUME, s->environment->volume_root.root_fd,
              s->environment->volume_root.lock_fd);
    request.volume_id = s->volume_ids[i];
    request.producer_id = s->producers[i];
    request.volume_root = s->environment->volume_root;
    request.volume_expected = s->expected;
    request.volume_existing = existing;
    request.oidc_volume_role = role;
    s->stage = PROVIDER_VOLUME;
    return gstd_filesystem_launch(&request, 10000, &s->worker) ? finish(s, -1, 0) : 0;
}
int gstd_oidc_setup_begin(const gstd_deployment_environment *environment,
                          const gstd_deployment_target *target, const gstd_oidc_ports *ports,
                          gstd_oidc_setup *s) {
    if (!environment || !environment->database || !sqlite3_get_autocommit(environment->database) ||
        !environment->settings || environment->resource_fd < 3 || environment->broker_fd < 3 ||
        environment->volume_root.root_fd < 3 || environment->volume_root.lock_fd < 3 || !target ||
        target->migration ||
        !gstd_identifier(target->operation_id, strnlen(target->operation_id, 65)) ||
        !gstd_identifier(target->deployment_id, strnlen(target->deployment_id, 65)) ||
        strnlen(target->store_id, 33) != 32 ||
        memcmp(target->store_id, environment->volume_root.store_id, 33) || !target->generation ||
        target->generation > GSTD_SAFE_INTEGER || !ports || gstd_oidc_ports_check(ports) ||
        gstd_oidc_core_manifest_check(target->manifest) ||
        !json_object_get(target->manifest, "oidcProfileDigest") || !s || s->environment ||
        s->worker.worker)
        return -1;
    char id[65];
    int n = snprintf(id, sizeof(id), "oidc-cfg-%s", target->operation_id);
    if (n < 1 || n >= (int)sizeof(id))
        return -1;
    json_t *source = gstd_runtime_settings_oidc_source(environment->settings, target->store_id,
                                                       target->deployment_id);
    if (!source)
        return -1;
    memset(s, 0, sizeof(*s));
    s->environment = environment;
    s->target = target;
    s->ports = *ports;
    s->source = source;
    strcpy(s->configuration_id, id);
    for (unsigned i = 0; i < 8; i++)
        s->configuration.descriptors[i] = s->previous.descriptors[i] = -1;
    for (unsigned i = 0; i < 4; i++)
        s->files[i] = -1;
    for (unsigned i = 0; i < 2; i++)
        s->volumes[i].data_fd = s->volumes[i].writer_fd = -1;
    if (sql(s, "BEGIN IMMEDIATE")) {
        finish(s, -2, 0);
        return 0;
    }
    if (commit(s, gstd_deployment_resource_reserve(environment->database, target, id,
                                                   "configuration"))) {
        finish(s, -2, 0);
        return 0;
    }
    gstd_filesystem_input request = input(s, GSTD_FS_SCOPE, environment->resource_fd, -1);
    s->stage = CONFIG_SCOPE;
    if (gstd_filesystem_launch(&request, 10000, &s->worker))
        finish(s, -1, 0);
    return 0;
}
int gstd_oidc_setup_poll(gstd_oidc_setup *s, int cancel) {
    if (!s || !s->environment || !sqlite3_get_autocommit(s->environment->database) || cancel < -1)
        return -1;
    if (s->stage == FINISHED)
        return 1;
    gstd_filesystem_reply reply = {0};
    int ready = gstd_filesystem_poll(&s->worker, cancel, &reply);
    if (ready <= 0)
        return ready;
    if (reply.status) {
        int outcome = reply.status == -2 ? -2 : -1, detail = reply.detail;
        gstd_filesystem_reply_close(&reply);
        return finish(s, outcome, detail);
    }
    sqlite3 *db = s->environment->database;
    if (s->stage == CONFIG_SCOPE) {
        if (reply.descriptor_count != 2) {
            gstd_filesystem_reply_close(&reply);
            return finish(s, -2, 0);
        }
        s->configuration = reply;
        if (sql(s, "BEGIN IMMEDIATE"))
            return finish(s, -2, 0);
        int failed =
            gstd_deployment_scope_record(db, s->target, s->configuration_id, &s->configuration) ||
            gstd_deployment_oidc_credential_reserve(db, s->target, s->configuration_id,
                                                    s->credential_reference, &s->initial);
        int previous =
            failed ? -1
                   : gstd_deployment_oidc_preparation_source(db, s->target, s->configuration_id,
                                                             s->previous_id, &s->previous_identity);
        if (commit(s, failed || previous < 0))
            return finish(s, -2, 0);
        if (previous == 1)
            return material(s);
        gstd_filesystem_input request =
            input(s, GSTD_FS_REOPEN_SCOPE, s->environment->resource_fd, -1);
        request.manifest = json_object_get(s->previous_identity, "executionManifest");
        request.scope_expected = s->previous_identity;
        request.resource_id = s->previous_id;
        request.operation_id =
            json_string_value(json_object_get(s->previous_identity, "operationId"));
        request.generation =
            (uint64_t)json_integer_value(json_object_get(s->previous_identity, "generation"));
        s->stage = PRIOR_SCOPE;
        return gstd_filesystem_launch(&request, 10000, &s->worker) ? finish(s, -1, 0) : 0;
    }
    if (s->stage == PRIOR_SCOPE) {
        if (reply.descriptor_count != 2) {
            gstd_filesystem_reply_close(&reply);
            return finish(s, -2, 0);
        }
        s->previous = reply;
        return material(s);
    }
    if (s->stage == MATERIAL) {
        if (reply.descriptor_count != 4) {
            gstd_filesystem_reply_close(&reply);
            return finish(s, -2, 0);
        }
        if (sql(s, "BEGIN IMMEDIATE")) {
            gstd_filesystem_reply_close(&reply);
            return finish(s, -2, 0);
        }
        int failed = gstd_deployment_oidc_prepared_record(
            db, s->target, s->configuration_id, s->configuration.descriptors[1], &s->ports, &reply);
        if (commit(s, failed)) {
            gstd_filesystem_reply_close(&reply);
            return finish(s, -2, 0);
        }
        for (unsigned i = 0; i < 4; i++)
            s->files[i] = reply.descriptors[i];
        reply.descriptor_count = 0;
        s->index = 0;
        return volume(s);
    }
    if (s->stage == PROVIDER_VOLUME) {
        if (reply.descriptor_count != 2 || reply.detail != GSTD_VOLUME_OK || s->index >= 2) {
            gstd_filesystem_reply_close(&reply);
            return finish(s, -2, 0);
        }
        unsigned i = s->index;
        s->volumes[i] = (gstd_volume_handle){.data_fd = reply.descriptors[0],
                                             .writer_fd = reply.descriptors[1],
                                             .identity = reply.volume};
        reply.descriptor_count = 0;
        if (sql(s, "BEGIN IMMEDIATE"))
            return finish(s, -2, 0);
        if (commit(s, gstd_oidc_volume_record_ready(db, &s->environment->volume_root,
                                                    (gstd_oidc_volume_role)i, s->volume_ids[i],
                                                    s->target->operation_id, &s->volumes[i])))
            return finish(s, -2, 0);
        if (++s->index == 2)
            return finish(s, 0, 0);
        return volume(s);
    }
    gstd_filesystem_reply_close(&reply);
    return finish(s, -2, 0);
}
int gstd_oidc_setup_close(gstd_oidc_setup *s) {
    if (!s || !s->environment || s->worker.worker)
        return -1;
    for (unsigned i = 0; i < 4; i++)
        if (s->files[i] >= 3)
            close(s->files[i]);
    for (unsigned i = 0; i < 2; i++)
        gstd_volume_handle_close(&s->volumes[i]);
    gstd_filesystem_reply_close(&s->configuration);
    gstd_filesystem_reply_close(&s->previous);
    json_decref(s->source);
    json_decref(s->previous_identity);
    memset(s, 0, sizeof(*s));
    return 0;
}
