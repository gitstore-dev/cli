#ifndef GITSTORED_OWNERSHIP_H
#define GITSTORED_OWNERSHIP_H
#include <sqlite3.h>
/* The directory is the stable canonical volume-lock root, not a deployment root.
 * Retain the returned FD while ownership is held or uncertain. */
int gstd_writer_lock(int directory_fd, const char *volume_id);
typedef int (*gstd_owner_observe)(const char *resource_id, const char *backend_identity,
                                  int *terminated);
/* 1 proven terminated/released, 0 observed running/held, -1 uncertain/error.
 * Observer runs outside transactions and must validate actual backend identity,
 * enumerating by canonical resource identity even when a recorded ID is absent. */
int gstd_ownership_reconcile(sqlite3 *db, const char *resource_id, const char *deployment_id,
                             gstd_owner_observe observer);
#endif
