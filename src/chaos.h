/******************************************************************************
 * chaos.h — Failure simulation for testing resilience.
 *
 * When enabled via [chaos] config section, injects:
 *   - Artificial latency before forwarding to the upstream
 *   - Random request drops (returns 500 without forwarding)
 *
 * This allows live demonstration of circuit breaker activation, failover,
 * and latency spike handling.
 ******************************************************************************/

#ifndef CHAOS_H
#define CHAOS_H

#include "config.h"

/* Initialise chaos RNG. Call once at startup. */
void chaos_init(void);

/* Inject configured delay (blocks calling thread). Returns 0 normally. */
void chaos_inject_delay(const proxy_config_t *cfg);

/* Returns 1 if this request should be dropped (simulated failure). */
int  chaos_should_drop(const proxy_config_t *cfg);

#endif /* CHAOS_H */
