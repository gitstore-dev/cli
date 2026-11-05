#ifndef GSTD_NETWORKING_H
#define GSTD_NETWORKING_H
#include <stdint.h>
typedef struct gstd_network_engine gstd_network_engine;
typedef enum {
    GSTD_FORWARD_API,
    GSTD_FORWARD_GIT,
    GSTD_FORWARD_OIDC_ISSUER,
    GSTD_FORWARD_OIDC_BRIDGE,
    GSTD_FORWARD_OIDC_IDENTITY,
    GSTD_FORWARD_OIDC_UI,
    GSTD_FORWARD_MAIL_UI,
    GSTD_FORWARD_ROLE_COUNT
} gstd_forward_role;
typedef enum {
    GSTD_NETWORK_OK,
    GSTD_NETWORK_CONTROL,
    GSTD_NETWORK_INVALID,
    GSTD_NETWORK_UNAVAILABLE,
    GSTD_NETWORK_HOST_UNMET,
    GSTD_NETWORK_FAILED
} gstd_network_status;
/* Private single-threaded worker only, never the control loop or wire API.
 * Parent must commit owned namespace/TAP/forward reservations before effects.
 * Borrowed nonblocking gs0 TAP comes from the recorded private namespace;
 * namespace preparation, authenticated IPC and durable supervision are separate.
 * No arbitrary host address, guest destination, port, path or executable input.
 * CORE=0, AS<=512MiB (except instrumented ASan), FSIZE<=64MiB required. */
int gstd_network_engine_available(void);
gstd_network_status gstd_network_open(int tap_fd, gstd_network_engine **);
gstd_network_status gstd_network_forward(gstd_network_engine *, gstd_forward_role, uint16_t *port);
/* Private optional-profile worker only. Fixed public OIDC roles use the same
 * selected unprivileged port on host and guest so issuer/browser URLs match.
 * Core and private admin/SMTP ports are rejected. A bind conflict fails; it
 * never substitutes a new issuer port or destination. No arbitrary IP input.
 * The profile worker must journal all resulting listener identities before use. */
gstd_network_status gstd_network_forward_bound(gstd_network_engine *, gstd_forward_role,
                                               uint16_t selected_port, uint16_t *port);
/* Private borrowed descriptor identity for the durable owner snapshot. Never
 * close/transfer this library-owned listener or expose it through the wire API. */
int gstd_network_listener(gstd_network_engine *, gstd_forward_role, int *descriptor,
                          uint64_t *inode);
/* Poll at most 50ms. Optional control descriptor (-1 absent) is never consumed.
 * At most 512 poll descriptors / 64 timers / 64 timer callbacks per step. */
gstd_network_status gstd_network_step(gstd_network_engine *, int control_fd, unsigned maximum_ms);
/* Library cleanup and close only this instance's retained descriptors. A failure
 * is not permission to release durable ownership; observe the helper's exit.
 * Never fork with a live engine and run cleanup in both copies. */
gstd_network_status gstd_network_close(gstd_network_engine *);
#endif
