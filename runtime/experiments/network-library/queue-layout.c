#define _GNU_SOURCE 1
#include "slirp.h"
#include <stdalign.h>
#include <stddef.h>
_Static_assert(sizeof(struct qlink) == 2 * sizeof(void *), "normal queue size unchanged");
_Static_assert(_Alignof(struct qlink) == _Alignof(void *), "normal queue alignment unchanged");
_Static_assert(sizeof(struct qlink_unaligned) == sizeof(struct qlink), "view size must agree");
_Static_assert(offsetof(struct qlink_unaligned, next) == offsetof(struct qlink, next),
               "next offset");
_Static_assert(offsetof(struct qlink_unaligned, prev) == offsetof(struct qlink, prev),
               "prev offset");
_Static_assert(_Alignof(struct qlink_unaligned) == 1, "packet view allows unaligned access");
_Static_assert(sizeof(struct slirp_quehead) == sizeof(struct qlink), "generic view size");
_Static_assert(_Alignof(struct slirp_quehead) == 1, "generic helpers allow packet views");
int main(void) {
    alignas(void *) unsigned char bytes[3 * sizeof(struct qlink) + 1];
    struct slirp_quehead *head = (void *)(bytes + 1);
    struct slirp_quehead *first = (void *)(bytes + 1 + sizeof(struct qlink));
    struct slirp_quehead *second = (void *)(bytes + 1 + 2 * sizeof(struct qlink));
    head->qh_link = head->qh_rlink = head;
    slirp_insque(first, head);
    slirp_insque(second, first);
    if (head->qh_link != first || first->qh_link != second || second->qh_link != head)
        return 1;
    slirp_remque(first);
    if (head->qh_link != second || second->qh_rlink != head || first->qh_rlink != NULL)
        return 1;
    slirp_remque(second);
    return head->qh_link != head || head->qh_rlink != head;
}
