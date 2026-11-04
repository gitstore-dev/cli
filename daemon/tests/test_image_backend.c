#include "image_backend.h"
#include <stdio.h>
int main(void) {
    gstd_image_result result = {99, 99};
    gstd_image_auth auth = {0};
    if (gstd_image_stage(NULL, 1, -1, &auth, &result) != GSTD_IMAGE_INVALID ||
        result.downloaded_bytes || result.merged_bytes)
        return 1;
    result = (gstd_image_result){99, 99};
    if (gstd_image_import(NULL, 1, -1, -1, &result) != GSTD_IMAGE_INVALID ||
        result.downloaded_bytes || result.merged_bytes)
        return 1;
    const unsigned char malformed[] = "{}";
    if (gstd_image_materialize(malformed, sizeof(malformed) - 1, 0, 0, &result) !=
        (gstd_image_backend_available() ? GSTD_IMAGE_INVALID : GSTD_IMAGE_UNAVAILABLE))
        return 1;
    if (gstd_image_resume(malformed, sizeof(malformed) - 1, -1, 0, &auth, &result) !=
        GSTD_IMAGE_INVALID)
        return 1;
    if (gstd_image_resume(malformed, sizeof(malformed) - 1, 0, 0, &auth, &result) !=
        (gstd_image_backend_available() ? GSTD_IMAGE_INVALID : GSTD_IMAGE_UNAVAILABLE))
        return 1;
    if (gstd_image_import(malformed, sizeof(malformed) - 1, 0, 0, &result) !=
        (gstd_image_backend_available() ? GSTD_IMAGE_INVALID : GSTD_IMAGE_UNAVAILABLE))
        return 1;
    gstd_image_status status =
        gstd_image_stage(malformed, sizeof(malformed) - 1, 0, &auth, &result);
    if (status != (gstd_image_backend_available() ? GSTD_IMAGE_INVALID : GSTD_IMAGE_UNAVAILABLE)) {
        fputs("Unexpected image ABI/build capability result\n", stderr);
        return 1;
    }
    auth.kind = 1;
    if (gstd_image_stage(malformed, sizeof(malformed) - 1, 0, &auth, &result) != GSTD_IMAGE_INVALID)
        return 1;
    const unsigned char plan[] =
        "{\"schemaVersion\":1,\"reference\":\"registry.example/owned/image@sha256:"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"configDigest\":\"sha256:"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"os\":\"linux\",\"architecture\":\"arm64\",\"tokenOrigins\":[],\"blobOrigins\":[],"
        "\"maximumDownloadBytes\":1024,\"maximumMergedBytes\":4096,\"timeoutMs\":1000}";
    auth = (gstd_image_auth){1, (const unsigned char *)":", 1,
                             (const unsigned char *)"synthetic-only", 14};
    status = gstd_image_stage(plan, sizeof(plan) - 1, 0, &auth, &result);
    /* The Rust boundary reads the actual borrowed C auth fields and rejects
     * an invalid Basic username before worker limits, paths or any network. */
    if (status != (gstd_image_backend_available() ? GSTD_IMAGE_INVALID : GSTD_IMAGE_UNAVAILABLE))
        return 1;
    return 0;
}
