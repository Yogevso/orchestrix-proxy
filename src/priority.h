/******************************************************************************
 * priority.h — Request priority classification for traffic shaping.
 *
 * Supports two priority levels:
 *   PRIORITY_HIGH   — enqueued at the HEAD of the work queue
 *   PRIORITY_NORMAL — enqueued at the TAIL (default FIFO behaviour)
 *
 * High-priority paths are configured in [priority] config section.
 ******************************************************************************/

#ifndef PRIORITY_H
#define PRIORITY_H

#include "config.h"

typedef enum {
    PRIORITY_NORMAL = 0,
    PRIORITY_HIGH   = 1
} request_priority_t;

/* Classify a request path based on configured priority prefixes. */
request_priority_t priority_classify(const char *path, const proxy_config_t *cfg);

#endif /* PRIORITY_H */
