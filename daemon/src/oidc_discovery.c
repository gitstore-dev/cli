#include "oidc_discovery.h"
#include <stdio.h>
#include <string.h>
static int same(const json_t *object, const char *key, const char *expected) {
    const json_t *value = json_object_get(object, key);
    return json_is_string(value) && json_string_length(value) == strlen(expected) &&
           !memcmp(json_string_value(value), expected, strlen(expected));
}
int gstd_oidc_discovery_matches(const json_t *value, const gstd_oidc_ports *ports) {
    if (gstd_oidc_ports_check(ports) || !json_is_object(value))
        return 0;
    char issuer[64], jwks[96];
    snprintf(issuer, sizeof(issuer), "http://127.0.0.1:%u/", ports->issuer);
    snprintf(jwks, sizeof(jwks), "http://127.0.0.1:%u/.well-known/jwks.json", ports->issuer);
    return same(value, "issuer", issuer) && same(value, "jwks_uri", jwks);
}
static int base64url(const json_t *value, size_t minimum, size_t maximum) {
    const char *bytes = json_string_value(value);
    size_t length = json_string_length(value);
    if (!bytes || length < minimum || length > maximum)
        return 0;
    for (size_t i = 0; i < length; i++)
        if (!((bytes[i] >= 'A' && bytes[i] <= 'Z') || (bytes[i] >= 'a' && bytes[i] <= 'z') ||
              (bytes[i] >= '0' && bytes[i] <= '9') || bytes[i] == '_' || bytes[i] == '-'))
            return 0;
    return 1;
}
int gstd_oidc_jwks_matches(const json_t *value) {
    const json_t *keys = json_object_get(value, "keys");
    if (!json_is_array(keys) || !json_array_size(keys) || json_array_size(keys) > 32)
        return 0;
    for (size_t i = 0; i < json_array_size(keys); i++) {
        const json_t *key = json_array_get(keys, i), *kid = json_object_get(key, "kid");
        if (!same(key, "kty", "RSA") || !same(key, "alg", "RS256") || !same(key, "use", "sig") ||
            !json_is_string(kid) || !json_string_length(kid) || json_string_length(kid) > 256 ||
            strlen(json_string_value(kid)) != json_string_length(kid) ||
            !base64url(json_object_get(key, "n"), 342, 1366) ||
            !base64url(json_object_get(key, "e"), 1, 16))
            return 0;
        const char *private_fields[] = {"d", "p", "q", "dp", "dq", "qi", "oth", "k"};
        for (size_t j = 0; j < sizeof(private_fields) / sizeof(*private_fields); j++)
            if (json_object_get(key, private_fields[j]))
                return 0;
        for (size_t j = 0; j < i; j++)
            if (json_equal(kid, json_object_get(json_array_get(keys, j), "kid")))
                return 0;
    }
    return 1;
}
