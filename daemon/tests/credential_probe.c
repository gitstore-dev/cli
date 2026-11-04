#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/credentials.h"
#include "gitstored/protocol.h"
#include <fcntl.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Opt-in process probe. Arguments are disposable binary/file paths, no secrets. */
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    gstd_json_initialize();
    int executable = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (executable < 0)
        return 2;
    json_t *request =
        json_pack("{s:{s:s,s:s,s:s,s:s,s:s,s:s},s:{s:s,s:s}}", "identity", "installation",
                  "local-fixture", "issuer", "fixture-issuer", "account", "fixture-account",
                  "purpose", "registry-pull", "registry", "https://ghcr.io", "repository",
                  "gitstore-dev/api", "source", "kind", "protected-file", "path", argv[2]);
    char *bytes = gstd_json_dump(request);
    json_decref(request);
    gstd_credential secret = {0};
    int status = !bytes ? -1
                        : gstd_credential_run(executable, (unsigned char *)bytes, strlen(bytes),
                                              5000, &secret);
    int valid =
        status == 0 && secret.length == 21 && !memcmp(secret.bytes, "private-fixture-value", 21);
    gstd_credential_clear(&secret);
    free(bytes);
    close(executable);
    return valid ? 0 : 1;
}
