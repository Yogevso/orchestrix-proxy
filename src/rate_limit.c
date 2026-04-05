/******************************************************************************
 * rate_limit.c — Per-client-IP fixed-window rate limiter.
 *
 * Uses a simple hash table of IP → (count, window_start).  When a new
 * minute window begins, the counter resets.  Thread-safe via a mutex.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "rate_limit.h"

#define RL_BUCKETS 1024

typedef struct rl_entry {
    char              ip[64];
    int               count;
    time_t            window_start;
    struct rl_entry  *next;
} rl_entry_t;

static rl_entry_t   *rl_table[RL_BUCKETS];
static pthread_mutex_t rl_lock = PTHREAD_MUTEX_INITIALIZER;
static int             rl_limit;

static unsigned int rl_hash(const char *ip) {
    unsigned int h = 5381;
    while (*ip)
        h = ((h << 5) + h) + (unsigned char)*ip++;
    return h % RL_BUCKETS;
}

void ratelimit_init(int limit) {
    rl_limit = limit;
    memset(rl_table, 0, sizeof(rl_table));
}

int ratelimit_allow(const char *client_ip) {
    if (rl_limit <= 0) return 1;

    unsigned int idx = rl_hash(client_ip);
    time_t now = time(NULL);

    pthread_mutex_lock(&rl_lock);

    rl_entry_t *e = rl_table[idx];
    while (e) {
        if (strcmp(e->ip, client_ip) == 0) break;
        e = e->next;
    }

    if (!e) {
        e = calloc(1, sizeof(rl_entry_t));
        if (!e) { pthread_mutex_unlock(&rl_lock); return 1; }
        snprintf(e->ip, sizeof(e->ip), "%s", client_ip);
        e->window_start = now;
        e->count = 0;
        e->next = rl_table[idx];
        rl_table[idx] = e;
    }

    /* Reset window if minute has elapsed. */
    if (now - e->window_start >= 60) {
        e->window_start = now;
        e->count = 0;
    }

    e->count++;
    int allowed = (e->count <= rl_limit);

    pthread_mutex_unlock(&rl_lock);
    return allowed;
}

void ratelimit_destroy(void) {
    pthread_mutex_lock(&rl_lock);
    for (int i = 0; i < RL_BUCKETS; i++) {
        rl_entry_t *e = rl_table[i];
        while (e) {
            rl_entry_t *next = e->next;
            free(e);
            e = next;
        }
        rl_table[i] = NULL;
    }
    pthread_mutex_unlock(&rl_lock);
}
