#ifndef GSTD_LOG_CAPTURE_H
#define GSTD_LOG_CAPTURE_H
#include "gitstored/protocol.h"
#include <sys/types.h>
#define GSTD_CAPTURE_MAX 9u
typedef struct {
    int state_fd, configuration_fd,
        broker_fd; /* configuration scope wrapper, not its material data directory */
    const json_t *source, *oidc_source;
    const char *store_id, *deployment_id, *operation_id;
    uint64_t generation;
    unsigned components; /* Git/API[/controller][/Scylla] order */
    unsigned oidc;       /* five fixed provider roles after core/datastore */
    unsigned scylladb;   /* trusted persistent graph dependency, not a wire selector */
    int test_fixture;    /* synthetic credentials only in explicitly built tests */
    const char *const *test_secrets;
    size_t test_secret_count;
} gstd_log_capture_input;
typedef struct {
    pid_t worker;
    int stage, status, channel, death_fd, interrupted;
    int output[GSTD_CAPTURE_MAX][2];
    int64_t deadline_ms;
    uint64_t start;
    char boot[37];
} gstd_log_capture;
#define GSTD_LOG_CAPTURE_INITIAL                                                                   \
    {                                                                                              \
        .channel = -1, .death_fd = -1, .output = { {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1} }        \
    }
/* One private collector per deployment, before any native CREATE. It reads the
 * existing canonical credential only in its private process, redacts both output
 * streams before persistence and never receives repository/Git/registry FDs.
 * Parent output descriptors are borrowed only for native CREATE, then closed.
 * Ready collector survives manager loss while actual workload pipe writers live. */
int gstd_log_capture_launch(const gstd_log_capture_input *, gstd_log_capture *);
/* 0=starting,1=ready/failed; status0=ready,negative=failed. Cancellation/deadline affects
 * only this private pre-launch collector, never an existing native workload. */
int gstd_log_capture_poll(gstd_log_capture *, int cancellation_fd);
void gstd_log_capture_outputs_close(gstd_log_capture *);
void gstd_log_capture_output_close(gstd_log_capture *, unsigned component);
/* Observer close only: never signal a ready collector. Its bounded retired-child
 * registry reaps after writer EOF; return -1 if retirement capacity is occupied. */
int gstd_log_capture_close(gstd_log_capture *);
void gstd_log_capture_reap(void);
json_t *gstd_log_capture_identity(const gstd_log_capture *);
#endif
