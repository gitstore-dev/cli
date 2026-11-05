#ifndef GSTD_LOG_READ_H
#define GSTD_LOG_READ_H
#include "logs.h"
#include <sqlite3.h>
#include <sys/types.h>
typedef struct {
    int state_fd;
    const char *store_id, *subscription_id, *deployment_id, *component, *cursor;
    gstd_stream_kind kind;
    unsigned limit;
    int64_t now;
} gstd_log_read_input;
typedef struct {
    pid_t worker;
    int channel, reaped, abandoned, complete, exit_ok, exit_status;
    int64_t deadline_ms;
    size_t received, expected;
    unsigned char header[8], *bytes;
    char subscription[65];
    gstd_log_status status;
    json_t *records;
} gstd_log_read;
#define GSTD_LOG_READ_INITIAL {.channel = -1}
/* Returns 1=started,0=shared worker budget busy,-1=invalid/storage/fork error.
 * Reserves before fork. The private process has no broker/repository capability.
 * Non-Linux hosts explicitly reject execution. */
int gstd_log_read_launch(sqlite3 *, const gstd_log_read_input *, gstd_log_read *);
/* Returns 0=pending,1=finished,-1=lease-release storage failure (retry).
 * A finished result owns records until close. Slot release follows waitpid. */
int gstd_log_read_poll(sqlite3 *, gstd_log_read *);
/* Disconnect only abandons this observation; never touches a workload or an
 * operation. Keep polling until reaped and its durable read lease released. */
void gstd_log_read_abandon(gstd_log_read *);
void gstd_log_read_close(gstd_log_read *);
#endif
