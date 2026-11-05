#ifndef GSTD_HELPER_OWNER_H
#define GSTD_HELPER_OWNER_H
#include "deployment_records.h"
typedef struct {
    sqlite3 *database;
    const gstd_deployment_target *target;
    gstd_helper helper;
    int stage, outcome;
} gstd_helper_owner;
/* Parent-only journaled helper dispatch. Borrow target/context inputs through
 * close. Requires an accepted persistent start, prepared helper scope, running
 * datastore and committed namespace attachments. No transaction may be open.
 * Dispatch is committed before CREATE, kernel identity before START, observed
 * deletion before exposing success/retry permission. No graph step is implied. */
int gstd_helper_owner_begin(sqlite3 *, const gstd_deployment_target *, const char *configuration_id,
                            const gstd_native_context *, gstd_helper_owner *);
/* 0=pending,1=finished,-1=invalid. outcome 0=verified success,-1=known failure,
 * -2=uncertain. Cancellation never releases a resource or signals a mutator. */
int gstd_helper_owner_poll(gstd_helper_owner *, int cancellation_fd);
/* Observer closure only, after completion and worker reaping. */
int gstd_helper_owner_close(gstd_helper_owner *);
int gstd_helper_owner_observers_close(gstd_helper_owner *);
#endif
