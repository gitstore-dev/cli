#define _POSIX_C_SOURCE 200809L
#include "oidc_enrollment.h"
#include "gitstored/protocol.h"
#include <string.h>
static int equal(const json_t *v, const char *s) {
    return json_is_string(v) && json_string_length(v) == strlen(s) &&
           !memcmp(json_string_value(v), s, strlen(s));
}
static int identifier(const json_t *v) {
    return json_is_string(v) && gstd_identifier(json_string_value(v), json_string_length(v));
}
static int decimal(const json_t *v) {
    const char *text = json_string_value(v);
    size_t length = json_string_length(v);
    if (!text || !length || length > 20 || text[0] < '1' || text[0] > '9')
        return 0;
    uint64_t value = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9' || value > (UINT64_MAX - (unsigned)(text[i] - '0')) / 10)
            return 0;
        value = value * 10 + (unsigned)(text[i] - '0');
    }
    return value != 0;
}
static int binding_valid(const json_t *binding) {
    if (!json_is_object(binding) || json_object_size(binding) != 9)
        return 0;
    const char *ids[] = {"storeId",          "deploymentId", "operationId", "providerResourceId",
                         "volumeProducerId", "credentialRef"};
    for (size_t i = 0; i < 6; i++)
        if (!identifier(json_object_get(binding, ids[i])))
            return 0;
    const json_t *generation = json_object_get(binding, "generation");
    return json_is_integer(generation) && json_integer_value(generation) > 0 &&
           json_integer_value(generation) <= INT64_C(9007199254740991) &&
           decimal(json_object_get(binding, "volumeDevice")) &&
           decimal(json_object_get(binding, "volumeInode"));
}
static int valid(const json_t *journal) {
    if (!json_is_object(journal) || json_object_size(journal) != 5 ||
        !json_is_integer(json_object_get(journal, "schemaVersion")) ||
        json_integer_value(json_object_get(journal, "schemaVersion")) != 1 ||
        !equal(json_object_get(journal, "kind"), "oidc-enrollment") ||
        !binding_valid(json_object_get(journal, "binding")))
        return 0;
    const json_t *client = json_object_get(journal, "client");
    if (!json_is_object(client) || json_object_size(client) != 3)
        return 0;
    const json_t *id = json_object_get(client, "clientId"),
                 *audience = json_object_get(client, "audience"),
                 *redirect = json_object_get(client, "redirectUri");
    if (!identifier(id) || !identifier(audience) || !json_is_string(redirect) ||
        json_string_length(redirect) != strlen(json_string_value(redirect)))
        return 0;
    gstd_oidc_client policy = {json_string_value(id), json_string_value(audience),
                               json_string_value(redirect)};
    const json_t *state = json_object_get(journal, "state");
    return !gstd_oidc_client_check(&policy) &&
           (equal(state, "prepared") || equal(state, "dispatched") || equal(state, "created") ||
            equal(state, "uncertain"));
}
json_t *gstd_oidc_enrollment_prepare(const json_t *binding, const gstd_oidc_client *client) {
    if (!binding_valid(binding) || gstd_oidc_client_check(client))
        return NULL;
    json_t *value = json_pack("{s:i,s:s,s:O,s:{s:s,s:s,s:s},s:s}", "schemaVersion", 1, "kind",
                              "oidc-enrollment", "binding", binding, "client", "clientId",
                              client->client_id, "audience", client->audience, "redirectUri",
                              client->redirect_uri, "state", "prepared");
    /* Do not share mutable caller binding; no journal change may alter intent. */
    json_t *copy = value ? json_deep_copy(value) : NULL;
    json_decref(value);
    return copy;
}
int gstd_oidc_enrollment_matches(const json_t *journal, const json_t *binding,
                                 const gstd_oidc_client *client) {
    if (!valid(journal))
        return -1;
    json_t *expected = gstd_oidc_enrollment_prepare(binding, client);
    int matched =
        expected &&
        json_equal(json_object_get(expected, "binding"), json_object_get(journal, "binding")) &&
        json_equal(json_object_get(expected, "client"), json_object_get(journal, "client"));
    json_decref(expected);
    return matched ? 0 : -1;
}
static json_t *transition(const json_t *journal, const char *state) {
    json_t *copy = json_deep_copy(journal);
    if (copy && json_object_set_new(copy, "state", json_string(state))) {
        json_decref(copy);
        copy = NULL;
    }
    return copy;
}
json_t *gstd_oidc_enrollment_dispatch(const json_t *journal) {
    return valid(journal) && equal(json_object_get(journal, "state"), "prepared")
               ? transition(journal, "dispatched")
               : NULL;
}
json_t *gstd_oidc_enrollment_observe(const json_t *journal, gstd_oidc_http_status outcome) {
    if (!valid(journal) || !equal(json_object_get(journal, "state"), "dispatched") ||
        outcome < GSTD_OIDC_UNCERTAIN || outcome > GSTD_OIDC_PUBLIC_MATCH)
        return NULL;
    /* A public GET, preflight rejection or missing reply cannot acknowledge a
     * committed dispatch. Only the worker's matching CREATE 201 does. */
    return transition(journal, outcome == GSTD_OIDC_CLIENT_CREATED ? "created" : "uncertain");
}
json_t *gstd_oidc_enrollment_recover(const json_t *journal) {
    if (!valid(journal))
        return NULL;
    return transition(journal, equal(json_object_get(journal, "state"), "dispatched")
                                   ? "uncertain"
                                   : json_string_value(json_object_get(journal, "state")));
}
json_t *gstd_oidc_enrollment_resume(const json_t *journal, const json_t *binding,
                                    const gstd_oidc_client *client) {
    const json_t *state = json_object_get(journal, "state");
    if (!valid(journal) || (!equal(state, "created") && !equal(state, "prepared")) ||
        !binding_valid(binding))
        return NULL;
    const json_t *prior = json_object_get(journal, "binding");
    const char *stable[] = {"storeId",       "deploymentId", "volumeProducerId",
                            "credentialRef", "volumeDevice", "volumeInode"};
    for (size_t i = 0; i < sizeof(stable) / sizeof(*stable); i++)
        if (!json_equal(json_object_get(prior, stable[i]), json_object_get(binding, stable[i])))
            return NULL;
    if (json_integer_value(json_object_get(binding, "generation")) <=
            json_integer_value(json_object_get(prior, "generation")) ||
        json_equal(json_object_get(binding, "operationId"),
                   json_object_get(prior, "operationId")) ||
        json_equal(json_object_get(binding, "providerResourceId"),
                   json_object_get(prior, "providerResourceId")))
        return NULL;
    json_t *retained = gstd_oidc_enrollment_prepare(binding, client);
    if (retained &&
        (!json_equal(json_object_get(retained, "client"), json_object_get(journal, "client")) ||
         json_object_set_new(retained, "state", json_string(json_string_value(state))))) {
        json_decref(retained);
        retained = NULL;
    }
    return retained;
}
