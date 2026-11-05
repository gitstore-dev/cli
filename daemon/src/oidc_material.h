#ifndef GSTD_OIDC_MATERIAL_H
#define GSTD_OIDC_MATERIAL_H
#include "gitstored/credentials.h"
#include <sys/types.h>
/* Private worker only. Persist add-only under oidc-provider-material before any
 * file preparation or provider mutation. Never place this document in parent
 * IPC, operation records, arguments or diagnostics. Independent from bootstrap. */
int gstd_oidc_material_generate(pid_t control_owner, gstd_credential *);
int gstd_oidc_material_check(const gstd_credential *);
#endif
