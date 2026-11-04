#ifndef GSTD_TEST_BACKEND_CONTROLLED_H
#define GSTD_TEST_BACKEND_CONTROLLED_H
#include "backend.h"
#include <sys/types.h>
typedef enum {
    GSTD_TEST_SLOW,
    GSTD_TEST_FAILED,
    GSTD_TEST_UNCERTAIN,
    GSTD_TEST_UNSAFE_MIGRATION
} gstd_test_behavior;
typedef struct {
    char identity[65];
    pid_t child;
    int control_fd, behavior, exited, status;
} gstd_test_workload;
/* Fixed built-in child behavior only: no shell, executable, argv or path input.
 * All handles must remain alive until close; at most eight fixtures at once. */
const gstd_backend *gstd_test_controlled_backend(void);
int gstd_test_workload_start(gstd_test_behavior, gstd_test_workload *);
int gstd_test_workload_release(gstd_test_workload *);
int gstd_test_workload_reveal(gstd_test_workload *);
int gstd_test_workload_close(gstd_test_workload *);
#endif
