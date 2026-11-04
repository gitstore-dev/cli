#ifndef GITSTORED_STATE_H
#define GITSTORED_STATE_H
#include <sqlite3.h>
/* Caller owns a verified private directory and manager lifetime lock. */
int gstd_state_open(const char *path, sqlite3 **out);
#endif
