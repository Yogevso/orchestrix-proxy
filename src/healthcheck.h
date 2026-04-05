/******************************************************************************
 * healthcheck.h — Periodic backend health probes.
 ******************************************************************************/

#ifndef HEALTHCHECK_H
#define HEALTHCHECK_H

#include "config.h"

/* Start the health-check background thread.  Returns 0 on success. */
int  healthcheck_start(const proxy_config_t *cfg);

/* Stop the health-check thread (called during shutdown). */
void healthcheck_stop(void);

#endif /* HEALTHCHECK_H */
