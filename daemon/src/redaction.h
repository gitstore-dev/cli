#ifndef GSTD_REDACTION_H
#define GSTD_REDACTION_H
#include <stddef.h>
#define GSTD_LOG_LINE_MAX 16384u
typedef struct gstd_redactor gstd_redactor;
typedef int (*gstd_redacted_sink)(void *, const char *, size_t);
/* Collector-private memory only. Known credentials never enter the manager or
 * retained logs. At most 64 strings/64 KiB total, each at most 4096 bytes. PEM
 * blocks and sensitive-field lines are suppressed independently of this list. */
gstd_redactor *gstd_redactor_open(const char *const *secrets, size_t count, gstd_redacted_sink,
                                  void *);
/* Preserve incomplete lines across chunks. Oversized, binary and invalid UTF-8
 * lines yield explicit omission markers, never an unredacted prefix. Input call
 * bound is 64 KiB. Sink failure is sticky and prohibits further ingestion. */
int gstd_redactor_feed(gstd_redactor *, const void *, size_t);
int gstd_redactor_finish(gstd_redactor *);
void gstd_redactor_close(gstd_redactor *);
#endif
