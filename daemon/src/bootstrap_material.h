#ifndef GSTD_BOOTSTRAP_MATERIAL_H
#define GSTD_BOOTSTRAP_MATERIAL_H
#include "gitstored/credentials.h"
#include <sys/types.h>
/* Private worker only: never called from validation or the control owner.
 * Generated material is returned only on private credential-broker IPC and must
 * be persisted before preparing runtime files. No plaintext diagnostic/output. */
int gstd_bootstrap_material_generate(pid_t control_owner, gstd_credential *);
int gstd_bootstrap_material_check(const gstd_credential *);
/* Public key only, malloc-owned. Existing controller private key is preserved. */
char *gstd_bootstrap_controller_public(const gstd_credential *);
#endif
