/******************************************************************************
 * metrics.c — Lock-free atomic counters and observability endpoints.
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "metrics.h"
#include "upstreams.h"

proxy_metrics_t   g_metrics;
backend_metrics_t g_backend_metrics[MAX_BACKENDS];

void metrics_init(void) {
    memset(&g_metrics, 0, sizeof(g_metrics));
    memset(g_backend_metrics, 0, sizeof(g_backend_metrics));
    g_metrics.start_time = time(NULL);
}

void metrics_record_request(int status, double latency_ms, double upstream_latency_ms) {
    atomic_fetch_add(&g_metrics.total_requests, 1);

    unsigned long lat_us = (unsigned long)(latency_ms * 1000.0);
    atomic_fetch_add(&g_metrics.total_latency_us, lat_us);

    unsigned long up_us = (unsigned long)(upstream_latency_ms * 1000.0);
    atomic_fetch_add(&g_metrics.upstream_latency_us, up_us);

    if      (status >= 200 && status < 300) atomic_fetch_add(&g_metrics.status_2xx, 1);
    else if (status >= 300 && status < 400) atomic_fetch_add(&g_metrics.status_3xx, 1);
    else if (status >= 400 && status < 500) atomic_fetch_add(&g_metrics.status_4xx, 1);
    else if (status >= 500)                 atomic_fetch_add(&g_metrics.status_5xx, 1);

    if (status >= 200 && status < 400)
        atomic_fetch_add(&g_metrics.proxy_success, 1);
}

void metrics_record_backend(int backend_idx, int failed) {
    if (backend_idx < 0 || backend_idx >= MAX_BACKENDS) return;
    atomic_fetch_add(&g_backend_metrics[backend_idx].requests, 1);
    if (failed)
        atomic_fetch_add(&g_backend_metrics[backend_idx].failures, 1);
}

int metrics_render_stats_json(char *buf, int buf_size,
                              int pool_qsize, int pool_qmax, int pool_threads) {
    time_t now      = time(NULL);
    long   uptime   = (long)(now - g_metrics.start_time);
    unsigned long total    = atomic_load(&g_metrics.total_requests);
    unsigned long active   = atomic_load(&g_metrics.active_connections);
    unsigned long s2xx     = atomic_load(&g_metrics.status_2xx);
    unsigned long s3xx     = atomic_load(&g_metrics.status_3xx);
    unsigned long s4xx     = atomic_load(&g_metrics.status_4xx);
    unsigned long s5xx     = atomic_load(&g_metrics.status_5xx);
    unsigned long overload = atomic_load(&g_metrics.overloaded);
    unsigned long rl       = atomic_load(&g_metrics.rate_limited);
    unsigned long lat_us   = atomic_load(&g_metrics.total_latency_us);
    unsigned long psuccess = atomic_load(&g_metrics.proxy_success);
    unsigned long ufail    = atomic_load(&g_metrics.upstream_failures);

    double avg_lat = total > 0 ? (lat_us / 1000.0) / total : 0.0;

    /* Build backends array. */
    char backends_json[4096] = "";
    int  boff = 0;
    for (int i = 0; i < g_upstreams.count; i++) {
        backend_t *b = &g_upstreams.backends[i];
        int st = atomic_load(&b->state);
        unsigned long breq  = atomic_load(&g_backend_metrics[i].requests);
        unsigned long bfail = atomic_load(&g_backend_metrics[i].failures);
        int bactive = atomic_load(&b->active_connections);
        const char *state_str = (st == BACKEND_UP) ? "up" :
                                (st == BACKEND_DRAINING) ? "draining" : "down";

        boff += snprintf(backends_json + boff, sizeof(backends_json) - boff,
            "%s    {\"name\":\"%s\",\"state\":\"%s\",\"active\":%d,"
            "\"requests\":%lu,\"failures\":%lu}",
            i > 0 ? ",\n" : "", b->name, state_str, bactive, breq, bfail);
    }

    return snprintf(buf, buf_size,
        "{\n"
        "  \"uptime_seconds\": %ld,\n"
        "  \"total_requests\": %lu,\n"
        "  \"active_connections\": %lu,\n"
        "  \"pool_threads\": %d,\n"
        "  \"queue_depth\": %d,\n"
        "  \"queue_capacity\": %d,\n"
        "  \"avg_latency_ms\": %.3f,\n"
        "  \"status\": { \"2xx\": %lu, \"3xx\": %lu, \"4xx\": %lu, \"5xx\": %lu },\n"
        "  \"overloaded_connections\": %lu,\n"
        "  \"rate_limited_requests\": %lu,\n"
        "  \"proxy_success\": %lu,\n"
        "  \"upstream_failures\": %lu,\n"
        "  \"backends\": [\n%s\n  ]\n"
        "}\n",
        uptime, total, active, pool_threads, pool_qsize, pool_qmax,
        avg_lat, s2xx, s3xx, s4xx, s5xx, overload, rl,
        psuccess, ufail, backends_json);
}

int metrics_render_prometheus(char *buf, int buf_size) {
    time_t now    = time(NULL);
    long   uptime = (long)(now - g_metrics.start_time);
    int off = 0;

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_uptime_seconds Seconds since proxy started\n"
        "# TYPE proxy_uptime_seconds gauge\n"
        "proxy_uptime_seconds %ld\n", uptime);

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_requests_total Total proxied requests\n"
        "# TYPE proxy_requests_total counter\n"
        "proxy_requests_total %lu\n", atomic_load(&g_metrics.total_requests));

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_active_connections Current in-flight connections\n"
        "# TYPE proxy_active_connections gauge\n"
        "proxy_active_connections %lu\n", atomic_load(&g_metrics.active_connections));

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_status_total Responses by status class\n"
        "# TYPE proxy_status_total counter\n"
        "proxy_status_total{class=\"2xx\"} %lu\n"
        "proxy_status_total{class=\"3xx\"} %lu\n"
        "proxy_status_total{class=\"4xx\"} %lu\n"
        "proxy_status_total{class=\"5xx\"} %lu\n",
        atomic_load(&g_metrics.status_2xx),
        atomic_load(&g_metrics.status_3xx),
        atomic_load(&g_metrics.status_4xx),
        atomic_load(&g_metrics.status_5xx));

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_overloaded_total 503 overload rejections\n"
        "# TYPE proxy_overloaded_total counter\n"
        "proxy_overloaded_total %lu\n", atomic_load(&g_metrics.overloaded));

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_rate_limited_total 429 rate-limited rejections\n"
        "# TYPE proxy_rate_limited_total counter\n"
        "proxy_rate_limited_total %lu\n", atomic_load(&g_metrics.rate_limited));

    /* Per-backend metrics. */
    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_backend_requests_total Requests forwarded per backend\n"
        "# TYPE proxy_backend_requests_total counter\n");
    for (int i = 0; i < g_upstreams.count; i++) {
        off += snprintf(buf + off, buf_size - off,
            "proxy_backend_requests_total{backend=\"%s\"} %lu\n",
            g_upstreams.backends[i].name,
            atomic_load(&g_backend_metrics[i].requests));
    }

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_backend_failures_total Failed requests per backend\n"
        "# TYPE proxy_backend_failures_total counter\n");
    for (int i = 0; i < g_upstreams.count; i++) {
        off += snprintf(buf + off, buf_size - off,
            "proxy_backend_failures_total{backend=\"%s\"} %lu\n",
            g_upstreams.backends[i].name,
            atomic_load(&g_backend_metrics[i].failures));
    }

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_backend_healthy Backend health state (1=up, 0=down)\n"
        "# TYPE proxy_backend_healthy gauge\n");
    for (int i = 0; i < g_upstreams.count; i++) {
        int st = atomic_load(&g_upstreams.backends[i].state);
        off += snprintf(buf + off, buf_size - off,
            "proxy_backend_healthy{backend=\"%s\"} %d\n",
            g_upstreams.backends[i].name,
            st == BACKEND_UP ? 1 : 0);
    }

    off += snprintf(buf + off, buf_size - off,
        "# HELP proxy_backend_active_connections Current active connections per backend\n"
        "# TYPE proxy_backend_active_connections gauge\n");
    for (int i = 0; i < g_upstreams.count; i++) {
        off += snprintf(buf + off, buf_size - off,
            "proxy_backend_active_connections{backend=\"%s\"} %d\n",
            g_upstreams.backends[i].name,
            atomic_load(&g_upstreams.backends[i].active_connections));
    }

    return off;
}
