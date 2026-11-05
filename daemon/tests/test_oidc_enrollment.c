#include "gitstored/protocol.h"
#include "oidc_enrollment.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "enrollment policy line %d\n", __LINE__);                              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int state(const json_t *value, const char *expected) {
    const char *actual = json_string_value(json_object_get(value, "state"));
    return actual && !strcmp(actual, expected);
}
int main(void) {
    gstd_json_initialize();
    gstd_oidc_client client = {"fixture-client", "gitstore", "http://127.0.0.1:24004/callback"};
    json_t *binding =
        json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:i,s:s,s:s}", "storeId", "fixture-store",
                  "deploymentId", "one", "operationId", "start-one", "providerResourceId",
                  "hydra-one", "volumeProducerId", "volume-one", "credentialRef", "oidc-one",
                  "generation", 1, "volumeDevice", "42", "volumeInode", "18446744073709551615");
    CHECK(binding);
    json_t *prepared = gstd_oidc_enrollment_prepare(binding, &client);
    CHECK(prepared && state(prepared, "prepared"));
    CHECK(!gstd_oidc_enrollment_matches(prepared, binding, &client));
    json_t *dispatched = gstd_oidc_enrollment_dispatch(prepared);
    CHECK(dispatched && state(dispatched, "dispatched") && state(prepared, "prepared"));
    CHECK(!gstd_oidc_enrollment_dispatch(dispatched));
    CHECK(!gstd_oidc_enrollment_observe(prepared, GSTD_OIDC_CLIENT_CREATED));
    json_t *created = gstd_oidc_enrollment_observe(dispatched, GSTD_OIDC_CLIENT_CREATED);
    CHECK(created && state(created, "created"));
    CHECK(!gstd_oidc_enrollment_dispatch(created));
    CHECK(!gstd_oidc_enrollment_observe(created, GSTD_OIDC_UNCERTAIN));
    json_t *recovered = gstd_oidc_enrollment_recover(dispatched);
    CHECK(recovered && state(recovered, "uncertain"));
    CHECK(!gstd_oidc_enrollment_dispatch(recovered));
    CHECK(!gstd_oidc_enrollment_observe(recovered, GSTD_OIDC_CLIENT_CREATED));
    json_decref(recovered);
    for (int result = GSTD_OIDC_UNCERTAIN; result <= GSTD_OIDC_PUBLIC_MATCH; result++) {
        if (result == GSTD_OIDC_CLIENT_CREATED)
            continue;
        json_t *uncertain = gstd_oidc_enrollment_observe(dispatched, (gstd_oidc_http_status)result);
        CHECK(uncertain && state(uncertain, "uncertain"));
        CHECK(!gstd_oidc_enrollment_dispatch(uncertain));
        json_decref(uncertain);
    }
    recovered = gstd_oidc_enrollment_recover(created);
    CHECK(recovered && json_equal(recovered, created));
    json_decref(recovered);
    recovered = gstd_oidc_enrollment_recover(prepared);
    CHECK(recovered && json_equal(recovered, prepared));
    json_decref(recovered);
    CHECK(!json_object_set_new(binding, "generation", json_integer(2)));
    CHECK(!gstd_oidc_enrollment_resume(created, binding, &client));
    CHECK(!json_object_set_new(binding, "operationId", json_string("start-two")));
    CHECK(!json_object_set_new(binding, "providerResourceId", json_string("hydra-two")));
    json_t *retained = gstd_oidc_enrollment_resume(created, binding, &client);
    CHECK(retained && state(retained, "created") &&
          !gstd_oidc_enrollment_matches(retained, binding, &client) &&
          !gstd_oidc_enrollment_dispatch(retained));
    json_t *unattempted = gstd_oidc_enrollment_resume(prepared, binding, &client);
    CHECK(unattempted && state(unattempted, "prepared"));
    json_t *first_attempt = gstd_oidc_enrollment_dispatch(unattempted);
    CHECK(first_attempt && state(first_attempt, "dispatched") &&
          !gstd_oidc_enrollment_dispatch(first_attempt));
    json_decref(first_attempt);
    json_decref(unattempted);
    CHECK(!gstd_oidc_enrollment_resume(dispatched, binding, &client));
    recovered = gstd_oidc_enrollment_recover(dispatched);
    CHECK(recovered && !gstd_oidc_enrollment_resume(recovered, binding, &client));
    json_decref(recovered);
    const char *stable[] = {"storeId",       "deploymentId", "volumeProducerId",
                            "credentialRef", "volumeDevice", "volumeInode"};
    for (size_t i = 0; i < sizeof(stable) / sizeof(*stable); i++) {
        json_t *changed = json_deep_copy(binding);
        CHECK(changed && !json_object_set_new(changed, stable[i], json_string("99")));
        CHECK(!gstd_oidc_enrollment_resume(created, changed, &client));
        json_decref(changed);
    }
    gstd_oidc_client changed_client = client;
    changed_client.redirect_uri = "http://127.0.0.1:24005/callback";
    CHECK(!gstd_oidc_enrollment_resume(created, binding, &changed_client));
    changed_client = client;
    changed_client.client_id = "another-client";
    CHECK(!gstd_oidc_enrollment_resume(created, binding, &changed_client));
    changed_client = client;
    changed_client.audience = "another-audience";
    CHECK(!gstd_oidc_enrollment_resume(created, binding, &changed_client));
    CHECK(!gstd_oidc_enrollment_resume(retained, binding, &client));
    json_decref(retained);
    CHECK(gstd_oidc_enrollment_matches(created, binding, &client) == -1);
    CHECK(json_integer_value(json_object_get(json_object_get(created, "binding"), "generation")) ==
          1);
    CHECK(!json_object_set_new(binding, "volumeInode", json_string("18446744073709551616")));
    CHECK(!gstd_oidc_enrollment_prepare(binding, &client));
    CHECK(!json_object_set_new(binding, "volumeInode", json_string("01")));
    CHECK(!gstd_oidc_enrollment_prepare(binding, &client));
    CHECK(!json_object_set_new(created, "secret", json_string("forbidden")));
    CHECK(!gstd_oidc_enrollment_recover(created));
    CHECK(!json_object_del(created, "secret"));
    CHECK(!json_object_set_new(created, "state", json_stringn("created\0suffix", 14)));
    CHECK(!gstd_oidc_enrollment_recover(created));
    json_decref(created);
    json_decref(dispatched);
    json_decref(prepared);
    json_decref(binding);
    return 0;
}
