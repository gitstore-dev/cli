#ifndef GSTD_MONITOR_H
#define GSTD_MONITOR_H
#include "deployments.h"
/* Current successful start only. Read-only fixed health actions share the eight
 * runtime slots. One five-second worker at a time per deployment; actions rotate
 * every five seconds and readiness expires after 30 seconds without fresh proof.
 * No enrollment, provisioning, restart, signal, owner release or terminal rewrite. */
int gstd_deployment_monitor_poll(gstd_deployment *);
/* A failed persistence update or pending start finalization cannot leave stale
 * ready observable. Optional
 * selectors isolate deployments/operations; NULL selectors match all. Clear only
 * after an actual successful observation transaction, not an idle poll. */
int gstd_deployment_monitor_observation_blocked(const gstd_deployment *, const char *deployment,
                                                const char *operation);
#endif
