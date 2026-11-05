#ifndef GSTD_RUNTIME_MANAGER_H
#define GSTD_RUNTIME_MANAGER_H
#include "gitstored/protocol.h"
#include "runtime_settings.h"
#include <sqlite3.h>
typedef struct gstd_runtime_manager gstd_runtime_manager;
/* Read-only installed sibling broker selection and library availability. No
 * provisioning or workload effects. Missing support leaves observation usable. */
gstd_runtime_manager *gstd_runtime_manager_open(sqlite3 *, int state_fd,
                                                const gstd_runtime_settings *,
                                                const char *entrypoint);
int gstd_runtime_manager_capable(const gstd_runtime_manager *);
/* Read-only cache/offline candidates; never claims content verification. */
int gstd_runtime_manager_assess(gstd_runtime_manager *, json_t *assessment);
/* Result/error shape is the versioned daemon contract. Accepted intent is
 * committed before returning; work continues independently of the connection. */
json_t *gstd_runtime_manager_submit(gstd_runtime_manager *, const gstd_request *, int *error);
void gstd_runtime_manager_poll(gstd_runtime_manager *);
int gstd_runtime_manager_observation_blocked(const gstd_runtime_manager *, const char *deployment,
                                             const char *operation);
/* Manager shutdown closes only parent capabilities; never claims owned stop or
 * releases a surviving writer. Running resource recovery remains durable. */
void gstd_runtime_manager_close(gstd_runtime_manager *);
#endif
