#ifndef GSTD_OIDC_PROFILE_H
#define GSTD_OIDC_PROFILE_H
#include <jansson.h>
/* Fixed reference inputs for the owned core/2 lifecycle. These factories do not
 * grant asset provisioning or bypass installation/host/ownership checks. */
int gstd_oidc_component(const char *);
json_t *gstd_oidc_manifest(const char *architecture);
json_t *gstd_oidc_manifest_component(const char *architecture, const char *component);
/* Core/2 composition. Core/1 descriptors/digests and per-image cache
 * identities are preserved. This factory alone does not activate serving. */
const char *gstd_oidc_core_profile_digest(void);
json_t *gstd_oidc_core_components(const json_t *requested);
json_t *gstd_oidc_core_manifest(const json_t *components, const char *architecture,
                                const char *datastore);
int gstd_oidc_core_manifest_check(const json_t *);
int gstd_oidc_manifest_check(const json_t *);
#endif
