#include "oidc_config.h"
#include "gitstored/protocol.h"
#include "oidc_material.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int gstd_oidc_ports_check(const gstd_oidc_ports *ports) {
    if (!ports)
        return -1;
    const uint16_t values[] = {ports->issuer, ports->bridge, ports->identity, ports->ui,
                               ports->mail};
    const uint16_t reserved[] = {1025, 4000, 9000, 6000, 50051, 5001, 4445, 4434};
    for (size_t i = 0; i < 5; i++) {
        if (values[i] < 1024)
            return -1;
        for (size_t j = 0; j < sizeof(reserved) / sizeof(*reserved); j++)
            if (values[i] == reserved[j])
                return -1;
        for (size_t j = 0; j < i; j++)
            if (values[i] == values[j])
                return -1;
    }
    return 0;
}
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
#include <openssl/crypto.h>
static json_t *url(uint16_t port, const char *path) {
    char text[128];
    int count = snprintf(text, sizeof(text), "http://127.0.0.1:%u%s", (unsigned)port, path);
    return count > 0 && (size_t)count < sizeof(text) ? json_string(text) : NULL;
}
void gstd_oidc_config_clear(json_t *value) {
    if (json_is_string(value))
        OPENSSL_cleanse((void *)json_string_value(value), json_string_length(value));
    else if (json_is_array(value)) {
        for (size_t i = 0; i < json_array_size(value); i++)
            gstd_oidc_config_clear(json_array_get(value, i));
    } else if (json_is_object(value)) {
        const char *key;
        json_t *item;
        json_object_foreach(value, key, item) {
            (void)key;
            gstd_oidc_config_clear(item);
        }
    }
    /* Wipe recursively without decref here: the caller owns one root reference. */
}
static int secret_array(json_t *secrets, const char *key, const json_t *material,
                        const char *field) {
    const char *text = json_string_value(json_object_get(material, field));
    json_t *array = text ? json_pack("[s]", text) : NULL;
    return array ? json_object_set_new(secrets, key, array) : -1;
}
json_t *gstd_oidc_provider_config(gstd_oidc_provider provider, const gstd_oidc_ports *ports,
                                  const gstd_credential *material) {
    if ((provider != GSTD_OIDC_HYDRA && provider != GSTD_OIDC_KRATOS) ||
        gstd_oidc_ports_check(ports) || gstd_oidc_material_check(material))
        return NULL;
    /* Config format accepted by Ory's JSON/YAML loader. No arbitrary service
     * setting, hostname, path, DSN, command or return URL is accepted. */
    static const char hydra[] =
        "{\"dsn\":\"sqlite:///work/provider.sqlite?_fk=true\","
        "\"serve\":{\"public\":{\"host\":\"0.0.0.0\",\"cors\":{\"enabled\":true,\"allowed_"
        "origins\":[]}},"
        "\"admin\":{\"host\":\"127.0.0.1\",\"port\":4445}},"
        "\"urls\":{\"self\":{}},\"secrets\":{},"
        "\"strategies\":{\"access_token\":\"jwt\",\"scope\":\"exact\"},"
        "\"webfinger\":{\"oidc_discovery\":{\"supported_scope\":[\"openid\",\"profile\",\"email\","
        "\"offline_access\"],"
        "\"supported_claims\":[\"sub\",\"email\",\"preferred_username\"]}},"
        "\"log\":{\"level\":\"error\",\"format\":\"json\",\"leak_sensitive_values\":false}}";
    static const char kratos[] =
        "{\"version\":\"v1.3.1\",\"dsn\":\"sqlite:///work/provider.sqlite?_fk=true\","
        "\"serve\":{\"public\":{\"host\":\"0.0.0.0\",\"cors\":{\"enabled\":true,\"allowed_"
        "origins\":[]}},"
        "\"admin\":{\"host\":\"127.0.0.1\",\"port\":4434}},"
        "\"identity\":{\"default_schema_id\":\"gitstore-user\",\"schemas\":[{\"id\":\"gitstore-"
        "user\","
        "\"url\":\"file:///run/gitstore-oidc/identity.schema.json\"}]},"
        "\"courier\":{\"smtp\":{\"connection_uri\":\"smtp://127.0.0.1:1025/"
        "?disable_starttls=true\"}},"
        "\"selfservice\":{\"allowed_return_urls\":[],\"methods\":{\"password\":{\"enabled\":true}},"
        "\"flows\":{\"error\":{},\"settings\":{},\"recovery\":{\"enabled\":true},"
        "\"verification\":{\"enabled\":true},\"login\":{\"lifespan\":\"10m\"},"
        "\"registration\":{\"enabled\":true,\"lifespan\":\"10m\","
        "\"after\":{\"password\":{\"hooks\":[{\"hook\":\"session\"}]}}}}},"
        "\"secrets\":{},\"hashers\":{\"algorithm\":\"bcrypt\",\"bcrypt\":{\"cost\":8}},"
        "\"log\":{\"level\":\"error\",\"format\":\"json\",\"leak_sensitive_values\":false}}";
    const char *source = provider == GSTD_OIDC_HYDRA ? hydra : kratos;
    json_t *value = gstd_json_parse((const unsigned char *)source, strlen(source)),
           *private = gstd_json_parse(material->bytes, material->length);
    json_t *public = json_object_get(json_object_get(value, "serve"), "public"),
           *secrets = json_object_get(value, "secrets"),
           *origins = json_object_get(json_object_get(public, "cors"), "allowed_origins");
    uint16_t port = provider == GSTD_OIDC_HYDRA ? ports->issuer : ports->identity;
    int bad = !value || !private || json_object_set_new(public, "port", json_integer(port));
    if (!bad && provider == GSTD_OIDC_HYDRA) {
        json_t *urls = json_object_get(value, "urls");
        bad = json_object_set_new(json_object_get(urls, "self"), "issuer", url(port, "/")) ||
              json_object_set_new(urls, "login", url(ports->bridge, "/login")) ||
              json_object_set_new(urls, "consent", url(ports->bridge, "/consent")) ||
              json_array_append_new(origins, url(ports->bridge, "")) ||
              secret_array(secrets, "system", private, "hydraSystem") ||
              secret_array(secrets, "cookie", private, "hydraCookie");
    } else if (!bad) {
        json_t *selfservice = json_object_get(value, "selfservice"),
               *returns = json_object_get(selfservice, "allowed_return_urls"),
               *flows = json_object_get(selfservice, "flows");
        bad = json_object_set_new(public, "base_url", url(port, "/")) ||
              json_array_append_new(origins, url(port, "")) ||
              json_array_append_new(origins, url(ports->ui, "")) ||
              json_object_set_new(selfservice, "default_browser_return_url",
                                  url(ports->ui, "/welcome")) ||
              json_array_append_new(returns, url(ports->ui, "")) ||
              json_array_append_new(returns, url(ports->bridge, "")) ||
              secret_array(secrets, "cookie", private, "kratosCookie") ||
              secret_array(secrets, "default", private, "kratosDefault") ||
              secret_array(secrets, "cipher", private, "kratosCipher");
        const char *names[] = {"error",        "settings", "recovery",
                               "verification", "login",    "registration"};
        for (size_t i = 0; !bad && i < 6; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/%s", names[i]);
            bad = json_object_set_new(json_object_get(flows, names[i]), "ui_url",
                                      url(ports->ui, path));
        }
    }
    gstd_oidc_config_clear(private);
    json_decref(private);
    if (bad) {
        gstd_oidc_config_clear(value);
        json_decref(value);
        return NULL;
    }
    return value;
}
int gstd_oidc_ui_environment(const gstd_oidc_ports *ports, const gstd_credential *material,
                             gstd_credential *output) {
    if (!output || output->bytes || output->length || gstd_oidc_ports_check(ports) ||
        gstd_oidc_material_check(material))
        return -1;
    json_t *value = gstd_json_parse(material->bytes, material->length);
    const char *cookie = json_string_value(json_object_get(value, "uiCookie")),
               *csrf = json_string_value(json_object_get(value, "uiCsrf"));
    char *bytes = malloc(1024);
    int length = bytes && cookie && csrf
                     ? snprintf(bytes, 1024,
                                "PORT=%u\nKRATOS_PUBLIC_URL=http://127.0.0.1:%u/"
                                "\nKRATOS_BROWSER_URL=http://127.0.0.1:%u/\n"
                                "COOKIE_SECRET=%s\nCSRF_COOKIE_NAME=gitstore_oidc_csrf\nCSRF_"
                                "COOKIE_SECRET=%s\nCOOKIE_SECURE=false\nNODE_ENV=production\n",
                                ports->ui, ports->identity, ports->identity, cookie, csrf)
                     : -1;
    gstd_oidc_config_clear(value);
    json_decref(value);
    if (length <= 0 || length >= 1024) {
        if (bytes)
            OPENSSL_cleanse(bytes, 1024);
        free(bytes);
        return -1;
    }
    output->bytes = (unsigned char *)bytes;
    output->length = (size_t)length;
    return 0;
}

#else
void gstd_oidc_config_clear(json_t *value) { (void)value; }
json_t *gstd_oidc_provider_config(gstd_oidc_provider provider, const gstd_oidc_ports *ports,
                                  const gstd_credential *material) {
    (void)provider;
    (void)ports;
    (void)material;
    return NULL;
}
#endif

#if !defined(__linux__) || !defined(GITSTORED_BOOTSTRAP)
int gstd_oidc_ui_environment(const gstd_oidc_ports *ports, const gstd_credential *material,
                             gstd_credential *output) {
    (void)ports;
    (void)material;
    (void)output;
    return -1;
}
#endif
