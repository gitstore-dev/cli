#ifndef GITSTORED_ASSETS_H
#define GITSTORED_ASSETS_H
#include <jansson.h>
/* Immutable registered descriptors only. These functions never inspect/download
 * cache content, resolve credentials, activate assets or accept caller paths. */
json_t *gstd_asset_manifest(const json_t *components, const char *architecture);
/* Private persistent graph dependency resolution. Includes the implicit pinned
 * datastore image plus schema/helper compatibility. No public execution or
 * provisioning capability is enabled by a candidate manifest. Memdb preserves
 * the existing descriptor bytes and never acquires a datastore dependency. */
json_t *gstd_asset_manifest_for_datastore(const json_t *components, const char *architecture,
                                          const char *datastore);
/* Canonical one-image cache descriptor from a registered manifest. Core app
 * descriptors retain existing cache identities across datastore variants. */
json_t *gstd_asset_manifest_component(const json_t *manifest, const char *component);
/* Compare all identities/policies/digests with the current compiled catalogue;
 * a self-consistent hash on arbitrary input is never trust evidence. */
int gstd_asset_manifest_check(const json_t *manifest);
#endif
