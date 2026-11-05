#ifndef GSTD_STREAMS_H
#define GSTD_STREAMS_H
#include <jansson.h>
#include <stddef.h>
#include <stdint.h>
#define GSTD_STREAM_CONNECTIONS 32u
#define GSTD_STREAM_SUBSCRIPTIONS 8u
#define GSTD_STREAM_EVENTS 256u
#define GSTD_STREAM_BYTES (4u * 1024u * 1024u)
#define GSTD_STREAM_GLOBAL_BYTES (64u * 1024u * 1024u)
typedef struct gstd_streams gstd_streams;
typedef enum { GSTD_STREAM_LOGS, GSTD_STREAM_EVENTS_KIND } gstd_stream_kind;
/* Sole control owner. No DB, filesystem retention, workload effects or secret
 * access. The caller verifies deployment/component existence and cursor replay
 * before subscribing. Only already-redacted payloads may enter these queues. */
gstd_streams *gstd_streams_open(void);
void gstd_streams_close(gstd_streams *);
/* 0=created,-1=invalid,-2=quota/OOM. ID belongs only to this connection lifetime. */
int gstd_streams_subscribe(gstd_streams *, unsigned connection, const char *deployment,
                           gstd_stream_kind, const char *component, char id[65]);
/* 1=closed,0=already closed,-1=foreign/unknown. Closed identity checks require no
 * unbounded tombstone list. Connection reuse invalidates the old namespace. */
int gstd_streams_unsubscribe(gstd_streams *, unsigned connection, const char *id);
void gstd_streams_disconnect(gstd_streams *, unsigned connection);
/* Broadcast immutable redacted payload to matching subscribers. A slow queue
 * receives a terminal STREAM_OVERFLOW gap, without closing another observer or
 * affecting a deployment/operation. 0=bounded delivery,-1=invalid/OOM. */
int gstd_streams_publish(gstd_streams *, const char *deployment, gstd_stream_kind,
                         const char *component, const json_t *redacted_payload);
/* Replay targets exactly one owned subscription, preserving other readers'
 * cursors and sequence namespaces. Pending count allows bounded page scheduling;
 * -1 means closed/foreign, and -2 means a terminal gap is queued. */
int gstd_streams_replay(gstd_streams *, unsigned connection, const char *id, const char *deployment,
                        gstd_stream_kind, const char *component, const json_t *);
int gstd_streams_pending(const gstd_streams *, unsigned connection, const char *id);
/* A live replay failure terminates only this observer with an explicit gap. */
int gstd_streams_terminal(gstd_streams *, unsigned connection, const char *id, const char *code);
/* Round-robin across this connection's subscriptions; 1=owned event,0=none,
 * -1=invalid/OOM. Event identity is subscriptionId, never requestId. */
int gstd_streams_next(gstd_streams *, unsigned connection, json_t **event);
size_t gstd_streams_queued_bytes(const gstd_streams *);
#endif
