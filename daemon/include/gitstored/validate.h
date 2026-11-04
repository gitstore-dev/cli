#ifndef GITSTORED_VALIDATE_H
#define GITSTORED_VALIDATE_H
#include <jansson.h>
/* Pure assessment: no cache writes, process launch, enrollment or downloads. */
json_t *gstd_validate(json_t *params);
#endif
