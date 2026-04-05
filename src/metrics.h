/******************************************************************************
 * metrics.h — Lock-free atomic counters and observability endpoints.
 ******************************************************************************/

#ifndef METRICS_H
#define METRICS_H

#include <stdatomic.h>
#include <time.h>
#include "config.h"

typedef struct {
    atomic_ulong total_requests;
    atomic_ulong active_connections;
    atomic_ulong status_2xx;
    atomic_ulong status_3xx;
    atomic_ulong status_4xx;
    atomic_ulong status_5xx;
    atomic_ulong overloaded;
    atomic_ulong rate_limited;
    atomic_ulong bytes_sent;
    atomic_ulong total_latency_us;
    atomic_ulong upstream_latency_us;
    atomic_ulong proxy_success;
    atomic_ulong upstream_failures;
    time_t       start_time;
} proxy_metrics_t;

/* Per-backend counters (indexed same as upstream pool). */
typedef struct {
    atomic_ulong requests;
    atomic_ulong failures;
} backend_metrics_t;

/* Global metrics singleton. */
extern proxy_metrics_t  g_metrics;
extern backend_metrics_t g_backend_metrics[MAX_BACKENDS];

void metrics_init(void);
void metrics_record_request(int status, double latency_ms, double upstream_latency_ms);
void metrics_record_backend(int backend_idx, int failed);

/* Render GET /stats response body into buf.  Returns length written. */
int  metrics_render_stats_json(char *buf, int buf_size,
                               int pool_qsize, int pool_qmax, int pool_threads);

/* Render GET /metrics (Prometheus exposition format).  Returns length. */
int  metrics_render_prometheus(char *buf, int buf_size);

#endif /* METRICS_H */
