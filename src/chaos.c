/******************************************************************************
 * chaos.c — Failure simulation for testing resilience.
 ******************************************************************************/

#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "chaos.h"

void chaos_init(void) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
}

void chaos_inject_delay(const proxy_config_t *cfg) {
    if (!cfg->chaos_enabled || cfg->chaos_inject_delay_ms <= 0) return;

    struct timespec ts;
    ts.tv_sec  = cfg->chaos_inject_delay_ms / 1000;
    ts.tv_nsec = (cfg->chaos_inject_delay_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int chaos_should_drop(const proxy_config_t *cfg) {
    if (!cfg->chaos_enabled || cfg->chaos_drop_rate <= 0.0) return 0;

    double r = (double)rand() / (double)RAND_MAX;
    return (r < cfg->chaos_drop_rate) ? 1 : 0;
}
