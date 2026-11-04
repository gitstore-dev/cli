#ifndef GITSTORED_IMAGE_BACKEND_H
#define GITSTORED_IMAGE_BACKEND_H
#include <stddef.h>
#include <stdint.h>

/* Internal worker API, never protocol params or a selectable backend command.
 * The caller owns every borrowed buffer/descriptor for the complete call. */
typedef struct gstd_image_auth {
    uint32_t kind; /* 0 anonymous, 1 explicit Basic; no ambient credential lookup */
    const unsigned char *username;
    size_t username_length;
    const unsigned char *secret;
    size_t secret_length;
} gstd_image_auth;
typedef struct gstd_image_result {
    uint64_t downloaded_bytes;
    uint64_t merged_bytes;
} gstd_image_result;
typedef enum {
    GSTD_IMAGE_OK = 0,
    GSTD_IMAGE_INVALID = 1,
    GSTD_IMAGE_UNAVAILABLE = 2,
    GSTD_IMAGE_TRANSFER = 3,
    GSTD_IMAGE_INTEGRITY = 4,
    GSTD_IMAGE_QUOTA = 5,
    GSTD_IMAGE_UNPACK = 6,
    GSTD_IMAGE_PANIC = 7
} gstd_image_status;

/* Availability is build linkage only, never readiness/capability advertisement. */
int gstd_image_backend_available(void);
/* Call only in an isolated native worker with finite memory/file limits.
 * staging_fd is an empty, private, owned directory, duplicated by the library.
 * No activation/DB writes occur. Failure may leave owned partial staging files.
 * The strict bounded non-secret JSON plan is described in image-backend/README.md.
 * Secrets arrive separately from the broker, never in plan/argv/URLs/logs. */
gstd_image_status gstd_image_stage(const unsigned char *plan, size_t plan_length, int staging_fd,
                                   const gstd_image_auth *auth, gstd_image_result *result);
/* Explicit offline source descriptor; no network or credential fallback. */
gstd_image_status gstd_image_import(const unsigned char *plan, size_t plan_length, int source_fd,
                                    int staging_fd, gstd_image_result *result);
/* No HTTP: reconstruct a separate private workload tree from managed cache
 * blobs, never hardlink/copy a mutable shared rootfs into live execution. */
gstd_image_status gstd_image_materialize(const unsigned char *plan, size_t plan_length,
                                         int cached_fd, int staging_fd, gstd_image_result *result);
/* Registry-authorized retry; rehash complete prior blobs, restart partial ones.
 * Source must be recorded owned staging with its producer observed terminated.
 * Fresh target remains separate; never appends bytes without a range validator. */
gstd_image_status gstd_image_resume(const unsigned char *plan, size_t plan_length, int cached_fd,
                                    int staging_fd, const gstd_image_auth *auth,
                                    gstd_image_result *result);
#endif
