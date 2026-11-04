#include "bootstrap.h"
#include "gitstored/assets.h"
#include "gitstored/profiles.h"
#include "gitstored/protocol.h"
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            return __LINE__;                                                                       \
    } while (0)
int main(void) {
    gstd_json_initialize();
    json_t *requested = json_pack("[s]", "git"), *expanded = gstd_profile_components(requested);
    CHECK(expanded && json_array_size(expanded) == 2 && json_array_size(requested) == 1);
    CHECK(!strcmp(json_string_value(json_array_get(expanded, 0)), "api") &&
          !strcmp(json_string_value(json_array_get(expanded, 1)), "git"));
    json_decref(expanded);
    json_decref(requested);
    requested = json_pack("[s,s]", "api", "storefront");
    CHECK(!gstd_profile_components(requested));
    json_decref(requested);
    requested = json_pack("[s,s]", "git", "git");
    CHECK(!gstd_profile_components(requested));
    json_decref(requested);
    requested = json_array();
    CHECK(!gstd_profile_components(requested));
    json_decref(requested);
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller");
    json_t *manifest = gstd_asset_manifest(components, "arm64");
    gstd_bootstrap_graph graph;
    CHECK(!gstd_bootstrap_graph_begin(manifest, "memdb", 2, &graph));
    CHECK(gstd_bootstrap_graph_observe(&graph, 1, GSTD_BOOT_PREPARE, 0, NULL) == -1 &&
          graph.stage == GSTD_BOOT_PREPARE);
    CHECK(gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_PREPARE, 1, NULL) == -1);
    CHECK(gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_LAUNCH_API, 0, NULL) == -1);
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_PREPARE, 0, NULL));
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_LAUNCH_GIT, 0, NULL));
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_LAUNCH_API, 0, NULL));
    for (unsigned i = 0; i < 5; i++)
        CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_WAIT_API, 1, NULL) &&
              graph.stage == GSTD_BOOT_WAIT_API);
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_WAIT_API, 0, NULL));
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_WAIT_GIT, 0, NULL));
    gstd_bootstrap_graph uncertain = graph;
    CHECK(!gstd_bootstrap_graph_observe(&uncertain, 2, GSTD_BOOT_ENROLL, -2, "known-partial-uid"));
    CHECK(uncertain.outcome == -2 && !strcmp(uncertain.controller_uid, "known-partial-uid"));
    CHECK(gstd_bootstrap_graph_observe(&uncertain, 2, GSTD_BOOT_ENROLL, 0, "other-uid") == -1);
    CHECK(gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_ENROLL, 0, NULL) == -1);
    CHECK(gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_ENROLL, 0, "injected\nuid") == -1);
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_ENROLL, 0, "assigned-uid"));
    CHECK(gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_LAUNCH_CONTROLLER, 0, NULL) == -1);
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_BIND_CONTROLLER, 0, NULL));
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_LAUNCH_CONTROLLER, 0, NULL));
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_WAIT_CONTROLLER, 1, NULL) &&
          graph.stage != GSTD_BOOT_COMPLETE);
    CHECK(!gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_WAIT_CONTROLLER, 0, NULL) &&
          graph.stage == GSTD_BOOT_COMPLETE);
    CHECK(gstd_bootstrap_graph_observe(&graph, 2, GSTD_BOOT_COMPLETE, 0, NULL) == -1);
    CHECK(gstd_bootstrap_graph_begin(manifest, "scylladb", 3, &graph) == -1);
    json_decref(manifest);
    json_decref(components);
    components = json_pack("[s,s]", "api", "git");
    manifest = gstd_asset_manifest(components, "arm64");
    CHECK(!gstd_bootstrap_graph_begin(manifest, "memdb", 3, &graph));
    for (gstd_bootstrap_stage stage = GSTD_BOOT_PREPARE; stage <= GSTD_BOOT_WAIT_GIT; stage++)
        CHECK(!gstd_bootstrap_graph_observe(&graph, 3, stage, 0, NULL));
    CHECK(graph.stage == GSTD_BOOT_COMPLETE && !graph.controller && !*graph.controller_uid);
    json_decref(manifest);
    json_decref(components);
    return 0;
}
