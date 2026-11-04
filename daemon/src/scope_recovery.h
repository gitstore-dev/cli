#ifndef GSTD_SCOPE_RECOVERY_H
#define GSTD_SCOPE_RECOVERY_H
#include "filesystem_job.h"
/* Private filesystem worker only. Reopen exact ledger-bound directories without
 * creation or repair. Workload returns wrapper/execution/bundle/rootfs; config
 * returns wrapper/data. Network returns only its ledger-bound data FD and no
 * wrapper cleanup/process control authority. Caller still must validate live
 * native/network identities. */
int gstd_scope_reopen(int resource_root, const gstd_filesystem_input *, gstd_filesystem_reply *);
#endif
