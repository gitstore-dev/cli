#ifndef GSTD_RUNTIME_SETTINGS_H
#define GSTD_RUNTIME_SETTINGS_H
#include <jansson.h>
typedef struct {
    json_t *credential_source;
    int credential_parent_fd;
    int offline_fds[3]; /* api, git, controller; borrowed by managed workers */
} gstd_runtime_settings;
#define GSTD_RUNTIME_SETTINGS_INITIAL {.credential_parent_fd = -1, .offline_fds = {-1, -1, -1}}
/* Read-only owner configuration from an already verified private state FD.
 * Missing settings uses the OS store and no offline inputs. Unknown/unsafe
 * settings fail before any credential lookup, directory creation or download.
 * All paths are owner inputs, never lifecycle wire parameters. Initialize with
 * the macro and clear only successfully loaded/initialized settings. */
int gstd_runtime_settings_load(int state_fd, gstd_runtime_settings *);
void gstd_runtime_settings_clear(gstd_runtime_settings *);
/* Resolve only bootstrap material. Explicit protected-directory policy derives
 * a distinct broker file from installation/issuer/deployment/purpose identity;
 * never share one literal file across accounts. Returns an owned broker source,
 * not secret material; pinned directory identity must still match its name. */
json_t *gstd_runtime_settings_source(const gstd_runtime_settings *, const char *store_id,
                                     const char *deployment_id);
#endif
