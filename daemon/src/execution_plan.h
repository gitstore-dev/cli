#ifndef GSTD_EXECUTION_PLAN_H
#define GSTD_EXECUTION_PLAN_H
#include <jansson.h>
typedef struct gstd_network_namespace gstd_network_namespace;
typedef enum {
    GSTD_MOUNT_BASE,
    GSTD_MOUNT_OVERLAY,
    GSTD_MOUNT_POLICY,
    GSTD_MOUNT_USERS,
    GSTD_MOUNT_SIGNING_RECORD,
    GSTD_MOUNT_REPOSITORIES,
    GSTD_MOUNT_RESOLVER,
    GSTD_MOUNT_DATASTORE,
    GSTD_MOUNT_COUNT
} gstd_mount_role;
typedef enum {
    GSTD_EXEC_SERVICE,
    GSTD_EXEC_SCYLLA_READY,
    GSTD_EXEC_SCYLLA_KEYSPACE,
    GSTD_EXEC_SCHEMA_MIGRATE,
    GSTD_EXEC_SCHEMA_INSPECT,
    GSTD_EXEC_SCHEMA_HISTORY,
    GSTD_EXEC_OIDC_MIGRATE
} gstd_execution_purpose;
/* Generated private configuration files / recorded named volume only. Absent
 * roles must be -1. No caller executable, mount target, environment or OCI JSON.
 * API issuer material belongs only in its private overlay, never the shared base.
 * Parent retains these descriptors/ownership while native preparation runs. */
typedef struct gstd_execution_inputs {
    const json_t *manifest;
    const char *component;
    gstd_execution_purpose purpose; /* fixed registered helper or service, never argv */
    int rootfs_fd;
    int role_fds[GSTD_MOUNT_COUNT];
    const gstd_network_namespace *network;
    const struct gstd_oidc_ports *oidc_ports; /* fixed public metadata only; optional adapter */
} gstd_execution_inputs;
/* Pure plan construction/metadata validation. No download, mount, process launch,
 * secret parsing or durable write. Image authenticity/private materialization and
 * canonical-volume leases must already be proven by their recorded owners.
 * Network is initially isolated; T041 attaches only an owned network namespace. */
json_t *gstd_execution_build(const gstd_execution_inputs *);
#endif
