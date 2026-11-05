#ifndef GSTD_STREAM_SERVICE_H
#define GSTD_STREAM_SERVICE_H
#include "gitstored/protocol.h"
#include "streams.h"
#include <sqlite3.h>
typedef struct gstd_stream_service gstd_stream_service;
gstd_stream_service *gstd_stream_service_open(sqlite3 *, int state_fd);
/* 1=asynchronous subscribe,0=immediate owned result/error,-1=internal failure.
 * At most one unresolved subscribe per connection; server admission enforces it. */
int gstd_stream_service_subscribe(gstd_stream_service *, unsigned connection, const json_t *params,
                                  json_t **result, int *error);
json_t *gstd_stream_service_unsubscribe(gstd_stream_service *, unsigned connection, const char *id,
                                        int *error);
void gstd_stream_service_poll(gstd_stream_service *);
int gstd_stream_service_reply(gstd_stream_service *, unsigned connection, json_t **, int *error);
int gstd_stream_service_ready(const gstd_stream_service *, unsigned connection);
int gstd_stream_service_next(gstd_stream_service *, unsigned connection, json_t **);
void gstd_stream_service_disconnect(gstd_stream_service *, unsigned connection);
void gstd_stream_service_close(gstd_stream_service *);
#endif
