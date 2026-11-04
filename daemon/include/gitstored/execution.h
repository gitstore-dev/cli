#ifndef GITSTORED_EXECUTION_H
#define GITSTORED_EXECUTION_H
#include <jansson.h>
#include <stdint.h>
/* Internal registered native policy, not caller OCI JSON or a host file path.
 * Returned policy is owned by the caller. Build-time source specialization fixes
 * native architecture, zero capabilities and Linux >=6.8. This alone proves no
 * cgroup/namespace/backend compatibility and advertises no lifecycle method. */
json_t *gstd_execution_policy(void);
const char *gstd_execution_policy_digest(void);
int gstd_execution_kernel_supported(void);
/* Call only after observing prior workload termination and safe owned cleanup.
 * 1=bounded retry, 0=exhausted budget, -1=invalid. Never sleep in the control loop;
 * scheduler owns the returned delay and remains responsive to cancellation. */
int gstd_execution_backoff(unsigned failed_attempts, uint64_t elapsed_ms, unsigned *delay_ms);
#endif
