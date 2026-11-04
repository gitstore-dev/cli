#ifndef GSTD_NAMESPACE_OWNER_H
#define GSTD_NAMESPACE_OWNER_H
#include "networking.h"
#include <stdint.h>
#include <sys/types.h>
typedef struct gstd_network_namespace {
    pid_t keeper_pid;
    uint64_t keeper_start, user_device, user_inode, network_device, network_inode;
    char boot_id[37];
    unsigned namespace_uid; /* fixed registered single-caller policies: 0 or 10000 */
    int user_fd, network_fd, tap_fd, life_fd, pid_fd;
} gstd_network_namespace;
#define GSTD_NETWORK_NAMESPACE_INITIAL                                                             \
    {.user_fd = -1, .network_fd = -1, .tap_fd = -1, .life_fd = -1, .pid_fd = -1}
/* Private bounded threadless worker, outside control/SQLite transactions. Parent
 * commits the network resource reservation before calling. Creates only one
 * caller-owned user/net namespace, single UID/GID mapping, gs0 TAP and fixed
 * private IPv4 route. No command, host address, path or elevation input.
 * Creator retains the lifeline; its death releases the keeper automatically.
 * Namespace FDs may be borrowed by registered native preparation, never the TAP,
 * lifeline or pidfd. Stop workloads before releasing their owned network. */
gstd_network_status gstd_network_namespace_open(gstd_network_namespace *);
/* Internal registered-policy comparison; no wire-controlled UID or host mapping.
 * Both choices map exactly one namespace UID/GID to the invoking caller. */
gstd_network_status gstd_network_namespace_open_user(gstd_network_namespace *, unsigned);
/* Read-only descriptor/type/owner/mapping/keeper PID-start identity check. Works
 * with a borrowed namespace view whose TAP/lifeline/pidfd are absent. */
int gstd_network_namespace_check(const gstd_network_namespace *);
/* Creator-only bounded close/reap. Unobserved termination retains keeper identity.
 * An observed abnormal exit still fails; neither case releases volume ownership. */
gstd_network_status gstd_network_namespace_close(gstd_network_namespace *);
#endif
