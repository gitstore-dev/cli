#ifndef GITSTORED_PROFILES_H
#define GITSTORED_PROFILES_H
#include <jansson.h>
/* Immutable metadata compiled from the distribution, never a caller path. */
json_t *gstd_profile(void);
int gstd_profile_check(const json_t *profile);
const char *gstd_profile_digest(void);
/* Pure registered core dependency closure. Omitted selection means all core;
 * explicit selection must be nonempty/unique/known. API and Git require each
 * other; controller is included only when selected. Never mutates input/order
 * or the semantic idempotency fingerprint of the original selection. */
json_t *gstd_profile_components(const json_t *requested);
/* Registered version-aware factories. Core/1 is frozen; core/2 optionally closes
 * the fixed OIDC graph. These metadata functions never grant native authority. */
json_t *gstd_profile_components_for(const char *version, const json_t *requested);
json_t *gstd_profile_manifest(const char *version, const json_t *components,
                              const char *architecture, const char *datastore);
const char *gstd_profile_digest_for(const char *version);
#endif
