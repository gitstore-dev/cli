#ifndef GSTD_NETWORK_WORKER_H
#define GSTD_NETWORK_WORKER_H
#include "namespace_owner.h"
#include <jansson.h>
typedef struct {
    const json_t *manifest;
    const char *store_id, *resource_id, *operation_id;
    int scope_fd;
} gstd_network_worker_request;
typedef struct {
    pid_t pid;
    uint64_t start, scope_device, scope_inode;
    int channel, pid_fd, scope_fd, permitted, received;
    unsigned roles;
    char store_id[33], resource_id[65], operation_id[65], profile_digest[72];
    char boot_id[37];
} gstd_network_worker;
#define GSTD_NETWORK_WORKER_INITIAL {.channel = -1, .pid_fd = -1, .scope_fd = -1}
typedef struct {
    pid_t pid;
    uint64_t start;
    gstd_network_namespace network; /* borrowed user/net FDs only */
    uint16_t api_port, git_port;
    int api_descriptor, git_descriptor;
    uint64_t api_inode, git_inode;
} gstd_network_worker_observation;
#define GSTD_NETWORK_OBSERVATION_INITIAL {.network = GSTD_NETWORK_NAMESPACE_INITIAL}
/* Parent-only threadless fork boundary. Scope is an already recorded private
 * empty resource directory, never a caller path. This creates a blocked child:
 * commit its returned PID/start/scope identity before granting permission.
 * No namespace, TAP or listener exists before the explicit permit byte. */
gstd_network_status gstd_network_worker_prepare(const gstd_network_worker_request *,
                                                gstd_network_worker *);
int gstd_network_worker_permit(gstd_network_worker *);
/* Nonblocking 0=pending, 1=ready, -1=failed/uncertain. The persistent worker owns
 * packet processing; only namespace FDs cross this private capability boundary.
 * Child death/missing replies never authorize releasing other resources. */
int gstd_network_worker_ready(gstd_network_worker *, gstd_network_worker_observation *);
/* Owned bounded JSON to persist in the parent ledger BEFORE permit. This is an
 * identity, never authority on its own. Reopen requires that ledger identity,
 * the registered request and matching live kernel/namespace/listener evidence. */
json_t *gstd_network_worker_identity(const gstd_network_worker *);
int gstd_network_worker_reopen(const gstd_network_worker_request *, const json_t *,
                               gstd_network_worker *, gstd_network_worker_observation *);
/* Fixed TERM/KILL only. Returns 0=signal accepted/already exited, -1=unknown.
 * Stop attached workloads first; no caller signal number or host PID is accepted. */
int gstd_network_worker_signal(gstd_network_worker *, int force);
void gstd_network_worker_observation_close(gstd_network_worker_observation *);
/* Close this process's handles, never a detached permitted worker. Before permit,
 * EOF aborts the still-blocked child. Durable owner/recovery state remains. */
void gstd_network_worker_close(gstd_network_worker *);
#endif
