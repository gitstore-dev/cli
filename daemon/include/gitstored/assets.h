#ifndef GITSTORED_ASSETS_H
#define GITSTORED_ASSETS_H
#include <jansson.h>
/* Immutable registered descriptors only. These functions never inspect/download
 * cache content, resolve credentials, activate assets or accept caller paths. */
json_t *gstd_asset_manifest(const json_t *components, const char *architecture);
/* Compare all identities/policies/digests with the current compiled catalogue;
 * a self-consistent hash on arbitrary input is never trust evidence. */
int gstd_asset_manifest_check(const json_t *manifest);
#endif
