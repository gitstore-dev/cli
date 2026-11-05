#include "gitstored/protocol.h"
#include <stdint.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    gstd_frame frame = {0};
    size_t offset = 0;
    while (offset < size) {
        /* Both fragmented headers/payloads and multiple coalesced frames. */
        size_t chunk = 1 + data[offset] % 127;
        if (chunk > size - offset)
            chunk = size - offset;
        size_t consumed = 0;
        int ready = gstd_frame_feed(&frame, data + offset, chunk, &consumed);
        if (consumed > chunk || (ready >= 0 && !consumed))
            abort();
        offset += consumed;
        if (ready < 0)
            break;
        if (ready) {
            if (frame.used != frame.expected || frame.expected > GSTD_FRAME_MAX)
                abort();
            json_t *message = gstd_json_parse(frame.payload, frame.used);
            if (message) {
                gstd_request request;
                (void)gstd_request_parse(message, &request);
                json_decref(message);
            }
            gstd_frame_reset(&frame);
        }
    }
    (void)gstd_frame_eof(&frame);
    gstd_frame_reset(&frame);
    return 0;
}
