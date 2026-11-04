#define _POSIX_C_SOURCE 200809L
#include "backend_controlled.h"
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int await(const gstd_backend *b, gstd_test_workload *w) {
    for (unsigned i = 0; i < 1000; i++) {
        int terminated = 0;
        if (b->observe(w->identity, &terminated))
            return -1;
        if (terminated)
            return 0;
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    return -1;
}
int main(void) {
    alarm(15);
    int result = 0, terminated = -1;
    gstd_test_workload work[8] = {0};
    const gstd_backend *b = gstd_test_controlled_backend();
    for (unsigned i = 0; i < 8; i++)
        CHECK(!gstd_test_workload_start((gstd_test_behavior)(i % 4), &work[i]));
    CHECK(gstd_test_workload_start(GSTD_TEST_SLOW, &work[0]) == -1);
    gstd_test_workload excess;
    CHECK(gstd_test_workload_start(GSTD_TEST_SLOW, &excess) == -1);
    CHECK(b->cancel("unregistered") == -1 && b->observe("unregistered", &terminated) == -1);
    for (unsigned i = 0; i < 8; i++) {
        if (i % 4 == GSTD_TEST_SLOW) {
            CHECK(!b->observe(work[i].identity, &terminated) && !terminated);
            CHECK(!b->cancel(work[i].identity));
        } else if (i % 4 == GSTD_TEST_UNCERTAIN) {
            CHECK(b->observe(work[i].identity, &terminated) == -1);
            CHECK(b->cancel(work[i].identity) == -1);
            CHECK(!gstd_test_workload_release(&work[i]));
            CHECK(b->observe(work[i].identity, &terminated) == -1);
            CHECK(!gstd_test_workload_reveal(&work[i]));
        } else if (i % 4 == GSTD_TEST_UNSAFE_MIGRATION) {
            CHECK(b->cancel(work[i].identity) == -1);
            CHECK(!b->observe(work[i].identity, &terminated) && !terminated);
            CHECK(!gstd_test_workload_release(&work[i]));
        }
        CHECK(!await(b, &work[i]));
        CHECK(WIFEXITED(work[i].status) &&
              WEXITSTATUS(work[i].status) == (i % 4 == GSTD_TEST_FAILED ? 17 : 0));
    }
done:
    for (unsigned i = 0; i < 8; i++)
        if (work[i].child > 0 && gstd_test_workload_close(&work[i]))
            result = 1;
    return result;
}
