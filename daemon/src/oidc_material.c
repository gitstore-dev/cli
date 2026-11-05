#define _GNU_SOURCE 1
#include "oidc_material.h"
#include "gitstored/protocol.h"
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sys/resource.h>
#include <unistd.h>
static const char format_name[] = "gitstored-oidc-material/v1";
static const char *fields[] = {"hydraSystem",  "hydraCookie", "kratosCookie", "kratosDefault",
                               "kratosCipher", "uiCookie",    "uiCsrf",       "clientSecret"};
static size_t field_length(size_t i) { return i == 4 ? 32 : 64; }
static void clear_document(json_t *value) {
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        json_t *item = json_object_get(value, fields[i]);
        if (json_is_string(item))
            OPENSSL_cleanse((void *)json_string_value(item), json_string_length(item));
    }
    json_decref(value);
}
int gstd_oidc_material_check(const gstd_credential *material) {
    if (!material || !material->bytes || !material->length || material->length > 1024)
        return -1;
    json_t *value = gstd_json_parse(material->bytes, material->length);
    const json_t *version = json_object_get(value, "schemaVersion"),
                 *format = json_object_get(value, "format");
    int valid = json_is_object(value) && json_object_size(value) == 10 &&
                json_is_integer(version) && json_integer_value(version) == 1 &&
                json_is_string(format) && json_string_length(format) == sizeof(format_name) - 1 &&
                !memcmp(json_string_value(format), format_name, sizeof(format_name) - 1);
    for (size_t i = 0; valid && i < sizeof(fields) / sizeof(*fields); i++) {
        const json_t *item = json_object_get(value, fields[i]);
        const char *text = json_string_value(item);
        valid = text && json_string_length(item) == field_length(i);
        for (size_t n = 0; valid && n < field_length(i); n++)
            valid = (text[n] >= '0' && text[n] <= '9') || (text[n] >= 'a' && text[n] <= 'f');
        /* Distinct purposes must not accidentally reuse a key (including the
         * shorter AES key as a prefix of another secret). */
        for (size_t j = 0; valid && j < i; j++) {
            size_t length = field_length(i) < field_length(j) ? field_length(i) : field_length(j);
            valid = CRYPTO_memcmp(text, json_string_value(json_object_get(value, fields[j])),
                                  length) != 0;
        }
    }
    clear_document(value);
    return valid ? 0 : -1;
}
int gstd_oidc_material_generate(pid_t owner, gstd_credential *output) {
    struct rlimit limit;
    if (!output || output->bytes || output->length || owner <= 0 || owner == getpid() ||
        !geteuid() || getrlimit(RLIMIT_CORE, &limit) || limit.rlim_cur)
        return -1;
    json_t *value = json_pack("{s:i,s:s}", "schemaVersion", 1, "format", format_name);
    unsigned char entropy[32] = {0};
    char encoded[65] = {0};
    static const char digits[] = "0123456789abcdef";
    int valid = value != NULL;
    for (size_t i = 0; valid && i < sizeof(fields) / sizeof(*fields); i++) {
        size_t length = field_length(i);
        valid = RAND_priv_bytes(entropy, (int)(length / 2)) == 1;
        if (valid) {
            for (size_t n = 0; n < length / 2; n++) {
                encoded[2 * n] = digits[entropy[n] >> 4];
                encoded[2 * n + 1] = digits[entropy[n] & 15];
            }
            encoded[length] = 0;
            valid = !json_object_set_new(value, fields[i], json_stringn(encoded, length));
        }
        OPENSSL_cleanse(entropy, sizeof(entropy));
        OPENSSL_cleanse(encoded, sizeof(encoded));
    }
    char *bytes = valid ? gstd_json_dump(value) : NULL;
    clear_document(value);
    if (!bytes)
        return -1;
    output->bytes = (unsigned char *)bytes;
    output->length = strlen(bytes);
    if (gstd_oidc_material_check(output)) {
        gstd_credential_clear(output);
        return -1;
    }
    return 0;
}
#else
int gstd_oidc_material_check(const gstd_credential *material) {
    (void)material;
    return -1;
}
int gstd_oidc_material_generate(pid_t owner, gstd_credential *output) {
    (void)owner;
    (void)output;
    return -1;
}
#endif
