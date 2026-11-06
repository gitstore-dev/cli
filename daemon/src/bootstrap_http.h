#ifndef GSTD_BOOTSTRAP_HTTP_H
#define GSTD_BOOTSTRAP_HTTP_H
#include "bootstrap_material.h"
#include "namespace_owner.h"
#include "oidc_client.h"
#include "oidc_config.h"
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
typedef enum {
    GSTD_OIDC_HTTP_READY,
    GSTD_OIDC_HTTP_CREATE,
    GSTD_OIDC_HTTP_INSPECT
} gstd_oidc_http_action;
typedef enum {
    GSTD_OIDC_UNCERTAIN = -2,
    GSTD_OIDC_REJECTED = -1,
    GSTD_OIDC_PENDING = 0,
    GSTD_OIDC_PROVIDER_READY = 1,
    GSTD_OIDC_CLIENT_CREATED = 2,
    GSTD_OIDC_PUBLIC_MATCH = 3
} gstd_oidc_http_status;
/* Fixed private service roles.  These are deliberately not component names or
 * caller-supplied URLs: the worker can only contact the pinned loopback routes
 * below after entering the deployment's owned namespace. */
typedef enum {
    GSTD_OIDC_HEALTH_HYDRA,
    GSTD_OIDC_HEALTH_KRATOS,
    GSTD_OIDC_HEALTH_BRIDGE,
    GSTD_OIDC_HEALTH_UI,
    GSTD_OIDC_HEALTH_SMTP,
    GSTD_OIDC_HEALTH_ISSUER
} gstd_oidc_health_role;
typedef enum {
    GSTD_OIDC_HEALTH_REJECTED = -1,
    GSTD_OIDC_HEALTH_PENDING = 0,
    GSTD_OIDC_HEALTH_READY = 1
} gstd_oidc_health_status;
/* T050 private worker building block, not an enabled optional profile. Fixed
 * Hydra admin loopback endpoint inside the owned namespace, no proxy/redirect.
 * Parent must journal dispatch before CREATE; one attempt, no automatic retry.
 * Only a matching 201 acknowledges CREATE; any other dispatched result remains
 * uncertain. INSPECT only matches public fields and is never proof of secret
 * ownership after a lost CREATE reply. No raw response or secret enters IPC. */
gstd_oidc_http_status gstd_oidc_http(const gstd_bootstrap_http_context *, gstd_oidc_http_action,
                                     const gstd_oidc_client *,
                                     const gstd_credential *creation_secret);
/* One bounded, read-only health attempt. Hydra preserves its source-verified
 * JSON {"status":"ok"} predicate. The other pinned consumers accept only an
 * HTTP 200 and discard their bodies without assigning a shared JSON schema or
 * interpreting failure status codes. No redirect, proxy or caller URL is used. */
gstd_oidc_health_status gstd_oidc_health(const gstd_bootstrap_http_context *,
                                         gstd_oidc_health_role, const gstd_oidc_ports *);
#endif
