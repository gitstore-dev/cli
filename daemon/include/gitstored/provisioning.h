#ifndef GITSTORED_PROVISIONING_H
#define GITSTORED_PROVISIONING_H
#include "gitstored/credentials.h"
#include <jansson.h>
#include <stdint.h>
typedef struct gstd_image_auth gstd_image_auth;
typedef struct {
    uint64_t downloaded_bytes, merged_bytes, logical_bytes;
    uint64_t staging_device, staging_inode;
    char tree_digest[65];
} gstd_provision_result;
/* Owner-configured request only: exact registered registry/repository and pull
 * purpose are checked before broker invocation. Material is username:password;
 * no anonymous fallback. 0=resolved, -1=invalid scope/material, -2=broker failure.
 * Outputs initially zero; auth borrows owner until gstd_credential_clear(owner). */
int gstd_provision_registry_auth(const json_t *manifest, const char *component,
                                 int installed_broker_fd, const unsigned char *request,
                                 size_t request_length, unsigned timeout_ms, gstd_credential *owner,
                                 gstd_image_auth *auth);
typedef enum {
    GSTD_PROVISION_OK = 0,
    GSTD_PROVISION_INVALID = 1,
    GSTD_PROVISION_UNAVAILABLE = 2,
    GSTD_PROVISION_TRANSFER = 3,
    GSTD_PROVISION_INTEGRITY = 4,
    GSTD_PROVISION_QUOTA = 5,
    GSTD_PROVISION_UNPACK = 6,
    GSTD_PROVISION_PANIC = 7,
    GSTD_PROVISION_DEADLINE = 8,
    GSTD_PROVISION_CANCELLED = 9,
    GSTD_PROVISION_WORKER = 10
} gstd_provision_status;
/* Build only from a currently registered immutable component descriptor. */
json_t *gstd_provision_plan(const json_t *manifest, const char *component, uint64_t download_limit,
                            uint64_t merge_limit, unsigned timeout_ms);
/* Blocking supervisor, never call on the control request loop or under DB locks.
 * Parent must reserve disk/job ownership before calling. This checks current free
 * space but does not replace a durable/global reservation. Staging FD is borrowed.
 * Optional borrowed cancellation FD (-1 absent) becoming readable aborts the worker
 * without consuming the signal. No asset activation or state writes occur here.
 * Linux only; worker has finite memory/files, closed unrelated FDs, parent-death
 * handling and a monotonic deadline. Failure can leave owned partial staging.
 * Caller retains the staging lock/FD through observed worker termination/cleanup. */
gstd_provision_status gstd_provision_stage(const json_t *manifest, const char *component,
                                           int staging_fd, const gstd_image_auth *auth,
                                           uint64_t download_limit, uint64_t merge_limit,
                                           unsigned timeout_ms, int cancellation_fd,
                                           gstd_provision_result *result);
/* Explicit owner-selected OCI-layout directory, borrowed and pinned by descriptor.
 * Same worker/ownership/reservation contract as stage; never falls back to HTTP.
 * Source is input only and cannot nominate a different registered image. */
gstd_provision_status gstd_provision_import(const json_t *manifest, const char *component,
                                            int source_fd, int staging_fd, uint64_t download_limit,
                                            uint64_t merge_limit, unsigned timeout_ms,
                                            int cancellation_fd, gstd_provision_result *result);
/* Registry-authorized retry from a recorded, stopped producer's staging root.
 * Caller must bind unchanged profile/source/limits/consent before dispatch and
 * reserve a separate empty destination. Complete blobs are rehashed and copied;
 * missing/truncated blobs restart through the maintained registry client. Never
 * use this entry point for a no-network/offline-only authorization. */
gstd_provision_status gstd_provision_resume(const json_t *manifest, const char *component,
                                            int cached_fd, int staging_fd,
                                            const gstd_image_auth *auth, uint64_t download_limit,
                                            uint64_t merge_limit, unsigned timeout_ms,
                                            int cancellation_fd, gstd_provision_result *result);
/* No-network private workload materialization, under the same worker limits.
 * Parent holds the managed cache reference and reserves an independent workload
 * tree. Missing/corrupt cached blobs fail; never uses registry credentials or HTTP.
 * The shared verified cache root is never used as a mutable live container root. */
gstd_provision_status gstd_provision_materialize(const json_t *manifest, const char *component,
                                                 int cached_fd, int staging_fd,
                                                 uint64_t download_limit, uint64_t merge_limit,
                                                 unsigned timeout_ms, int cancellation_fd,
                                                 gstd_provision_result *result);
#endif
