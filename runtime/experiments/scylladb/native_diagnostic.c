#define _GNU_SOURCE 1
/* Separate translation unit keeps libcrun/json-c out of Jansson callers.
 * Bounded diagnostics are only for this fixed, credential-free developer probe. */
#include "native_abi.h"
#include <libcrun/container.h>
#include <stdio.h>
static void diagnostic(int number, const char *message, int level, void *context) {
    (void)number;
    (void)level;
    (void)context;
    if (message)
        fprintf(stderr, "Scylla raw callback: %.240s\n", message);
}
int scylla_probe_create(const gstd_crun_request *request, const char *bytes, int *number) {
    libcrun_set_verbosity(LIBCRUN_VERBOSITY_ERROR);
    libcrun_context_t context = {.state_root = request->state_root,
                                 .id = request->id,
                                 .bundle = request->bundle,
                                 .detach = true,
                                 .systemd_cgroup = true,
                                 .no_new_keyring = true,
                                 .output_handler = diagnostic};
    libcrun_error_t error = NULL;
    libcrun_container_t *container = libcrun_container_load_from_memory(bytes, &error);
    int result = container ? libcrun_container_create(&context, container, 0, &error) : -1;
    if (container)
        libcrun_container_free(container);
    if (error) {
        *number = error->status;
        fprintf(stderr, "Scylla raw CREATE: %.240s\n", error->msg ? error->msg : "unknown");
        libcrun_error_release(&error);
    }
    return result;
}
