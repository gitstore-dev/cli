#ifndef GSTD_NETWORK_RESTORE_H
#define GSTD_NETWORK_RESTORE_H
#include "filesystem_job.h"
/* Private filesystem worker: scope is an already opened directory; verify its
 * exact durable owner and live kernel/network/listener identities before passing
 * capabilities. No allocation, signal or network reconfiguration. */
int gstd_network_restore_worker(int scope, const gstd_filesystem_input *, gstd_filesystem_reply *);
/* Parent adopts only the explicit private reply FD set. Failure retains reply
 * ownership. Success clears its descriptor count and transfers all four handles. */
int gstd_network_restore_adopt(gstd_filesystem_reply *, const json_t *expected_identity,
                               gstd_network_worker *, gstd_network_worker_observation *);
#endif
