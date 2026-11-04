#include "gitstored/core.h"
const char *gstd_version(void) {
    return GITSTORED_VERSION;
}
int gstd_testing(void) {
#ifdef GITSTORED_TESTING
    return 1;
#else
    return 0;
#endif
}
