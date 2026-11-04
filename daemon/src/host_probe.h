#ifndef GSTD_HOST_PROBE_H
#define GSTD_HOST_PROBE_H
typedef enum {
    GSTD_HOST_OK,
    GSTD_HOST_KERNEL,
    GSTD_HOST_USER_SESSION,
    GSTD_HOST_CGROUP,
    GSTD_HOST_DELEGATION,
    GSTD_HOST_USER_BUS,
    GSTD_HOST_TUN,
    GSTD_HOST_NAMESPACES,
    GSTD_HOST_PIDFD,
    GSTD_HOST_OPENAT2,
    GSTD_HOST_LIBRARY
} gstd_host_requirement;
/* Credential-free isolated worker only. Canonical user bus, kernel primitives
 * and temporary namespaces are checked before image or credential effects. */
gstd_host_requirement gstd_host_probe(int private_state_fd);
const char *gstd_host_requirement_name(gstd_host_requirement);
#endif
