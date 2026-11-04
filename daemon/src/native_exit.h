#ifndef GSTD_NATIVE_EXIT_H
#define GSTD_NATIVE_EXIT_H
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    int exited, signalled, code;
} gstd_native_exit;
/* Parent control owner only; must be the workload's reaper/subreaper. Polls one
 * recorded PID plus kernel start time. Does not reap a different/reused child.
 * 0=not exited, 1=collected, -1=invalid, -2=unknown/not ours. Unknown is never
 * termination proof or permission to release a volume reservation. */
int gstd_native_exit_poll(pid_t, uint64_t start_time, gstd_native_exit *);
/* Internal kernel identity observation, not authorization by PID alone. */
int gstd_native_process_start(pid_t, uint64_t *);
/* Persist with durable identities: PIDs/ticks can repeat across host reboots. */
int gstd_native_boot_id(char output[37]);
/* Read-only recovery prerequisite for an init which a restarted manager cannot
 * reap. Open binds same-UID PID, kernel start time and creator boot before/after
 * pidfd_open. A held pidfd reports only that exact process death, including after
 * its real parent reaps it. This never proves cgroup emptiness or writer release.
 * Initialize descriptor to -1; no wire caller PID or signal operation exists. */
int gstd_native_watch_open(pid_t, uint64_t start_time, const char *boot, int *descriptor);
/* 0=still live, 1=held original process exited, -1=unknown/invalid. */
int gstd_native_watch_poll(int descriptor);
#endif
