#ifndef GSTD_OIDC_CLIENT_H
#define GSTD_OIDC_CLIENT_H
#include "gitstored/credentials.h"
#include <jansson.h>
typedef struct {
    const char *client_id, *audience, *redirect_uri;
} gstd_oidc_client;
/* T050 private provider integration building block; no enabled wire capability.
 * Fixed reference client policy only. IDs are bounded identifiers; callback is
 * numeric IPv4 loopback, an unprivileged port, and the fixed /callback path. */
int gstd_oidc_client_check(const gstd_oidc_client *);
/* Private credential worker only. Returned JSON contains sensitive material;
 * never send it on parent IPC, log it or persist it as operation intent/result. */
json_t *gstd_oidc_client_request(const gstd_oidc_client *, const gstd_credential *secret);
/* 1=matching public configuration, 0=mismatch/invalid. With secret, validate the
 * sensitive successful-create echo; without it, accept only an absent/empty
 * secret in a GET. Public matching alone never authorizes adoption after a lost
 * create acknowledgment. Other provider credential fields are not projected. */
int gstd_oidc_client_matches(const gstd_oidc_client *, const json_t *,
                             const gstd_credential *creation_secret);
#endif
