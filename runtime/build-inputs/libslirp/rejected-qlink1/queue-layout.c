#define _GNU_SOURCE 1
#include "slirp.h"
#include <stddef.h>
#include <stdalign.h>
_Static_assert(sizeof(struct qlink) == 2 * sizeof(void *), "queue size must not change");
_Static_assert(offsetof(struct qlink, next) == 0, "next offset must not change");
_Static_assert(offsetof(struct qlink, prev) == sizeof(void *), "prev offset must not change");
_Static_assert(_Alignof(struct qlink) == 1, "packet overlay must allow unaligned access");
int main(void) {
    alignas(void *) unsigned char bytes[sizeof(struct qlink) + 1];
    struct qlink *queue = (void *)(bytes + 1);
    queue->next = bytes;
    queue->prev = bytes + sizeof(struct qlink);
    return queue->next != bytes || queue->prev != bytes + sizeof(struct qlink);
}
