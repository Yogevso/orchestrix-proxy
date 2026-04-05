/******************************************************************************
 * upstreams.h — Backend registry, load balancing, and health state.
 ******************************************************************************/

#ifndef UPSTREAMS_H
#define UPSTREAMS_H

#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include "config.h"

typedef enum {
    BACKEND_UP       = 0,
    BACKEND_DRAINING = 1,
    BACKEND_DOWN     = 2
} backend_state_t;

typedef enum {
    LB_ROUND_ROBIN       = 0,
    LB_LEAST_CONNECTIONS = 1
} lb_strategy_t;

typedef struct {
    char         name[64];
    char         host[64];
    int          port;
    atomic_int   state;                /* backend_state_t */
    atomic_int   active_connections;
    atomic_ulong total_requests;
    atomic_ulong total_failures;
    /* Circuit breaker state */
    atomic_int   consecutive_failures;
    atomic_long  circuit_open_until;   /* epoch seconds; 0 = closed */
} backend_t;

typedef struct {
    backend_t       backends[MAX_BACKENDS];
    int             count;
    atomic_ulong    rr_counter;        /* for round-robin */
    lb_strategy_t   strategy;
    pthread_mutex_t lock;              /* protects count / add / remove */
} upstream_pool_t;

/* Global upstream pool. */
extern upstream_pool_t g_upstreams;

/* Initialise the pool from parsed config. */
void upstreams_init(const proxy_config_t *cfg);

/* Select the next healthy backend.  Returns index or -1 if none available. */
int  upstreams_select(const proxy_config_t *cfg);

/* Mark a backend as failed (increments failure counters, may trip breaker). */
void upstreams_mark_failure(int idx, const proxy_config_t *cfg);

/* Mark a backend request success (resets consecutive failures). */
void upstreams_mark_success(int idx);

/* Set the state of a backend (UP / DRAINING / DOWN). */
void upstreams_set_state(int idx, backend_state_t state);

/* Find backend index by name.  Returns -1 if not found. */
int  upstreams_find(const char *name);

/* Reload backends from a new config (add new, drain removed). */
void upstreams_reload(const proxy_config_t *cfg);

#endif /* UPSTREAMS_H */
