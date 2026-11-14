#define _GNU_SOURCE 1
/* Developer-only payload validator; excluded from production/installation. */
#include "backup.h"
#include "gitstored/protocol.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if ((argc != 2 && argc != 3) || !geteuid() || argv[1][0] != '/')
        return 2;
    gstd_json_initialize();
    int payload = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (payload < 3) {
        if (payload >= 0)
            close(payload);
        return 1;
    }
    const char *target = "gitstore-06152d-schema-012";
    int result = 1;
    json_t *manifest = NULL;
    if (argc == 2) {
        manifest = gstd_backup_describe(payload, target, UINT64_C(512) * 1024 * 1024);
        char *bytes = manifest ? gstd_json_dump(manifest) : NULL;
        if (bytes) {
            puts(bytes);
            free(bytes);
            result = 0;
        }
    } else {
        int input = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat info;
        char buffer[65537];
        if (input >= 3 && !fstat(input, &info) && S_ISREG(info.st_mode) &&
            info.st_uid == geteuid() && (info.st_mode & 07777) == 0600 && info.st_nlink == 1 &&
            info.st_size > 0 && info.st_size <= 65536) {
            ssize_t count = read(input, buffer, (size_t)info.st_size);
            manifest = count == info.st_size
                           ? gstd_json_parse((unsigned char *)buffer, (size_t)count)
                           : NULL;
            if (manifest && !gstd_backup_restore_validate(payload, manifest, target,
                                                          UINT64_C(512) * 1024 * 1024)) {
                puts("{\"validated\":true}");
                result = 0;
            }
        }
        if (input >= 0)
            close(input);
    }
    json_decref(manifest);
    close(payload);
    return result;
}
