#define _GNU_SOURCE 1
#include "gitstored/protocol.h"
#include "oidc_config.h"
#include "oidc_material.h"
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
    int rejected = gstd_oidc_material_check(&candidate) == -1;
    gstd_credential_clear(&candidate);
    return rejected;
}
static int worker(pid_t parent) {
    alarm(10);
    struct rlimit core = {0, 0};
    CHECK(!setrlimit(RLIMIT_CORE, &core));
    gstd_credential material = {0}, second = {0};
    CHECK(gstd_oidc_material_generate(getpid(), &material) == -1 && !material.bytes);
    CHECK(!gstd_oidc_material_generate(parent, &material));
    CHECK(material.length > 400 && material.length <= 1024 && !gstd_oidc_material_check(&material));
    CHECK(!gstd_oidc_material_generate(parent, &second));
    CHECK(material.length == second.length &&
          memcmp(material.bytes, second.bytes, material.length));
    CHECK(gstd_oidc_material_generate(parent, &material) == -1);
    json_t *value = gstd_json_parse(material.bytes, material.length),
           *other = gstd_json_parse(second.bytes, second.length);
    CHECK(value && other && json_object_size(value) == 10);
    const char *fields[] = {"hydraSystem",  "hydraCookie", "kratosCookie", "kratosDefault",
                            "kratosCipher", "uiCookie",    "uiCsrf",       "clientSecret"};
    for (size_t i = 0; i < 8; i++) {
        CHECK(!json_equal(json_object_get(value, fields[i]), json_object_get(other, fields[i])));
        CHECK(changed(&material, fields[i], json_string("short")));
    }
    CHECK(changed(&material, "schemaVersion", json_integer(2)));
    CHECK(changed(&material, "schemaVersion", json_real(1)));
    CHECK(changed(&material, "extra", json_true()));
    CHECK(changed(&material, "format", json_stringn("gitstored-oidc-material/v1\0suffix", 32)));
    CHECK(changed(&material, "clientSecret", json_incref(json_object_get(value, "hydraSystem"))));
    CHECK(changed(&material, "kratosCipher",
                  json_stringn(json_string_value(json_object_get(value, "hydraSystem")), 32)));
    CHECK(changed(&material, "clientSecret",
                  json_string("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")));
    gstd_credential truncated = {material.bytes, material.length - 1};
    CHECK(gstd_oidc_material_check(&truncated) == -1);
    gstd_oidc_ports ports = {
        .issuer = 24001, .bridge = 24002, .identity = 24003, .ui = 24004, .mail = 24005};
    CHECK(!gstd_oidc_ports_check(&ports));
    for (unsigned role = 0; role < 2; role++) {
        json_t *config = gstd_oidc_provider_config((gstd_oidc_provider)role, &ports, &material);
        CHECK(config);
        json_t *serve = json_object_get(config, "serve"), *admin = json_object_get(serve, "admin");
        CHECK(!strcmp(json_string_value(json_object_get(admin, "host")), "127.0.0.1"));
        CHECK(json_integer_value(json_object_get(admin, "port")) == (role ? 4434 : 4445));
        CHECK(!strcmp(json_string_value(json_object_get(config, "dsn")),
                      "sqlite:///work/provider.sqlite?_fk=true"));
        gstd_oidc_config_clear(config);
        json_decref(config);
    }
    CHECK(!gstd_oidc_material_check(&material)); /* No shared/cleansed source strings. */
    gstd_credential ui_env = {0};
    CHECK(!gstd_oidc_ui_environment(&ports, &material, &ui_env));
    CHECK(ui_env.length < 1024 && strstr((char *)ui_env.bytes, "PORT=24004\n"));
    CHECK(strstr((char *)ui_env.bytes, json_string_value(json_object_get(value, "uiCookie"))));
    CHECK(!strstr((char *)ui_env.bytes, json_string_value(json_object_get(value, "hydraSystem"))));
    CHECK(gstd_oidc_ui_environment(&ports, &material, &ui_env) == -1);
    gstd_credential_clear(&ui_env);
    CHECK(!gstd_oidc_material_check(&material));
    ports.issuer = 4445;
    CHECK(gstd_oidc_ports_check(&ports) == -1);
    CHECK(!gstd_oidc_provider_config(GSTD_OIDC_HYDRA, &ports, &material));
    ports.issuer = ports.bridge;
    CHECK(gstd_oidc_ports_check(&ports) == -1);
    ports.issuer = 4434;
    CHECK(gstd_oidc_ports_check(&ports) == -1);
    ports.issuer = 1023;
    CHECK(gstd_oidc_ports_check(&ports) == -1);
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
