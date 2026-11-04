#include "gitstored/core.h"
#include "gitstored/protocol.h"
#include "gitstored/server.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    gstd_json_initialize();
    if (argc == 3 && strcmp(argv[1], "--runtime-dir") == 0)
        return gstd_serve(argv[2], NULL, argv[0], -1);
    if (argc == 5 && !strcmp(argv[1], "--runtime-dir") && !strcmp(argv[3], "--state-dir"))
        return gstd_serve(argv[2], argv[4], argv[0], -1);
    if (argc == 7 && !strcmp(argv[1], "--runtime-dir") && !strcmp(argv[3], "--state-dir") &&
        !strcmp(argv[5], "--ready-fd") && !strcmp(argv[6], "3"))
        return gstd_serve(argv[2], argv[4], argv[0], 3);
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("gitstored %s (protocol 1.0; %s build)\n", gstd_version(),
               gstd_testing() ? "testing" : "production");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("Usage: gitstored --help | --version | --runtime-dir ABSOLUTE_PATH [--state-dir "
             "ABSOLUTE_PATH]\n"
             "Per-user manager; native execution requires linked runtime libraries and "
             "a supported Linux user session.");
        return 0;
    }
    fputs("gitstored: invalid arguments; use --help or --version\n", stderr);
    return 2;
}
