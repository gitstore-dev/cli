#ifndef GSTD_FILESYSTEM_JOB_H
#define GSTD_FILESYSTEM_JOB_H
#include "gitstored/asset_cache.h"
#include "network_worker.h"
#include "volumes.h"
#include <stddef.h>
#include <sys/types.h>
typedef enum {
    GSTD_FS_INITIALIZE,
    GSTD_FS_SCOPE,
    GSTD_FS_IMAGE_STAGE,
    GSTD_FS_IMAGE_IMPORT,
    GSTD_FS_IMAGE_MATERIALIZE,
    GSTD_FS_IMAGE_VERIFY,
    GSTD_FS_IMAGE_PROMOTE,
    GSTD_FS_VOLUME,
    GSTD_FS_STAGE_DIRECTORY,
    GSTD_FS_OPEN_ACTIVE,
    GSTD_FS_RESOLVER,
    GSTD_FS_HOST_PROBE,
    GSTD_FS_REOPEN_SCOPE,
    GSTD_FS_OBSERVE_VOLUME,
    GSTD_FS_RECOVER_NETWORK,
    GSTD_FS_REOPEN_ROOTS,
    GSTD_FS_DATASTORE_VOLUME,
    GSTD_FS_OBSERVE_DATASTORE,
    GSTD_FS_OIDC_VOLUME,
    GSTD_FS_OBSERVE_OIDC_VOLUME,
    GSTD_FS_OIDC_PREPARE
} gstd_filesystem_action;
typedef struct {
    gstd_filesystem_action action;
    const json_t *manifest;
    const json_t *scope_expected; /* parent-loaded immutable ledger receipt for reopen */
    const char *component, *store_id, *operation_id, *deployment_id, *resource_id, *resource_kind;
    const char *asset_key, *staging_name, *volume_id, *producer_id;
    uint64_t generation;
    int descriptors[3]; /* action-specific borrowed capabilities, absent = -1 */
    gstd_cache_owner expected_cache;
    gstd_volume_root volume_root;
    gstd_volume_identity volume_expected;
    int volume_existing;
    gstd_oidc_ports oidc_ports;
    const json_t *credential_source;        /* explicit owner settings, never wire paths */
    int credential_prior_required;          /* retained prepared family must match descriptor 2 */
    int credential_initial;                 /* one-shot committed parent reservation */
    gstd_oidc_volume_role oidc_volume_role; /* fixed provider role; OIDC volume actions only */
    gstd_provision_result proof;
    uint64_t download_limit, merge_limit;
    unsigned image_timeout_ms;
    unsigned test_pause; /* testing only: 1 before effects, 2 before reply */
} gstd_filesystem_input;
typedef struct {
    int32_t status, detail;
    unsigned descriptor_count;
    int descriptors[8];
    gstd_provision_result proof;
    gstd_cache_owner cache_owner;
    gstd_volume_root volume_root;
    gstd_volume_identity volume;
    uint64_t scope_device, scope_inode;
    gstd_network_worker network_owner;
    gstd_network_worker_observation network_observed;
} gstd_filesystem_reply;
/* Descriptor order: INITIALIZE cache/cache-lock/volume-root/volume-lock/resource-
 * root/native-state; SCOPE wrapper/data[/bundle]; REOPEN_SCOPE wrapper/data[/bundle/rootfs];
 * VOLUME/DATASTORE_VOLUME data/writer;
 * OBSERVE_VOLUME/OBSERVE_DATASTORE data (no writer authority);
 * STAGE_DIRECTORY new staging directory; OPEN_ACTIVE freshly verified active
 * directory. Owner FD
 * fields are -1; adopt actual received handles explicitly. Other actions return
 * no descriptors. */
typedef struct {
    pid_t worker;
    int channel, interrupted, received;
    unsigned expected_descriptors;
    int64_t deadline_ms;
    gstd_filesystem_reply reply;
} gstd_filesystem_job;
/* Accepted, owner-reserved work only. Never accepts wire paths, commands or OCI
 * options. Initialize job to zero. Parent commits resource intent before launch,
 * retains original capabilities, and validates/persists returned receipts before
 * further effects. All blocking filesystem/SDK work stays outside the control
 * loop and SQLite transactions. Private reply contains no credentials. */
int gstd_filesystem_launch(const gstd_filesystem_input *, unsigned timeout_ms,
                           gstd_filesystem_job *);
/* Nonblocking 0=pending, 1=reaped, -1=invalid. Deadline/crash/cancel/lost reply is
 * status -2 (uncertain), never rollback or permission to release a resource.
 * Cancellation FD is borrowed and unconsumed. On success the caller owns exactly
 * descriptor_count handles and must close/adopt them. */
int gstd_filesystem_poll(gstd_filesystem_job *, int cancellation_fd, gstd_filesystem_reply *);
void gstd_filesystem_reply_close(gstd_filesystem_reply *);
#endif
