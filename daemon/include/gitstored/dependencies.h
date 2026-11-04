#ifndef GITSTORED_DEPENDENCIES_H
#define GITSTORED_DEPENDENCIES_H
#include <jansson.h>
/* Actual linked-code/build observations. No generic PATH/dlopen/provider scan.
 * Unselected external implementations remain unavailable until a registered
 * verifier supplies trusted identity/platform/ABI evidence. */
json_t *gstd_dependency_resolutions(void);
int gstd_dependency_resolutions_check(const json_t *resolutions);
/* Observe already-present upstream manifest bytes, never a native executable.
 * The descriptor must match the compiled catalogue. FD is borrowed, read-only,
 * regular, singly linked and trusted-owner/mode; no caller path is reopened.
 * origin is host/cache. A successful snapshot verifies only these manifest
 * bytes, not layers/config/rootfs readiness; consumers recheck the same FD. */
json_t *gstd_dependency_asset_fd(const json_t *manifest, const char *component, int fd,
                                 const char *origin);
int gstd_dependency_asset_fd_check(const json_t *observed, const json_t *manifest,
                                   const char *component, int fd, const char *origin);
#endif
