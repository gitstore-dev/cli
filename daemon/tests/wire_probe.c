#include "gitstored/protocol.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    (void)argv;
    unsigned char prefix[4];
    if (fread(prefix, 1, 4, stdin) != 4)
        return 2;
    uint32_t length = ((uint32_t)prefix[0] << 24) | ((uint32_t)prefix[1] << 16) |
                      ((uint32_t)prefix[2] << 8) | prefix[3];
    if (!length || length > GSTD_FRAME_MAX)
        return 2;
    unsigned char *payload = malloc(length);
    if (!payload)
        return 2;
    if (fread(payload, 1, length, stdin) != length || fgetc(stdin) != EOF) {
        free(payload);
        return 2;
    }
    json_t *value = gstd_json_parse(payload, length);
    free(payload);
    if (!value)
        return 2;
    if (argc == 2) {
        gstd_request request;
        if (gstd_request_parse(value, &request) != 0) {
            json_decref(value);
            return 2;
        }
    }
    json_decref(value);
    puts("valid");
    return 0;
}
