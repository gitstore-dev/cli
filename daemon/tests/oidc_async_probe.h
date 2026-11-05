#ifndef GSTD_TEST_OIDC_ASYNC_PROBE_H
#define GSTD_TEST_OIDC_ASYNC_PROBE_H
#include <jansson.h>
int gstd_test_oidc_async(const char *root_path, int root, int configuration, int broker,
                         const json_t *source, const json_t *manifest, const char *postman,
                         const char *mock);
#endif
