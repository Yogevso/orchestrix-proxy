/******************************************************************************
 * priority.c — Request priority classification.
 ******************************************************************************/

#include <string.h>
#include "priority.h"

request_priority_t priority_classify(const char *path, const proxy_config_t *cfg) {
    if (!cfg->priority_enabled || !path) return PRIORITY_NORMAL;

    for (int i = 0; i < cfg->priority_prefix_count; i++) {
        if (strncmp(path, cfg->priority_prefixes[i],
                    strlen(cfg->priority_prefixes[i])) == 0)
            return PRIORITY_HIGH;
    }
    return PRIORITY_NORMAL;
}
