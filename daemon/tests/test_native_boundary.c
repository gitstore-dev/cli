#define _POSIX_C_SOURCE 200809L
#include "backend_libcrun.h"
#include "native_job.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_native_observation observed;
    memset(&observed, 255, sizeof(observed));
    CHECK(gstd_native_apply(NULL, GSTD_NATIVE_CREATE, &observed) == GSTD_NATIVE_INVALID);
    CHECK(!observed.pid && !observed.start_time && !observed.effect_possible);
    gstd_native_context context = {
        .component = "api", .backend_id = "owned", .control_owner = getpid()};
    CHECK(gstd_native_apply(&context, GSTD_NATIVE_CREATE, &observed) == GSTD_NATIVE_INVALID);
    context.control_owner++;
    CHECK(gstd_native_apply(&context, GSTD_NATIVE_TERM, &observed) == GSTD_NATIVE_INVALID);
    CHECK(gstd_native_apply(&context, GSTD_NATIVE_DELETE, &observed) == GSTD_NATIVE_INVALID);
    context.expected_start_time = 1;
    CHECK(gstd_native_apply(&context, GSTD_NATIVE_OBSERVE, &observed) == GSTD_NATIVE_INVALID);
    context.expected_pid = 1;
    CHECK(gstd_native_apply(&context, GSTD_NATIVE_CREATE, &observed) == GSTD_NATIVE_INVALID);
    gstd_native_job job = {0};
    CHECK(gstd_native_launch(&context, GSTD_NATIVE_CREATE, 1000, &job) == -1);
    context.control_owner = getpid();
    CHECK(gstd_native_launch(&context, GSTD_NATIVE_CREATE, 0, &job) == -1);
    CHECK(gstd_native_launch(&context, GSTD_NATIVE_CREATE, 30001, &job) == -1);
    gstd_native_status status;
    CHECK(gstd_native_poll(&job, -1, &status, &observed) == -1);
    CHECK(!job.worker);
    return 0;
}
