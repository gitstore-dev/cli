#ifndef GSTD_NETWORK_SANDBOX_H
#define GSTD_NETWORK_SANDBOX_H
#include <jansson.h>
/* Owned policy: a strict reduction of the compiled maintained application
 * policy. No caller policy, paths, syscall names or flags are accepted. */
json_t *gstd_network_worker_policy(void);
/* Private, single-threaded setup only, before processing a single TAP packet.
 * scope is a validated empty owned resource directory. Leaves no external
 * directory descriptor except the caller's scope, which must then be closed. */
int gstd_network_sandbox(int scope);
#endif
