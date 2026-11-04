#define _GNU_SOURCE 1
#include "bootstrap_material.h"
#include "gitstored/protocol.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int changed(const gstd_credential *original, const char *field, json_t *replacement) {
    json_t *value = gstd_json_parse(original->bytes, original->length);
    if (!value || json_object_set_new(value, field, replacement)) {
        json_decref(value);
        return 0;
    }
    char *bytes = gstd_json_dump(value);
    json_decref(value);
    gstd_credential candidate = {(unsigned char *)bytes, bytes ? strlen(bytes) : 0};
    int rejected = gstd_bootstrap_material_check(&candidate) == -1;
    gstd_credential_clear(&candidate);
    return rejected;
}
static int worker(pid_t parent) {
    alarm(10);
    struct rlimit core = {0, 0};
    CHECK(!setrlimit(RLIMIT_CORE, &core));
    gstd_credential material = {0}, second = {0};
    CHECK(gstd_bootstrap_material_generate(getpid(), &material) == -1 && !material.bytes);
    CHECK(!gstd_bootstrap_material_generate(parent, &material));
    CHECK(material.length > 300 && material.length <= 4096 &&
          !gstd_bootstrap_material_check(&material));
    CHECK(!gstd_bootstrap_material_generate(parent, &second));
    CHECK(material.length != second.length ||
          memcmp(material.bytes, second.bytes, material.length));
    json_t *value = gstd_json_parse(material.bytes, material.length),
           *other = gstd_json_parse(second.bytes, second.length);
    CHECK(value && other && json_object_size(value) == 9);
    for (size_t i = 0; i < 4; i++) {
        const char *names[] = {"password", "jwtSecret", "apiIssuerPEM", "controllerPEM"};
        CHECK(!json_equal(json_object_get(value, names[i]), json_object_get(other, names[i])));
    }
    CHECK(changed(&material, "apiIssuerPEM", json_incref(json_object_get(value, "controllerPEM"))));
    CHECK(changed(&material, "password", json_string("unsafe-short-password")));
    CHECK(changed(&material, "schemaVersion", json_integer(2)));
    CHECK(changed(&material, "passwordHash",
                  json_string("$2b$31$expensive-untrusted-cost-is-rejected")));
    CHECK(changed(&material, "extra", json_true()));
    CHECK(changed(&material, "username", json_stringn("admin\0suffix", 12)));
    CHECK(
        changed(&material, "apiIssuerPEM", json_string("-----BEGIN ENCRYPTED PRIVATE KEY-----\n")));
    CHECK(changed(&material, "hmacSecret", json_incref(json_object_get(value, "jwtSecret"))));
    json_decref(value);
    json_decref(other);
    gstd_credential_clear(&material);
    gstd_credential_clear(&second);
    CHECK(!material.bytes && !material.length && !second.bytes && !second.length);
    return 0;
}
int main(void) {
    gstd_json_initialize();
    CHECK(geteuid() != 0);
    pid_t parent = getpid(), child = fork();
    CHECK(child >= 0);
    if (!child)
        _exit(worker(parent));
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    return 0;
}
