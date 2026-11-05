#ifndef GSTD_CANCELLATION_H
#define GSTD_CANCELLATION_H
#include "deployments.h"
#include <sqlite3.h>
#include <stddef.h>
/* Sole control owner, outside a transaction. Resume running cancellation intent
 * before dispatching another accepted request. Waiting IDs belong to retained
 * actors whose cancellation/cleanup is already in flight. 1=ready,0=none,-1=DB.
 * Also retries terminal control-job bookkeeping without rewriting any outcome. */
int gstd_cancellation_next(sqlite3 *, const char *const *waiting, size_t waiting_count,
                           char operation[65]);
/* A notification cannot interrupt enrollment or an unacknowledged native create.
 * This predicate grants no signaling authority; the manager still requires its
 * exact mapped actor, durable control operation and private eventfd. */
int gstd_cancellation_can_notify(const gstd_deployment *);
#endif
