#include "image_backend.h"
#include <string.h>

#ifdef GITSTORED_IMAGE_SDK
extern int gitstored_image_stage_v1(const unsigned char *, size_t, int, const gstd_image_auth *,
                                    gstd_image_result *);
extern int gitstored_image_resume_v1(const unsigned char *, size_t, int, int,
                                     const gstd_image_auth *, gstd_image_result *);
extern int gitstored_image_materialize_v1(const unsigned char *, size_t, int, int,
                                          gstd_image_result *);
extern int gitstored_image_import_v1(const unsigned char *, size_t, int, int, gstd_image_result *);
#endif
int gstd_image_backend_available(void) {
#ifdef GITSTORED_IMAGE_SDK
    return 1;
#else
    return 0;
#endif
}
static gstd_image_status from_source(const unsigned char *plan, size_t length, int source_fd,
                                     int staging_fd, gstd_image_result *result, int cached) {
    if (!result)
        return GSTD_IMAGE_INVALID;
    memset(result, 0, sizeof(*result));
    if (!plan || !length || length > 65536 || source_fd < 0 || staging_fd < 0)
        return GSTD_IMAGE_INVALID;
#ifdef GITSTORED_IMAGE_SDK
    int status = cached
                     ? gitstored_image_materialize_v1(plan, length, source_fd, staging_fd, result)
                     : gitstored_image_import_v1(plan, length, source_fd, staging_fd, result);
    if (status < GSTD_IMAGE_OK || status > GSTD_IMAGE_PANIC) {
        memset(result, 0, sizeof(*result));
        return GSTD_IMAGE_UNAVAILABLE;
    }
    return (gstd_image_status)status;
#else
    (void)cached;
    return GSTD_IMAGE_UNAVAILABLE;
#endif
}
gstd_image_status gstd_image_import(const unsigned char *plan, size_t length, int source_fd,
                                    int staging_fd, gstd_image_result *result) {
    return from_source(plan, length, source_fd, staging_fd, result, 0);
}
gstd_image_status gstd_image_materialize(const unsigned char *plan, size_t length, int source_fd,
                                         int staging_fd, gstd_image_result *result) {
    return from_source(plan, length, source_fd, staging_fd, result, 1);
}
gstd_image_status gstd_image_stage(const unsigned char *plan, size_t length, int staging_fd,
                                   const gstd_image_auth *auth, gstd_image_result *result) {
    if (!result)
        return GSTD_IMAGE_INVALID;
    memset(result, 0, sizeof(*result));
    if (!plan || !length || length > 65536 || staging_fd < 0 || !auth || auth->kind > 1 ||
        auth->username_length > 4096 || auth->secret_length > 65536 ||
        (auth->username_length && !auth->username) || (auth->secret_length && !auth->secret) ||
        (!auth->kind && (auth->username_length || auth->secret_length)) ||
        (auth->kind && (!auth->username_length || !auth->secret_length)))
        return GSTD_IMAGE_INVALID;
#ifdef GITSTORED_IMAGE_SDK
    int status = gitstored_image_stage_v1(plan, length, staging_fd, auth, result);
    if (status < GSTD_IMAGE_OK || status > GSTD_IMAGE_PANIC) {
        memset(result, 0, sizeof(*result));
        return GSTD_IMAGE_UNAVAILABLE;
    }
    return (gstd_image_status)status;
#else
    return GSTD_IMAGE_UNAVAILABLE;
#endif
}

gstd_image_status gstd_image_resume(const unsigned char *plan, size_t length, int cached_fd,
                                    int staging_fd, const gstd_image_auth *auth,
                                    gstd_image_result *result) {
    if (!result)
        return GSTD_IMAGE_INVALID;
    memset(result, 0, sizeof(*result));
    if (!plan || !length || length > 65536 || cached_fd < 0 || staging_fd < 0 || !auth ||
        auth->kind > 1 || auth->username_length > 4096 || auth->secret_length > 65536 ||
        (auth->username_length && !auth->username) || (auth->secret_length && !auth->secret) ||
        (!auth->kind && (auth->username_length || auth->secret_length)) ||
        (auth->kind && (!auth->username_length || !auth->secret_length)))
        return GSTD_IMAGE_INVALID;
#ifdef GITSTORED_IMAGE_SDK
    int status = gitstored_image_resume_v1(plan, length, cached_fd, staging_fd, auth, result);
    if (status < GSTD_IMAGE_OK || status > GSTD_IMAGE_PANIC) {
        memset(result, 0, sizeof(*result));
        return GSTD_IMAGE_UNAVAILABLE;
    }
    return (gstd_image_status)status;
#else
    return GSTD_IMAGE_UNAVAILABLE;
#endif
}
