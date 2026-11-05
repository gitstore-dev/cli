#ifndef GSTD_OIDC_ENROLLMENT_H
#define GSTD_OIDC_ENROLLMENT_H
#include "bootstrap_http.h"
/* Pure, public-only journal policy. Caller must bind its configuration/volume/
 * credential reservations and exact live provider before using it. A journal is
 * never execution, credential ownership or volume adoption authority.
 * Each returned object is newly owned. Persist prepare, then persist dispatch
 * BEFORE the single HTTP mutation. Never replace a prior record with prepare.
 * Lost commit/reply after dispatch is uncertain and does not authorize retry. */
json_t *gstd_oidc_enrollment_prepare(const json_t *binding, const gstd_oidc_client *);
json_t *gstd_oidc_enrollment_dispatch(const json_t *);
json_t *gstd_oidc_enrollment_observe(const json_t *, gstd_oidc_http_status);
json_t *gstd_oidc_enrollment_recover(const json_t *);
/* A later generation may resume a known never-dispatched preparation or retain
 * acknowledged creation only for the same store,
 * deployment, immutable volume producer/inodes, credential reference and public
 * client mapping. New operation/provider identities and a higher generation are
 * required. The parent must prove released source ownership, retained material,
 * writer exclusion and current provider readiness separately before committing
 * this record. A prepared record still requires committed dispatch before its
 * first POST; a created record never permits another POST. This never turns a
 * public lookup or uncertain POST into creation or another attempt. */
json_t *gstd_oidc_enrollment_resume(const json_t *, const json_t *binding,
                                    const gstd_oidc_client *);
/* Strict public record validation and exact comparison before effects. */
int gstd_oidc_enrollment_matches(const json_t *, const json_t *binding, const gstd_oidc_client *);
#endif
