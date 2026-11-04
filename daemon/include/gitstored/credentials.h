#ifndef GITSTORED_CREDENTIALS_H
#define GITSTORED_CREDENTIALS_H
#include <stddef.h>
typedef struct {
    unsigned char *bytes;
    size_t length;
} gstd_credential;
void gstd_credential_clear(gstd_credential *secret);
/* Private same-UID Unix stream, borrowed FD. One bounded request, then
 * write half-close; bounded secret response. 0=secret, 1=absent, -1=failure.
 * Read requests are nonsecret; bootstrap create-if-absent requests include
 * sensitive material, and their caller must clear its owned request buffer.
 * Output must initially be zero. No diagnostics include request/secret bytes. */
int gstd_credential_exchange(int socket_fd, const unsigned char *request, size_t length,
                             unsigned timeout_ms, gstd_credential *out);
/* Linux worker only, before any threads: execute the caller's registered installed
 * gs broker through a pinned executable FD, never PATH. Caller establishes trusted
 * installation/source identity; this checks FD owner/mode/type and ELF format.
 * Sanitized environment, private FD 3, closed unrelated FDs, bounded lifetime.
 * OS-store access uses only the canonical /run/user/UID/bus; other session-bus
 * layouts require explicit protected-file input for this initial implementation.
 * No runtime download/credential lookup is performed by the control owner. */
int gstd_credential_run(int installed_broker_fd, const unsigned char *request, size_t length,
                        unsigned timeout_ms, gstd_credential *out);
#endif
