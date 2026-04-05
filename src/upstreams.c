/******************************************************************************
 * upstreams.c — Backend registry, load balancing, and health state.
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "upstreams.h"
#include "circuit_breaker.h"

upstream_pool_t g_upstreams;

void upstreams_init(const proxy_config_t *cfg) {
    memset(&g_upstreams, 0, sizeof(g_upstreams));
    pthread_mutex_init(&g_upstreams.lock, NULL);
    g_upstreams.strategy = (lb_strategy_t)cfg->lb_strategy;

    for (int i = 0; i < cfg->backend_count && i < MAX_BACKENDS; i++) {
        backend_t *b = &g_upstreams.backends[i];
        snprintf(b->name, sizeof(b->name), "%s", cfg->backends[i].name);
        snprintf(b->host, sizeof(b->host), "%s", cfg->backends[i].host);
        b->port = cfg->backends[i].port;
        atomic_store(&b->state, BACKEND_UP);
        atomic_store(&b->active_connections, 0);
        atomic_store(&b->total_requests, 0);
        atomic_store(&b->total_failures, 0);
        atomic_store(&b->consecutive_failures, 0);
        atomic_store(&b->circuit_open_until, 0);
    }
    g_upstreams.count = cfg->backend_count;
}

/* Round-robin: pick next healthy backend. */
static int select_round_robin(const proxy_config_t *cfg) {
    int n = g_upstreams.count;
    if (n == 0) return -1;

    unsigned long base = atomic_fetch_add(&g_upstreams.rr_counter, 1);

    for (int attempt = 0; attempt < n; attempt++) {
        int idx = (int)((base + attempt) % n);
        backend_t *b = &g_upstreams.backends[idx];

        int st = atomic_load(&b->state);
        if (st != BACKEND_UP) continue;

        if (cfg->circuit_breaker_enabled && cb_is_open(idx, cfg))
            continue;

        return idx;
    }
    return -1;
}

/* Least-connections: pick healthy backend with fewest active connections. */
static int select_least_connections(const proxy_config_t *cfg) {
    int n = g_upstreams.count;
    if (n == 0) return -1;

    int best = -1;
    int best_active = __INT_MAX__;

    for (int i = 0; i < n; i++) {
        backend_t *b = &g_upstreams.backends[i];

        int st = atomic_load(&b->state);
        if (st != BACKEND_UP) continue;

        if (cfg->circuit_breaker_enabled && cb_is_open(i, cfg))
            continue;

        int active = atomic_load(&b->active_connections);
        if (active < best_active) {
            best_active = active;
            best = i;
        }
    }
    return best;
}

int upstreams_select(const proxy_config_t *cfg) {
    if (g_upstreams.strategy == LB_LEAST_CONNECTIONS)
        return select_least_connections(cfg);
    return select_round_robin(cfg);
}

void upstreams_mark_failure(int idx, const proxy_config_t *cfg) {
    if (idx < 0 || idx >= g_upstreams.count) return;
    backend_t *b = &g_upstreams.backends[idx];
    atomic_fetch_add(&b->total_failures, 1);

    if (cfg->circuit_breaker_enabled)
        cb_record_failure(idx, cfg);
}

void upstreams_mark_success(int idx) {
    if (idx < 0 || idx >= g_upstreams.count) return;
    backend_t *b = &g_upstreams.backends[idx];
    atomic_fetch_add(&b->total_requests, 1);
    cb_record_success(idx);
}

void upstreams_set_state(int idx, backend_state_t state) {
    if (idx < 0 || idx >= g_upstreams.count) return;
    atomic_store(&g_upstreams.backends[idx].state, state);
    fprintf(stderr, "[UPSTREAM] %s → %s\n",
            g_upstreams.backends[idx].name,
            state == BACKEND_UP ? "UP" :
            state == BACKEND_DRAINING ? "DRAINING" : "DOWN");
}

int upstreams_find(const char *name) {
    for (int i = 0; i < g_upstreams.count; i++)
        if (strcmp(g_upstreams.backends[i].name, name) == 0)
            return i;
    return -1;
}

void upstreams_reload(const proxy_config_t *cfg) {
    pthread_mutex_lock(&g_upstreams.lock);

    /* Mark existing backends not in new config as DRAINING. */
    for (int i = 0; i < g_upstreams.count; i++) {
        int found = 0;
        for (int j = 0; j < cfg->backend_count; j++) {
            if (strcmp(g_upstreams.backends[i].name, cfg->backends[j].name) == 0) {
                found = 1;
                /* Update host/port if changed. */
                snprintf(g_upstreams.backends[i].host, 64, "%s", cfg->backends[j].host);
                g_upstreams.backends[i].port = cfg->backends[j].port;
                break;
            }
        }
        if (!found) {
            upstreams_set_state(i, BACKEND_DRAINING);
        }
    }

    /* Add new backends. */
    for (int j = 0; j < cfg->backend_count; j++) {
        if (upstreams_find(cfg->backends[j].name) < 0 &&
            g_upstreams.count < MAX_BACKENDS) {
            int idx = g_upstreams.count;
            backend_t *b = &g_upstreams.backends[idx];
            snprintf(b->name, sizeof(b->name), "%s", cfg->backends[j].name);
            snprintf(b->host, sizeof(b->host), "%s", cfg->backends[j].host);
            b->port = cfg->backends[j].port;
            atomic_store(&b->state, BACKEND_UP);
            atomic_store(&b->active_connections, 0);
            atomic_store(&b->total_requests, 0);
            atomic_store(&b->total_failures, 0);
            atomic_store(&b->consecutive_failures, 0);
            atomic_store(&b->circuit_open_until, 0);
            g_upstreams.count++;
            fprintf(stderr, "[UPSTREAM] Added %s (%s:%d)\n", b->name, b->host, b->port);
        }
    }

    g_upstreams.strategy = (lb_strategy_t)cfg->lb_strategy;
    pthread_mutex_unlock(&g_upstreams.lock);
}
