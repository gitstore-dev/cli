#ifndef GSTD_OIDC_PROBE_SUPPORT_H
#define GSTD_OIDC_PROBE_SUPPORT_H
#include "backend_libcrun.h"
#include "execution_plan.h"
#include "oidc_config.h"
#include <stdint.h>
int64_t oidc_probe_now_ms(void);
void oidc_probe_pause_poll(void);
int oidc_probe_native(gstd_native_context *, gstd_native_action, gstd_native_observation *);
int oidc_probe_record(int, const char *, json_t *);
int oidc_probe_prepare(int, int, const char *, int allow_initial);
int oidc_probe_ready(pid_t, const char *, const gstd_oidc_ports *);
int oidc_probe_enrollment(int owner, int material_directory, int volume_directory, pid_t, uint64_t,
                          const char *, int);
int oidc_probe_adapter_policy_checks(const gstd_execution_inputs *);
int oidc_probe_migrate_provider(int, int, int, int, int, const char *, const char *,
                                const gstd_execution_inputs *);
int oidc_probe_courier(int, const char *, const gstd_network_namespace *, const char *,
                       const char *);
int oidc_probe_recover_ui_stop(const char *);
#endif
