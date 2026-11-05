#ifndef GSTD_DEPLOYMENT_TYPES_H
#define GSTD_DEPLOYMENT_TYPES_H
#include "execution_plan.h"
#include "filesystem_job.h"
#include "native_job.h"
#include "native_population.h"
#include "runtime_settings.h"
#include <sqlite3.h>
typedef struct {
    sqlite3 *database;
    const gstd_runtime_settings *settings;
    int state_fd, cache_fd, resource_fd, native_fd, broker_fd;
    gstd_volume_root volume_root;
} gstd_deployment_environment;
typedef struct {
    const char *name;
    char resource[65], asset_resource[65];
    gstd_filesystem_reply scope;
    int rootfs_fd;
    gstd_execution_inputs inputs;
    gstd_native_context native;
    gstd_native_observation observed;
    gstd_native_population population;
    int launched, exited;
    int recovered_stopped; /* exact stopped native tuple plus reopened recorded empty cgroup */
    int death_fd;          /* held pidfd when the manager cannot reap a recovered init */
} gstd_deployment_workload;
#endif
