#define _GNU_SOURCE 1
#include "bootstrap_material.h"
#include "gitstored/protocol.h"
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
#include <crypt.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <sys/resource.h>
#include <unistd.h>
static int hex_secret(char output[65]) {
    unsigned char bytes[32];
    if (RAND_priv_bytes(bytes, sizeof(bytes)) != 1)
        return -1;
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); i++) {
        output[2 * i] = digits[bytes[i] >> 4];
        output[2 * i + 1] = digits[bytes[i] & 15];
    }
    output[64] = 0;
    OPENSSL_cleanse(bytes, sizeof(bytes));
    return 0;
}
static char *private_key(void) {
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    BIO *memory = BIO_new(BIO_s_mem());
    char *result = NULL;
    if (key && memory && PEM_write_bio_PrivateKey(memory, key, NULL, NULL, 0, NULL, NULL) == 1) {
        BUF_MEM *buffer = NULL;
        BIO_get_mem_ptr(memory, &buffer);
        if (buffer && buffer->length > 0 && buffer->length <= 1024) {
            result = malloc(buffer->length + 1);
            if (result) {
                memcpy(result, buffer->data, buffer->length);
                result[buffer->length] = 0;
            }
        }
        if (buffer)
            OPENSSL_cleanse(buffer->data, buffer->length);
    }
    BIO_free(memory);
    EVP_PKEY_free(key);
    return result;
}
static int password_hash(const char *password, char output[61]) {
    unsigned char entropy[16];
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    struct crypt_data *state = calloc(1, sizeof(*state));
    int result = -1;
    if (state && RAND_priv_bytes(entropy, sizeof(entropy)) == 1 &&
        crypt_gensalt_rn("$2b$", 10, (const char *)entropy, sizeof(entropy), salt, sizeof(salt))) {
        char *hash = crypt_r(password, salt, state);
        if (hash && strlen(hash) == 60 && !strncmp(hash, "$2b$10$", 7)) {
            memcpy(output, hash, 61);
            result = 0;
        }
    }
    if (state)
        OPENSSL_cleanse(state, sizeof(*state));
    free(state);
    OPENSSL_cleanse(entropy, sizeof(entropy));
    OPENSSL_cleanse(salt, sizeof(salt));
    return result;
}
static int fixed_hex(const json_t *value, const char *field) {
    const json_t *item = json_object_get(value, field);
    const char *bytes = json_string_value(item);
    if (!bytes || json_string_length(item) != 64)
        return 0;
    for (size_t i = 0; i < 64; i++)
        if (!((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'a' && bytes[i] <= 'f')))
            return 0;
    return 1;
}
static int no_password(char *buffer, int size, int writing, void *argument) {
    (void)buffer;
    (void)size;
    (void)writing;
    (void)argument;
    return 0;
}
static int fixed_string(const json_t *value, const char *field, const char *expected) {
    const json_t *item = json_object_get(value, field);
    const char *bytes = json_string_value(item);
    return bytes && json_string_length(item) == strlen(expected) &&
           !memcmp(bytes, expected, strlen(expected));
}
static EVP_PKEY *parse_key(const json_t *value, const char *field) {
    const json_t *item = json_object_get(value, field);
    const char *bytes = json_string_value(item);
    if (!bytes || json_string_length(item) != strlen(bytes) || strlen(bytes) > 1024 ||
        strncmp(bytes, "-----BEGIN PRIVATE KEY-----\n", 28))
        return NULL;
    BIO *memory = BIO_new_mem_buf(bytes, (int)strlen(bytes));
    EVP_PKEY *key = memory ? PEM_read_bio_PrivateKey(memory, NULL, no_password, NULL) : NULL;
    char trailing[8];
    int extra = memory ? BIO_read(memory, trailing, sizeof(trailing)) : -1;
    BIO_free(memory);
    if (!key || EVP_PKEY_is_a(key, "ED25519") != 1 || extra > 0) {
        EVP_PKEY_free(key);
        return NULL;
    }
    return key;
}
int gstd_bootstrap_material_check(const gstd_credential *material) {
    if (!material || !material->bytes || !material->length || material->length > 4096)
        return -1;
    json_t *value = gstd_json_parse(material->bytes, material->length);
    int valid = value && json_object_size(value) == 9 &&
                json_integer_value(json_object_get(value, "schemaVersion")) == 1 &&
                json_is_integer(json_object_get(value, "schemaVersion")) &&
                fixed_string(value, "format", "gitstored-bootstrap-material/v1") &&
                fixed_string(value, "username", "admin") && fixed_hex(value, "password") &&
                fixed_hex(value, "jwtSecret") && fixed_hex(value, "hmacSecret");
    EVP_PKEY *issuer = valid ? parse_key(value, "apiIssuerPEM") : NULL,
             *controller = valid ? parse_key(value, "controllerPEM") : NULL;
    valid = valid && issuer && controller && EVP_PKEY_eq(issuer, controller) == 0;
    if (valid) {
        const char *password = json_string_value(json_object_get(value, "password")),
                   *jwt = json_string_value(json_object_get(value, "jwtSecret")),
                   *hmac = json_string_value(json_object_get(value, "hmacSecret")),
                   *hash = json_string_value(json_object_get(value, "passwordHash"));
        const json_t *item = json_object_get(value, "passwordHash");
        valid = strcmp(password, jwt) && strcmp(password, hmac) && strcmp(jwt, hmac) && hash &&
                json_string_length(item) == 60 && strlen(hash) == 60 &&
                !strncmp(hash, "$2b$10$", 7);
        if (valid) {
            for (size_t i = 7; i < 60; i++)
                if (!((hash[i] >= 'a' && hash[i] <= 'z') || (hash[i] >= 'A' && hash[i] <= 'Z') ||
                      (hash[i] >= '0' && hash[i] <= '9') || hash[i] == '.' || hash[i] == '/'))
                    valid = 0;
        }
        struct crypt_data *state = valid ? calloc(1, sizeof(*state)) : NULL;
        char *actual = state ? crypt_r(password, hash, state) : NULL;
        valid = valid && actual && !strcmp(actual, hash);
        if (state)
            OPENSSL_cleanse(state, sizeof(*state));
        free(state);
    }
    EVP_PKEY_free(issuer);
    EVP_PKEY_free(controller);
    json_decref(value);
    return valid ? 0 : -1;
}
int gstd_bootstrap_material_generate(pid_t owner, gstd_credential *output) {
    struct rlimit limit;
    if (!output || output->bytes || output->length || owner <= 0 || owner == getpid() ||
        !geteuid() || getrlimit(RLIMIT_CORE, &limit) || limit.rlim_cur)
        return -1;
    char password[65] = {0}, jwt[65] = {0}, hmac[65] = {0}, hash[61] = {0};
    char *issuer = NULL, *controller = NULL, *encoded = NULL;
    json_t *value = NULL;
    if (hex_secret(password) || hex_secret(jwt) || hex_secret(hmac) ||
        password_hash(password, hash) || !(issuer = private_key()) || !(controller = private_key()))
        goto done;
    value = json_pack("{s:i,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}", "schemaVersion", 1, "format",
                      "gitstored-bootstrap-material/v1", "username", "admin", "password", password,
                      "jwtSecret", jwt, "hmacSecret", hmac, "passwordHash", hash, "apiIssuerPEM",
                      issuer, "controllerPEM", controller);
    encoded = value ? gstd_json_dump(value) : NULL;
    if (encoded) {
        output->bytes = (unsigned char *)encoded;
        output->length = strlen(encoded);
        encoded = NULL;
        if (gstd_bootstrap_material_check(output))
            gstd_credential_clear(output);
    }
done:
    OPENSSL_cleanse(password, sizeof(password));
    OPENSSL_cleanse(jwt, sizeof(jwt));
    OPENSSL_cleanse(hmac, sizeof(hmac));
    OPENSSL_cleanse(hash, sizeof(hash));
    if (issuer)
        OPENSSL_cleanse(issuer, strlen(issuer));
    if (controller)
        OPENSSL_cleanse(controller, strlen(controller));
    free(issuer);
    free(controller);
    free(encoded);
    json_decref(value);
    return output->bytes ? 0 : -1;
}
char *gstd_bootstrap_controller_public(const gstd_credential *material) {
    if (gstd_bootstrap_material_check(material))
        return NULL;
    json_t *value = gstd_json_parse(material->bytes, material->length);
    EVP_PKEY *key = value ? parse_key(value, "controllerPEM") : NULL;
    BIO *memory = key ? BIO_new(BIO_s_mem()) : NULL;
    char *result = NULL;
    if (memory && PEM_write_bio_PUBKEY(memory, key) == 1) {
        BUF_MEM *buffer = NULL;
        BIO_get_mem_ptr(memory, &buffer);
        if (buffer && buffer->length > 0 && buffer->length <= 1024) {
            result = malloc(buffer->length + 1);
            if (result) {
                memcpy(result, buffer->data, buffer->length);
                result[buffer->length] = 0;
            }
        }
    }
    BIO_free(memory);
    EVP_PKEY_free(key);
    json_decref(value);
    return result;
}
#else
int gstd_bootstrap_material_generate(pid_t owner, gstd_credential *output) {
    (void)owner;
    (void)output;
    return -1;
}
int gstd_bootstrap_material_check(const gstd_credential *material) {
    (void)material;
    return -1;
}
char *gstd_bootstrap_controller_public(const gstd_credential *material) {
    (void)material;
    return NULL;
}
#endif
