#ifndef GSTD_BOOTSTRAP_HTTP_H
#define GSTD_BOOTSTRAP_HTTP_H
#include "bootstrap_material.h"
#include "namespace_owner.h"
typedef enum {
    GSTD_BOOT_API_READY,
    GSTD_BOOT_GIT_READY,
    GSTD_BOOT_CONTROLLER_ENROLL,
    GSTD_BOOT_CONTROLLER_READY
} gstd_bootstrap_http_action;
typedef struct {
    pid_t control_owner;
    const gstd_network_namespace *network; /* matched accepted attachment; borrowed */
    unsigned test_fixture;                 /* compiled out of production, never a wire field */
} gstd_bootstrap_http_context;
typedef enum {
    GSTD_BOOT_HTTP_UNCERTAIN = -2,
    GSTD_BOOT_HTTP_REJECTED = -1,
    GSTD_BOOT_HTTP_READY = 0,
    GSTD_BOOT_HTTP_PENDING = 1
} gstd_bootstrap_http_status;
/* Short-lived, single-threaded private worker only. Enters the owned user/net
 * namespace and drops capabilities before HTTP. Fixed public API/Git paths and
 * internal controller health only; no caller URL, redirects, proxy or raw query.
 * One call per worker. No mutation retry, rotation, file/DB write or raw error
 * response output. uid must initially be empty; a known partial enrollment UID
 * is retained even when the response requires an UNCERTAIN outcome. */
gstd_bootstrap_http_status gstd_bootstrap_http(const gstd_bootstrap_http_context *,
                                               gstd_bootstrap_http_action, const gstd_credential *,
                                               char uid[129]);
#endif
