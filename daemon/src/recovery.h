#ifndef GSTD_RECOVERY_H
#define GSTD_RECOVERY_H
#include <sqlite3.h>
/* Parent-only startup metadata boundary, before dispatch/capabilities. Lost
 * in-memory ownership invalidates readiness and records uncertainty. Terminal
 * outcomes are immutable. Accepted undispatched jobs remain accepted. No native
 * I/O, signal, compensation, reference release or writer release occurs here.
 * Repeated calls are safe; actual native reconciliation follows separately. */
int gstd_recovery_invalidate(sqlite3 *);
#endif
