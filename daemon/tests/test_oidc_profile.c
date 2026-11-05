#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "gitstored/provisioning.h"
#include "network_worker.h"
#include "oidc_profile.h"
#include "volumes.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC profile line %d\n", __LINE__);                                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    const char *architectures[] = {"amd64", "arm64"};
    for (size_t i = 0; i < 2; i++) {
        const char *stores[] = {"memdb", "scylladb"};
        for (size_t store = 0; store < 2; store++) {
            json_t *requested = json_pack("[s,s]", "controller", "oidc");
            json_t *combined = gstd_oidc_core_manifest(requested, architectures[i], stores[store]);
            CHECK(combined && !gstd_oidc_core_manifest_check(combined) &&
                  !gstd_asset_manifest_check(combined) &&
                  json_array_size(json_object_get(combined, "images")) == 8 + store);
            gstd_oidc_ports public_ports = {24001, 24002, 24003, 24004, 24005};
            gstd_network_worker_request combined_request = {.manifest = combined,
                                                            .oidc_ports = &public_ports};
            unsigned combined_roles = 0;
            CHECK(!gstd_network_request_roles(&combined_request, &combined_roles) &&
                  combined_roles == 127);
            json_t *core_components = json_pack("[s,s,s]", "api", "git", "controller");
            json_t *base =
                gstd_asset_manifest_for_datastore(core_components, architectures[i], stores[store]);
            CHECK(base);
            const char *core_names[] = {"api", "git", "controller", "scylladb"};
            for (size_t n = 0; n < 3 + store; n++) {
                json_t *one = gstd_asset_manifest_component(combined, core_names[n]);
                json_t *old = gstd_asset_manifest_component(base, core_names[n]);
                CHECK(one && old && json_equal(one, old));
                json_decref(one);
                json_decref(old);
            }
            json_t *one = gstd_asset_manifest_component(combined, "oidc-hydra");
            json_t *old = gstd_oidc_manifest_component(architectures[i], "oidc-hydra");
            CHECK(one && old && json_equal(one, old));
            json_decref(one);
            json_decref(old);
            CHECK(!json_object_set_new(combined, "baseProfileDigest", json_string("unregistered")));
            CHECK(gstd_asset_manifest_check(combined) == -1);
            json_decref(base);
            json_decref(core_components);
            json_decref(combined);
            json_decref(requested);
        }
        json_t *only_oidc = json_pack("[s]", "oidc");
        json_t *small = gstd_oidc_core_manifest(only_oidc, architectures[i], "memdb");
        CHECK(small && !gstd_asset_manifest_check(small) &&
              json_array_size(json_object_get(small, "images")) == 5);
        CHECK(!gstd_oidc_core_manifest(only_oidc, architectures[i], "scylladb"));
        json_decref(small);
        json_decref(only_oidc);
        json_t *plain = gstd_oidc_core_manifest(NULL, architectures[i], "memdb");
        CHECK(plain && !gstd_asset_manifest_check(plain) &&
              json_array_size(json_object_get(plain, "images")) == 3 &&
              !json_object_get(plain, "oidcProfileDigest"));
        json_decref(plain);
        json_t *manifest = gstd_oidc_manifest(architectures[i]);
        CHECK(manifest && !gstd_oidc_manifest_check(manifest) &&
              !gstd_asset_manifest_check(manifest));
        gstd_oidc_ports ports = {24001, 24002, 24003, 24004, 24005};
        gstd_network_worker_request network = {.manifest = manifest, .oidc_ports = &ports};
        unsigned roles = 0;
        CHECK(!gstd_network_request_roles(&network, &roles) && roles == 124);
        network.oidc_ports = NULL;
        CHECK(gstd_network_request_roles(&network, &roles) == -1);
        network.oidc_ports = &ports;
        ports.mail = ports.ui;
        CHECK(gstd_network_request_roles(&network, &roles) == -1);
        ports.mail = 4434;
        CHECK(gstd_network_request_roles(&network, &roles) == -1);
        ports.mail = 24005;
        json_t *ports_value = gstd_network_oidc_ports_json(&ports);
        gstd_oidc_ports parsed = {0};
        CHECK(ports_value && !gstd_network_oidc_ports_parse(ports_value, &parsed) &&
              parsed.mail == ports.mail);
        json_decref(ports_value);
        json_t *images = json_object_get(manifest, "images");
        CHECK(json_array_size(images) == 5);
        for (size_t j = 0; j < 5; j++) {
            json_t *image = json_array_get(images, j);
            CHECK(gstd_oidc_component(json_string_value(json_object_get(image, "component"))));
            CHECK(!strcmp(json_string_value(json_object_get(image, "integrityPolicy")),
                          "source-platform-digest"));
            const char *component = json_string_value(json_object_get(image, "component"));
            json_t *single = gstd_asset_manifest_component(manifest, component);
            json_t *expected = gstd_oidc_manifest_component(architectures[i], component);
            CHECK(single && expected && json_equal(single, expected) &&
                  !gstd_asset_manifest_check(single) &&
                  json_array_size(json_object_get(single, "images")) == 1);
            network.manifest = single;
            CHECK(gstd_network_request_roles(&network, &roles) == -1);
            network.manifest = manifest;
            json_t *plan = gstd_provision_plan(single, component, 536870912, 1073741824, 90000);
            CHECK(plan && json_integer_value(json_object_get(plan, "timeoutMs")) == 90000);
            json_decref(plan);
            CHECK(!json_object_set_new(single, "unregistered", json_true()));
            CHECK(gstd_asset_manifest_check(single) == -1 &&
                  !gstd_provision_plan(single, component, 536870912, 1073741824, 90000));
            json_decref(single);
            json_decref(expected);
            if (j < 2) {
                const char *compatibility =
                    gstd_oidc_volume_compatibility((gstd_oidc_volume_role)j);
                const char *index = json_string_value(json_object_get(image, "indexDigest"));
                CHECK(compatibility && index && strlen(index) == 71 && strlen(compatibility) > 64 &&
                      !strcmp(compatibility + strlen(compatibility) - 64, index + 7));
            }
        }
        json_t *image = json_array_get(images, 0);
        CHECK(!json_object_set_new(image, "source", json_string("docker.io/untrusted/provider")));
        CHECK(gstd_oidc_manifest_check(manifest) == -1);
        json_decref(manifest);
        manifest = gstd_oidc_manifest(architectures[i]);
        CHECK(manifest);
        CHECK(!json_object_set_new(manifest, "profileVersion", json_string("unregistered")));
        CHECK(gstd_oidc_manifest_check(manifest) == -1);
        json_decref(manifest);
    }
    json_t *invalid = json_pack("[s,s]", "oidc", "oidc");
    CHECK(invalid && !gstd_oidc_core_components(invalid) &&
          !gstd_oidc_core_manifest(invalid, "arm64", "memdb"));
    json_decref(invalid);
    invalid = json_pack("[s]", "admin");
    CHECK(invalid && !gstd_oidc_core_components(invalid));
    json_decref(invalid);
    CHECK(!gstd_oidc_core_manifest(NULL, "arm64", "arbitrary"));
    CHECK(!gstd_oidc_manifest("riscv64") && !gstd_oidc_manifest(NULL));
    CHECK(!gstd_oidc_manifest_component("arm64", "api") &&
          !gstd_oidc_manifest_component("riscv64", "oidc-hydra"));
    CHECK(!gstd_oidc_component("api") && !gstd_oidc_component("arbitrary") &&
          !gstd_oidc_component(NULL));
    return 0;
}
