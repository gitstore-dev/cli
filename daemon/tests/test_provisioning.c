#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "gitstored/provisioning.h"
#include "image_backend.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    int result = 0, fd = -1;
    char *root = gstd_test_dir();
    json_t *components = json_pack("[s]", "api"), *manifest = NULL, *plan = NULL;
    CHECK(root && components);
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    manifest = gstd_asset_manifest(components, architecture);
    CHECK(manifest);
    plan = gstd_provision_plan(manifest, "api", 1024, 4096, 1000);
    CHECK(plan && json_integer_value(json_object_get(plan, "maximumDownloadBytes")) == 1024);
    char *serialized = gstd_json_dump(plan);
    CHECK(serialized);
    free(serialized);
    CHECK(!gstd_provision_plan(manifest, "git", 1024, 4096, 1000));
    CHECK(!gstd_provision_plan(manifest, "api", 0, 4096, 1000));
    CHECK(!gstd_provision_plan(manifest, "api", 1024, 4096, 600001));
    CHECK(!json_object_set_new(manifest, "profileVersion", json_string("foreign")));
    CHECK(!gstd_provision_plan(manifest, "api", 1024, 4096, 1000));
    json_decref(manifest);
    manifest = gstd_asset_manifest(components, architecture);
    fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 0);
    gstd_image_auth auth = {0};
    gstd_provision_result outcome = {0};
    gstd_credential secret = {0};
    const char wrong_scope[] = "{\"identity\":{\"purpose\":\"registry-pull\",\"registry\":\"https:/"
                               "/wrong.invalid\",\"repository\":\"gitstore-dev/api\"}}";
    CHECK(gstd_provision_registry_auth(manifest, "api", -1, (const unsigned char *)wrong_scope,
                                       sizeof(wrong_scope) - 1, 100, &secret, &auth) == -1);
    const char matching_scope[] = "{\"identity\":{\"purpose\":\"registry-pull\",\"registry\":"
                                  "\"https://ghcr.io\",\"repository\":\"gitstore-dev/api\"}}";
    CHECK(gstd_provision_registry_auth(manifest, "api", -1, (const unsigned char *)matching_scope,
                                       sizeof(matching_scope) - 1, 100, &secret, &auth) == -2);
    CHECK(!secret.bytes && !auth.kind);
    CHECK(gstd_provision_stage(manifest, "unknown", fd, &auth, 1024, 4096, 1000, -1, &outcome) ==
          GSTD_PROVISION_INVALID);
    CHECK(!outcome.downloaded_bytes && !outcome.merged_bytes);
    CHECK(gstd_provision_import(manifest, "api", -1, fd, 1024, 4096, 1000, -1, &outcome) ==
          GSTD_PROVISION_INVALID);
    CHECK(gstd_provision_resume(manifest, "api", -1, fd, &auth, 1024, 4096, 1000, -1, &outcome) ==
          GSTD_PROVISION_INVALID);
    CHECK(gstd_provision_materialize(manifest, "api", -1, fd, 1024, 4096, 1000, -1, &outcome) ==
          GSTD_PROVISION_INVALID);
    CHECK(fcntl(fd, F_GETFD) >= 0);
    if (gstd_image_backend_available()) {
        int cancellation[2];
        CHECK(!pipe(cancellation));
        CHECK(write(cancellation[1], "x", 1) == 1);
        gstd_provision_status cancelled = gstd_provision_stage(
            manifest, "api", fd, &auth, 1024, 4096, 1000, cancellation[0], &outcome);
        unsigned char signal;
        CHECK(read(cancellation[0], &signal, 1) == 1 && signal == 'x');
        close(cancellation[0]);
        close(cancellation[1]);
        CHECK(cancelled == GSTD_PROVISION_CANCELLED);
        CHECK(gstd_provision_materialize(manifest, "api", fd, fd, 1024, 4096, 1000, -1, &outcome) ==
              GSTD_PROVISION_INVALID);
        CHECK(gstd_provision_import(manifest, "api", fd, fd, 1024, 4096, 1000, -1, &outcome) ==
              GSTD_PROVISION_INVALID);
        CHECK(gstd_provision_resume(manifest, "api", fd, fd, &auth, 1024, 4096, 1000, -1,
                                    &outcome) == GSTD_PROVISION_INVALID);
    } else {
        CHECK(gstd_provision_stage(manifest, "api", fd, &auth, 1024, 4096, 1000, -1, &outcome) ==
              GSTD_PROVISION_UNAVAILABLE);
    }
done:
    if (fd >= 0)
        close(fd);
    json_decref(plan);
    json_decref(manifest);
    json_decref(components);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
