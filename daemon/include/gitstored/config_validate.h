#ifndef GITSTORED_CONFIG_VALIDATE_H
#define GITSTORED_CONFIG_VALIDATE_H
#include <jansson.h>
/* Internal normalized layer trees; not caller-controlled wire configuration. */
json_t *gstd_config_merge(const char *component, const json_t *layers);
json_t *gstd_config_plan(const json_t *profile, const json_t *components, const char *datastore);
json_t *gstd_bootstrap_plan(const json_t *components, const char *datastore);
#endif
