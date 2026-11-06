#ifndef GSTD_TEST_OIDC_MIGRATION_JOURNAL_H
#define GSTD_TEST_OIDC_MIGRATION_JOURNAL_H
#include "oidc_setup.h"
/* Synthetic execution directories; real prepared volume/network capabilities.
 * Metadata policy only: never creates a native workload or executes SQL. */
int gstd_test_oidc_migration_journal(const gstd_deployment_environment *,
                                      const gstd_deployment_target *, const gstd_oidc_setup *, int expect_dispatch,
                                      const char *native_source);
#endif
