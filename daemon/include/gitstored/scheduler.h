#ifndef GITSTORED_SCHEDULER_H
#define GITSTORED_SCHEDULER_H
#include <sqlite3.h>
/* Called inside the same durable acceptance transaction, before acknowledgment. */
int gstd_schedule_reserve(sqlite3 *db, const char *operation_id, int control);
/* Returns 1 job, 0 none, -1 storage/validation error. No backend I/O under DB locks. */
int gstd_schedule_take(sqlite3 *db, int control, char operation_id[65]);
int gstd_schedule_finish(sqlite3 *db, const char *operation_id);
/* Read-only supervision of the current successful start. Borrows one existing
 * runtime slot on its internal job row without changing terminal operation
 * history. 1=reserved,0=busy/stale,-1=storage failure. Never queue/steal a slot or
 * reserve while an explicit lifecycle operation for that deployment is active. */
int gstd_schedule_observer_reserve(sqlite3 *db, const char *operation_id);
/* Bounded log replay shares the same eight slots, at most two read leases.
 * Reserve before fork; release only after the private reader is reaped. Closing
 * a subscription never releases an in-flight worker early. Startup recovery is
 * called only by the manager holding this store's lifetime lock. */
int gstd_schedule_read_reserve(sqlite3 *, const char *subscription, const char *deployment);
int gstd_schedule_read_finish(sqlite3 *, const char *subscription);
int gstd_schedule_read_recover(sqlite3 *);
#endif
