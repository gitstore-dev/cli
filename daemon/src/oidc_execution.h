#ifndef GSTD_OIDC_EXECUTION_H
#define GSTD_OIDC_EXECUTION_H
#include "execution_plan.h"
/* Private fixed adapter. Manifest, owned namespace, prepared component files
 * and canonical data writer reservations must already be verified by the owner.
 * No public profile, paths, command, mount targets or environment are accepted.
 * OVERLAY=provider JSON/UI env, POLICY=Kratos identity schema,
 * DATASTORE=Hydra/Kratos SQLite directory, RESOLVER=fixed namespace resolver.
 * All other role descriptors must be -1. Secrets remain behind file descriptors. */
json_t *gstd_oidc_execution_build(const gstd_execution_inputs *);
#endif
