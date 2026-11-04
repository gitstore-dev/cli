#include "gitstored/execution.h"
#include "execution_policy.inc"
#include "gitstored/protocol.h"
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

json_t *gstd_execution_policy(void) {
    return gstd_json_parse(execution_policy, sizeof(execution_policy) - 1);
}
const char *gstd_execution_policy_digest(void) { return execution_policy_digest; }
int gstd_execution_kernel_supported(void) {
#ifdef __linux__
    struct utsname host;
    unsigned major = 0, minor = 0;
    if (uname(&host) || strcmp(host.sysname, "Linux") ||
        sscanf(host.release, "%u.%u", &major, &minor) != 2)
        return 0;
    return major > 6 || (major == 6 && minor >= 8);
#else
    return 0;
#endif
}
int gstd_execution_backoff(unsigned attempts, uint64_t elapsed, unsigned *delay) {
    if (!delay)
        return -1;
    *delay = 0;
    if (!attempts)
        return -1;
    if (attempts > 4 || elapsed >= 10000)
        return 0;
    unsigned next = 250u << (attempts - 1);
    if (next > 10000 - elapsed)
        return 0;
    *delay = next;
    return 1;
}
