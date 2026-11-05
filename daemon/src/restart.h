#ifndef GSTD_RESTART_H
#define GSTD_RESTART_H
#include "gitstored/protocol.h"
#include <sqlite3.h>
#include <stdint.h>
typedef struct {
    char deployment[65], current[65], stop[65];
    json_t *intent;
    /* Four replacement attempts per manager-owned chain, with 250/500/1000/2000
     * ms nonblocking delays. Ordinary stop/start deadlines remain independent. */
    unsigned attempts, delayed_ms;
    int phase; /* 1=watch, 2=stop, 3=delay, 4=replacement, 5=ended */
    int64_t due_ms;
} gstd_restart;
typedef int (*gstd_restart_submit)(void *, gstd_method, json_t *, const char *, char[65]);
/* Sole manager thread. Begin only after exact kernel exit observation from its
 * current, previously ready actor. Metadata alone never authorizes this call.
 * No native effect, wait or ownership release is performed by this module. */
int gstd_restart_departed(sqlite3 *, const char *start, gstd_restart *, gstd_restart_submit,
                          void *);
/* Nonblocking; separate normal acceptance implements all actual mutations.
 * Lost manager memory ends supervision: recovered actors cannot call departed.
 * Terminal history stays in the ordinary durable operation/dedupe records. */
void gstd_restart_poll(sqlite3 *, gstd_restart *, int64_t now_ms, gstd_restart_submit, void *);
void gstd_restart_clear(gstd_restart *);
#endif
