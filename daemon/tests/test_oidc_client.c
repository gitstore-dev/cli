#include "gitstored/protocol.h"
#include "oidc_client.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    gstd_oidc_client client = {"gitstore-oidc-fixture-one", "gitstore",
                               "http://127.0.0.1:3000/callback"};
    unsigned char bytes[64];
    memset(bytes, 'S', sizeof(bytes));
    gstd_credential secret = {.bytes = bytes, .length = sizeof(bytes)};
    CHECK(!gstd_oidc_client_check(&client));
    json_t *request = gstd_oidc_client_request(&client, &secret);
    CHECK(request && json_object_size(request) == 8);
    CHECK(gstd_oidc_client_matches(&client, request, &secret));
    CHECK(
        !gstd_oidc_client_matches(&client, request, NULL)); /* A GET must not disclose a secret. */
    json_t *reply = json_deep_copy(request);
    CHECK(reply && !json_object_del(reply, "client_secret"));
    CHECK(gstd_oidc_client_matches(&client, reply, NULL));
    CHECK(!gstd_oidc_client_matches(&client, reply,
                                    &secret)); /* Public match is not a create acknowledgment. */
    CHECK(!json_object_set_new(reply, "registration_access_token",
                               json_string("synthetic-discarded-provider-credential")));
    CHECK(gstd_oidc_client_matches(&client, reply, NULL)); /* No credential field is projected. */
    CHECK(!json_object_set_new(reply, "grant_types",
                               json_pack("[s,s]", "refresh_token", "authorization_code")));
    CHECK(!json_object_set_new(reply, "scope", json_string("email offline_access profile openid")));
    CHECK(gstd_oidc_client_matches(&client, reply, NULL));
    const char *bad_scopes[] = {"openid profile email",
                                "openid profile email offline_access admin",
                                "openid profile profile offline_access",
                                "openid  profile email offline_access",
                                "openid profile email offline_access ",
                                "openid\tprofile email offline_access"};
    for (unsigned i = 0; i < sizeof(bad_scopes) / sizeof(bad_scopes[0]); i++) {
        CHECK(!json_object_set_new(reply, "scope", json_string(bad_scopes[i])));
        CHECK(!gstd_oidc_client_matches(&client, reply, NULL));
    }
    CHECK(!json_object_set(reply, "scope", json_object_get(request, "scope")));
    const char *fields[] = {"client_id",
                            "grant_types",
                            "response_types",
                            "scope",
                            "audience",
                            "redirect_uris",
                            "token_endpoint_auth_method"};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        json_t *copy = json_deep_copy(reply);
        CHECK(copy);
        CHECK(!json_object_set_new(copy, fields[i], json_string("conflicting")));
        CHECK(!gstd_oidc_client_matches(&client, copy, NULL));
        json_decref(copy);
    }
    bytes[0] = 'T';
    CHECK(!gstd_oidc_client_matches(&client, request, &secret));
    bytes[0] = 'S';
    secret.length = 31;
    CHECK(!gstd_oidc_client_request(&client, &secret));
    secret.length = 64;
    bytes[0] = 0;
    CHECK(!gstd_oidc_client_request(&client, &secret));
    bytes[0] = 'S';
    const char *bad_urls[] = {"http://localhost:3000/callback",
                              "https://127.0.0.1:3000/callback",
                              "http://127.0.0.1:3000/callback?next=remote",
                              "http://127.0.0.1:3000/callback#fragment",
                              "http://127.0.0.1:03000/callback",
                              "http://127.0.0.1:3000/../callback",
                              "http://127.0.0.1:80/callback",
                              "http://127.0.0.1:65536/callback",
                              "http://127.0.0.1:+3000/callback",
                              "http://127.0.0.1:3000/callback/"};
    for (unsigned i = 0; i < sizeof(bad_urls) / sizeof(bad_urls[0]); i++) {
        gstd_oidc_client bad = client;
        bad.redirect_uri = bad_urls[i];
        CHECK(gstd_oidc_client_check(&bad) && !gstd_oidc_client_request(&bad, &secret));
    }
    gstd_oidc_client bad = client;
    bad.client_id = "../another-client";
    CHECK(gstd_oidc_client_check(&bad) && !gstd_oidc_client_request(&bad, &secret));
    json_decref(reply);
    json_decref(request);
    return 0;
}
