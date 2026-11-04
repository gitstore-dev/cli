#include "gitstored/wait.h"
#include <string.h>
static int equals(json_t *object, const char *field, const char *expected) {
    json_t *v = json_object_get(object, field);
    return json_is_string(v) && json_string_length(v) == strlen(expected) &&
           !memcmp(json_string_value(v), expected, strlen(expected));
}
gstd_query_status gstd_wait_poll(sqlite3 *db, const char *id, const char *until, json_t **out) {
    *out = NULL;
    if (!id || !until ||
        (strcmp(until, "accepted") && strcmp(until, "terminal") && strcmp(until, "ready")))
        return GSTD_QUERY_INVALID;
    json_t *params = json_pack("{s:s}", "operationId", id), *op = NULL;
    if (!params)
        return GSTD_QUERY_DATABASE;
    gstd_query_status status = gstd_inspect(db, GSTD_OPERATION_INSPECT, params, &op);
    json_decref(params);
    if (status != GSTD_QUERY_OK)
        return status;
    if (!strcmp(until, "ready") && !equals(op, "method", "deployment.start")) {
        json_decref(op);
        return GSTD_QUERY_INVALID;
    }
    int terminal = equals(op, "state", "succeeded") || equals(op, "state", "failed") ||
                   equals(op, "state", "cancelled");
    if (!strcmp(until, "accepted") || (!strcmp(until, "terminal") && terminal) ||
        (!strcmp(until, "ready") &&
         (equals(op, "state", "failed") || equals(op, "state", "cancelled")))) {
        *out = op;
        return GSTD_QUERY_OK;
    }
    if (!strcmp(until, "ready") && equals(op, "state", "succeeded")) {
        json_t *deployment = NULL,
               *selected = json_pack("{s:O}", "deploymentId", json_object_get(op, "deploymentId"));
        if (!selected) {
            json_decref(op);
            return GSTD_QUERY_DATABASE;
        }
        status = gstd_inspect(db, GSTD_INSPECT, selected, &deployment);
        json_decref(selected);
        if (status != GSTD_QUERY_OK) {
            json_decref(op);
            return status;
        }
        int current = equals(deployment, "lastOperationId", id);
        int ready = current && equals(deployment, "desiredState", "running") &&
                    equals(deployment, "readiness", "ready") &&
                    equals(deployment, "recovery", "normal") &&
                    json_equal(json_object_get(deployment, "generation"),
                               json_object_get(deployment, "observedGeneration"));
        json_decref(deployment);
        if (ready) {
            *out = op;
            return GSTD_QUERY_OK;
        }
    }
    json_decref(op);
    return GSTD_QUERY_PENDING;
}
