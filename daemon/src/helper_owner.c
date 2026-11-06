#include "helper_owner.h"
#include <string.h>
enum { RUNNING = 1, FINISHED };
static int commit(sqlite3 *db, int failed) {
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
static int finish(gstd_helper_owner *owner, int outcome) {
    owner->stage = FINISHED;
    owner->outcome = outcome;
    return 1;
}
int gstd_helper_owner_begin(sqlite3 *db, const gstd_deployment_target *target,
                            const char *configuration, const gstd_native_context *context,
                            gstd_helper_owner *owner) {
    if (!db || !sqlite3_get_autocommit(db) || !target || !configuration || !owner || owner->stage ||
        gstd_helper_validate(context))
        return -1;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int reserved =
        context->registered_inputs->purpose == GSTD_EXEC_OIDC_MIGRATE
            ? gstd_deployment_oidc_helper_reserve(db, target, configuration, context)
            : target->migration
            ? gstd_migration_helper_reserve(db, target, configuration, context->backend_id,
                                            context->registered_inputs->purpose)
            : gstd_deployment_helper_reserve(db, target, configuration, context->backend_id,
                                             context->registered_inputs->purpose);
    if (commit(db, reserved))
        return -1;
    *owner = (gstd_helper_owner){.database = db, .target = target, .stage = RUNNING};
    /* A failure after the dispatch commit retains that journal. It cannot be
     * mistaken for a clean helper that may be retried, even without an init. */
    if (gstd_helper_begin(context, &owner->helper))
        finish(owner, -2);
    return 0;
}
int gstd_helper_owner_poll(gstd_helper_owner *owner, int cancellation) {
    if (!owner || !owner->stage || !owner->database || !owner->target ||
        (cancellation != -1 && cancellation < 3) || !sqlite3_get_autocommit(owner->database))
        return -1;
    if (owner->stage == FINISHED)
        return 1;
    int ready = gstd_helper_poll(&owner->helper, cancellation);
    if (ready <= 0)
        return ready;
    const char *id = owner->helper.context.backend_id;
    if (ready == 1) {
        int failed =
            sqlite3_exec(owner->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK;
        if (!failed)
            failed = commit(
                owner->database,
                gstd_deployment_helper_created(owner->database, owner->target, id, &owner->helper));
        if (failed) {
            /* No native START follows a failed durable identity commit. */
            (void)gstd_helper_withhold_start(&owner->helper);
            return finish(owner, -2);
        }
        /* Recheck cancellation after SQLite work, before permitting a mutator. */
        if (gstd_helper_poll(&owner->helper, cancellation) != 1)
            return finish(owner, -2);
        if (gstd_helper_permit_start(&owner->helper)) {
            (void)gstd_helper_withhold_start(&owner->helper);
            return finish(owner, -2);
        }
        return 0;
    }
    if (owner->helper.may_exist || !owner->helper.deleted || owner->helper.outcome == -2)
        return finish(owner, -2);
    if (sqlite3_exec(owner->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return finish(owner, -2);
    if (commit(owner->database,
               gstd_deployment_helper_finished(owner->database, owner->target, id, &owner->helper)))
        return finish(owner, -2);
    return finish(owner, owner->helper.outcome);
}
int gstd_helper_owner_close(gstd_helper_owner *owner) {
    if (!owner || owner->stage != FINISHED || owner->helper.worker.worker ||
        (owner->helper.stage && gstd_helper_close(&owner->helper)))
        return -1;
    memset(owner, 0, sizeof(*owner));
    return 0;
}
int gstd_helper_owner_observers_close(gstd_helper_owner *owner) {
    if (!owner || owner->helper.worker.worker || gstd_helper_observers_close(&owner->helper))
        return -1;
    memset(owner, 0, sizeof(*owner));
    return 0;
}
