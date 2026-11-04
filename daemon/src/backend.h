#ifndef GITSTORED_BACKEND_H
#define GITSTORED_BACKEND_H
#include <stddef.h>
/* Internal resolved profile work only; never a caller-supplied executable/path. */
typedef struct {
    int (*observe)(const char *identity, int *terminated);
    int (*cancel)(const char *identity);
} gstd_backend;
#ifdef GITSTORED_TESTING
int gstd_test_backend_register(const gstd_backend *backend);
#endif
#endif
