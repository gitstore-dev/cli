#ifndef GSTD_NATIVE_POPULATION_H
#define GSTD_NATIVE_POPULATION_H
#include <jansson.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    int directory_fd, events_fd;
    uint64_t device, inode, events_inode;
} gstd_native_population;
#define GSTD_NATIVE_POPULATION_INITIAL {.directory_fd = -1, .events_fd = -1}
/* Capture while the recorded native init is still alive. Boot, PID/start and
 * exact crun scope membership must match before and after opening its actual
 * cgroup2 kernel inode. Initialize with the macro. No arbitrary caller path or
 * signal is accepted. */
int gstd_native_population_open(const char *native_id, pid_t, uint64_t start, const char *boot,
                                gstd_native_population *);
/* Read-only: 1=kernel reports this held cgroup subtree empty (or its exact inode
 * has been removed), 0=live population, -1=unknown. A vanished pathname or a
 * stopped init alone is never proof. Retain these FDs across stop/delete. */
int gstd_native_population_empty(const gstd_native_population *);
/* Confirm a still-live init remains in the exact captured kernel group. */
int gstd_native_population_matches(const gstd_native_population *, const char *native_id, pid_t,
                                   uint64_t start, const char *boot);
/* Capture/reopen the exact cgroup2 path and inode pair before OCI START. These
 * observers do not signal, create, release a writer or infer death from ENOENT.
 * Missing/replaced/inaccessible groups fail closed. Identity is boot-bound. */
json_t *gstd_native_population_identity(const gstd_native_population *, const char *native_id,
                                        const char *boot);
int gstd_native_population_reopen(const json_t *, const char *native_id, const char *boot,
                                  gstd_native_population *);
void gstd_native_population_close(gstd_native_population *);
#endif
