/******************************************************************************
 * circuit_breaker.c — Per-backend circuit breaker.
 ******************************************************************************/

#include <stdio.h>
#include <time.h>
#include "circuit_breaker.h"
#include "upstreams.h"

int cb_is_open(int backend_idx, const proxy_config_t *cfg) {
    if (!cfg->circuit_breaker_enabled) return 0;
    if (backend_idx < 0 || backend_idx >= g_upstreams.count) return 0;

    backend_t *b = &g_upstreams.backends[backend_idx];
    long until = atomic_load(&b->circuit_open_until);

    if (until == 0) return 0;           /* breaker closed */

    time_t now = time(NULL);
    if (now >= until) {
        /* Cooldown expired — half-open: allow exactly one probe request. */
        int expected = 0;
        if (atomic_compare_exchange_strong(&b->half_open_probe, &expected, 1)) {
            /* We won the CAS — this request is the probe. */
            return 0;
        }
        /* Another request is already probing — stay open. */
        return 1;
    }
    return 1;                           /* breaker open */
}

void cb_record_failure(int backend_idx, const proxy_config_t *cfg) {
    if (backend_idx < 0 || backend_idx >= g_upstreams.count) return;
    backend_t *b = &g_upstreams.backends[backend_idx];

    /* Reset probe flag so next half-open window allows a new probe. */
    atomic_store(&b->half_open_probe, 0);

    int cf = atomic_fetch_add(&b->consecutive_failures, 1) + 1;

    if (cfg->circuit_breaker_enabled && cf >= cfg->circuit_breaker_threshold) {
        time_t trip_until = time(NULL) + cfg->circuit_breaker_cooldown_sec;
        atomic_store(&b->circuit_open_until, (long)trip_until);
        fprintf(stderr, "[CIRCUIT-BREAKER] %s OPEN (failures=%d, cooldown=%ds)\n",
                b->name, cf, cfg->circuit_breaker_cooldown_sec);
    }
}

void cb_record_success(int backend_idx) {
    if (backend_idx < 0 || backend_idx >= g_upstreams.count) return;
    backend_t *b = &g_upstreams.backends[backend_idx];
    atomic_store(&b->consecutive_failures, 0);
    atomic_store(&b->circuit_open_until, 0);
    atomic_store(&b->half_open_probe, 0);
}
