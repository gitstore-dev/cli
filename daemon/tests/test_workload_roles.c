#include "workload_roles.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "oidc_profile.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "workload roles line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    gstd_json_initialize();
    gstd_workload_role roles[GSTD_ROLE_COUNT];
    unsigned count = 0;
    const char *architectures[] = {"amd64", "arm64"};
    for (unsigned arch = 0; arch < 2; arch++) {
        for (unsigned selection = 0; selection < 4; selection++) {
            json_t *components = selection == 0 ? json_pack("[s,s]", "api", "git")
                               : selection == 1 ? json_pack("[s]", "oidc")
                               : selection == 2 ? json_pack("[s,s]", "api", "oidc")
                                                : json_pack("[s,s]", "controller", "oidc");
            json_t *manifest = selection ? gstd_oidc_core_manifest(components, architectures[arch], "memdb")
                                        : gstd_asset_manifest(components, architectures[arch]);
            CHECK(manifest && !gstd_workload_layout(manifest, roles, &count));
            const unsigned expected[] = {2, 5, 7, 8};
            CHECK(count == expected[selection]);
            CHECK(gstd_workload_index(roles, count, GSTD_ROLE_GIT) ==
                  (selection == 1 ? -1 : selection ? 5 : 0));
            CHECK(selection == 0 || (roles[0] == GSTD_ROLE_SMTP && roles[4] == GSTD_ROLE_BRIDGE));
            for (unsigned i = 1; i < count; i++)
                CHECK(roles[i] > roles[i - 1]);
            json_t *images = json_object_get(manifest, "images");
            CHECK(!json_array_append(images, json_array_get(images, 0)));
            CHECK(gstd_workload_layout(manifest, roles, &count) == -1);
            json_decref(manifest);
            json_decref(components);
        }
    }
    CHECK(gstd_workload_oidc_volume(GSTD_ROLE_HYDRA) == 0 &&
          gstd_workload_oidc_volume(GSTD_ROLE_KRATOS) == 1 &&
          gstd_workload_oidc_volume(GSTD_ROLE_GIT) == -1);
    CHECK(gstd_workload_oidc_health(GSTD_ROLE_BRIDGE) == GSTD_OIDC_HEALTH_BRIDGE &&
          gstd_workload_oidc_health(GSTD_ROLE_API) == -1);
    CHECK(!gstd_workload_name((gstd_workload_role)99));
    CHECK(gstd_workload_layout(NULL, roles, &count) == -1);
    return 0;
}
