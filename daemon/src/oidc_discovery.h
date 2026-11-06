#ifndef GSTD_OIDC_DISCOVERY_H
#define GSTD_OIDC_DISCOVERY_H
#include "oidc_config.h"
/* Pure pinned consumer predicates. No response-supplied URL is authority for a
 * request. JWKS checks the public RSA signing-key envelope, not a token signature. */
int gstd_oidc_discovery_matches(const json_t *, const gstd_oidc_ports *);
int gstd_oidc_jwks_matches(const json_t *);
#endif
