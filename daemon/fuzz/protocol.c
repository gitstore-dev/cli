#include "gitstored/protocol.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Available only in the explicit testing build; absent from distribution. */
void gstd_test_json_fail_after(size_t);
static void parse(const uint8_t *data, size_t size) {
    json_t *message = gstd_json_parse(data, size);
    if (message) {
        gstd_request request;
        (void)gstd_request_parse(message, &request);
        char *encoded = gstd_json_dump(message);
        if (encoded) {
            json_t *again = gstd_json_parse((const unsigned char *)encoded, strlen(encoded));
            if (!again || !json_equal(message, again))
                abort();
            json_decref(again);
            free(encoded);
        }
        json_decref(message);
    }
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    gstd_json_initialize();
    parse(data, size);
    /* The identical parser also sees deterministic allocation denial. Failure
     * must leave it usable for the next request and must not leak partial trees. */
    json_t *reference = gstd_json_parse(data, size);
    gstd_test_json_fail_after(size % 64);
    json_t *denied = gstd_json_parse(data, size);
    gstd_test_json_fail_after(SIZE_MAX);
    if (denied && (!reference || !json_equal(reference, denied)))
        abort();
    json_decref(denied);
    json_decref(reference);
    return 0;
}
