#define _POSIX_C_SOURCE 200809L
#include "oidc_client.h"
#include "gitstored/protocol.h"
#include <stdio.h>
#include <string.h>

static int same(const json_t *value, const char *expected) {
    size_t length = expected ? strlen(expected) : 0;
    return json_is_string(value) && json_string_length(value) == length &&
           !memcmp(json_string_value(value), expected, length);
}
static int identifier(const char *value) {
    return value && gstd_identifier(value, strnlen(value, 65));
}
int gstd_oidc_client_check(const gstd_oidc_client *client) {
    if (!client || !identifier(client->client_id) || !identifier(client->audience) ||
        !client->redirect_uri)
        return -1;
    /* Reconstruct the canonical selected callback, rejecting alternate hosts,
     * credentials, encodings, query/fragment, leading zeroes and trailing data. */
    unsigned port = 0;
    int end = 0;
    if (strnlen(client->redirect_uri, 129) > 128 ||
        sscanf(client->redirect_uri, "http://127.0.0.1:%5u/callback%n", &port, &end) != 1 || !end ||
        client->redirect_uri[end] || port < 1024 || port > 65535)
        return -1;
    char canonical[64];
    int count = snprintf(canonical, sizeof(canonical), "http://127.0.0.1:%u/callback", port);
    return count > 0 && (size_t)count < sizeof(canonical) &&
                   !strcmp(canonical, client->redirect_uri)
               ? 0
               : -1;
}
static int secret_check(const gstd_credential *secret) {
    if (!secret || !secret->bytes || secret->length < 32 || secret->length > 128)
        return 0;
    for (size_t i = 0; i < secret->length; i++) {
        unsigned char c = secret->bytes[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}
json_t *gstd_oidc_client_request(const gstd_oidc_client *client, const gstd_credential *secret) {
    if (gstd_oidc_client_check(client) || !secret_check(secret))
        return NULL;
    json_t *request = json_pack(
        "{s:s,s:[s,s],s:[s],s:s,s:[s],s:[s],s:s}", "client_id", client->client_id, "grant_types",
        "authorization_code", "refresh_token", "response_types", "code", "scope",
        "openid profile email offline_access", "audience", client->audience, "redirect_uris",
        client->redirect_uri, "token_endpoint_auth_method", "client_secret_basic");
    if (!request ||
        json_object_set_new(request, "client_secret",
                            json_stringn((const char *)secret->bytes, secret->length))) {
        json_decref(request);
        return NULL;
    }
    return request;
}
static int pair(const json_t *array, const char *first, const char *second) {
    return json_is_array(array) && json_array_size(array) == 2 &&
           ((same(json_array_get(array, 0), first) && same(json_array_get(array, 1), second)) ||
            (same(json_array_get(array, 1), first) && same(json_array_get(array, 0), second)));
}
static int one(const json_t *array, const char *expected) {
    return json_is_array(array) && json_array_size(array) == 1 &&
           same(json_array_get(array, 0), expected);
}
static int scopes(const json_t *value) {
    static const char *names[] = {"openid", "profile", "email", "offline_access"};
    if (!json_is_string(value) || json_string_length(value) > 128)
        return 0;
    const char *bytes = json_string_value(value);
    size_t length = json_string_length(value), offset = 0;
    unsigned seen = 0;
    while (offset < length) {
        size_t begin = offset;
        while (offset < length && bytes[offset] != ' ')
            offset++;
        unsigned index;
        for (index = 0; index < 4; index++)
            if (offset - begin == strlen(names[index]) &&
                !memcmp(bytes + begin, names[index], offset - begin))
                break;
        if (index == 4 || (seen & (1u << index)))
            return 0;
        seen |= 1u << index;
        if (offset < length && ++offset == length)
            return 0;
    }
    return seen == 15;
}
int gstd_oidc_client_matches(const gstd_oidc_client *client, const json_t *value,
                             const gstd_credential *secret) {
    if (gstd_oidc_client_check(client) || !json_is_object(value) ||
        !same(json_object_get(value, "client_id"), client->client_id) ||
        !pair(json_object_get(value, "grant_types"), "authorization_code", "refresh_token") ||
        !one(json_object_get(value, "response_types"), "code") ||
        !one(json_object_get(value, "audience"), client->audience) ||
        !one(json_object_get(value, "redirect_uris"), client->redirect_uri) ||
        !same(json_object_get(value, "token_endpoint_auth_method"), "client_secret_basic") ||
        !scopes(json_object_get(value, "scope")))
        return 0;
    const json_t *echo = json_object_get(value, "client_secret");
    if (!secret)
        return !echo || same(echo, "");
    if (!secret_check(secret) || !json_is_string(echo) ||
        json_string_length(echo) != secret->length)
        return 0;
    const unsigned char *bytes = (const unsigned char *)json_string_value(echo);
    unsigned difference = 0;
    for (size_t i = 0; i < secret->length; i++)
        difference |= bytes[i] ^ secret->bytes[i];
    return difference == 0;
}
