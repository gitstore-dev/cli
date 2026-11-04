#include "gitstored/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int valid(const char *s) {
    json_t *v = gstd_json_parse((const unsigned char *)s, strlen(s));
    int result = v != NULL;
    json_decref(v);
    return result;
}
int main(void) {
    CHECK(valid("{\"n\":9007199254740991,\"s\":\"hello\\u0000world\"}"));
    CHECK(valid("{\"n\":-9007199254740991}"));
    CHECK(valid("{\"a\\u0000b\":1}"));
    CHECK(!valid("{\"a\\u0000b\":1,\"a\\u0000b\":2}"));
    const char *bad[] = {"[]",
                         "null",
                         "1",
                         "{} {}",
                         "{\"n\":9007199254740992}",
                         "{\"n\":-9007199254740992}",
                         "{\"n\":1.0}",
                         "{\"n\":1e0}",
                         "{\"x\":1,\"\\u0078\":2}",
                         "{\"x\":NaN}",
                         "{\"x\":\"\xff\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        CHECK(!valid(bad[i]));
    char nested[256];
    for (unsigned depth = 32; depth <= 33; ++depth) {
        char *p = nested;
        memcpy(p, "{\"x\":", 5);
        p += 5;
        for (unsigned i = 1; i < depth; ++i)
            *p++ = '[';
        *p++ = '0';
        for (unsigned i = 1; i < depth; ++i)
            *p++ = ']';
        *p++ = '}';
        *p = 0;
        CHECK(valid(nested) == (depth == 32));
    }
    char *large = malloc(GSTD_STRING_MAX + 32);
    CHECK(large);
    memcpy(large, "{\"s\":\"", 6);
    memset(large + 6, 'a', GSTD_STRING_MAX + 1);
    memcpy(large + 6 + GSTD_STRING_MAX, "\"}", 3);
    CHECK(valid(large));
    large[6 + GSTD_STRING_MAX] = 'a';
    memcpy(large + 7 + GSTD_STRING_MAX, "\"}", 3);
    CHECK(!valid(large));
    free(large);
    char *items = malloc(GSTD_ITEMS_MAX * 2 + 32);
    CHECK(items);
    char *p = items;
    memcpy(p, "{\"a\":[", 6);
    p += 6;
    for (unsigned i = 0; i < GSTD_ITEMS_MAX; ++i) {
        *p++ = '0';
        *p++ = ',';
    }
    memcpy(p - 1, "]}", 3);
    CHECK(valid(items));
    memcpy(p, "0]}", 4);
    CHECK(!valid(items));
    free(items);
    CHECK(gstd_identifier("a-_.:Z09", 8));
    CHECK(!gstd_identifier("../a", 4));
    CHECK(!gstd_identifier("", 0));
    CHECK(!gstd_identifier("a\0b", 3));
    gstd_frame f = {0};
    size_t consumed;
    unsigned char bytes[] = {0, 0, 0, 2, '{', '}', 0, 0, 0, 2, '{', '}'};
    for (size_t i = 0; i < 5; ++i)
        CHECK(gstd_frame_feed(&f, bytes + i, 1, &consumed) == 0 && consumed == 1);
    CHECK(!gstd_frame_eof(&f));
    CHECK(gstd_frame_feed(&f, bytes + 5, 7, &consumed) == 1 && consumed == 1);
    CHECK(gstd_frame_eof(&f));
    CHECK(f.used == 2 && memcmp(f.payload, "{}", 2) == 0);
    gstd_frame_reset(&f);
    CHECK(gstd_frame_feed(&f, bytes + 6, 6, &consumed) == 1 && consumed == 6);
    gstd_frame_reset(&f);
    unsigned char zero[] = {0, 0, 0, 0};
    CHECK(gstd_frame_feed(&f, zero, 4, &consumed) == -1 && f.payload == NULL);
    gstd_frame_reset(&f);
    unsigned char huge[] = {0, 16, 0, 1};
    CHECK(gstd_frame_feed(&f, huge, 4, &consumed) == -1 && f.payload == NULL);
    gstd_frame_reset(&f);
    return 0;
}
