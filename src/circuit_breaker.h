/******************************************************************************
 * circuit_breaker.h — Per-backend circuit breaker.
 *
 * States:  CLOSED  → requests flow normally
 *          OPEN    → requests rejected; cooldown timer running
 *          HALF    → one probe request allowed to test recovery
 *
 * The breaker state is embedded in backend_t (consecutive_failures,
 * circuit_open_until).  These helpers read/write those atomics.
 ******************************************************************************/

#ifndef CIRCUIT_BREAKER_H
#define CIRCUIT_BREAKER_H

#include "config.h"
#include "upstreams.h"

/* Returns 1 if the circuit is open (backend should be skipped). */
int  cb_is_open(int backend_idx, const proxy_config_t *cfg);

/* Record a failure; may trip the breaker to OPEN. */
void cb_record_failure(int backend_idx, const proxy_config_t *cfg);

/* Record a success; resets consecutive failures and closes the breaker. */
void cb_record_success(int backend_idx);

#endif /* CIRCUIT_BREAKER_H */
