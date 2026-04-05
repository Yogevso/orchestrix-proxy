/******************************************************************************
 * proxy_handler.c — Core reverse-proxy request handler.
 *
 * Called by a worker thread for each accepted connection in proxy mode.
 * Parses the client request, selects a healthy backend, forwards the
 * request, relays the response, handles failover, and records metrics.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>

#include "proxy_handler.h"
#include "server_core.h"
#include "http_parse.h"
#include "http_io.h"
#include "upstreams.h"
#include "metrics.h"
#include "logger.h"
#include "rate_limit.h"
#include "config.h"
#include "chaos.h"
#include "chaos.h"

/* Shared config pointer — set by main(). */
extern const proxy_config_t *g_proxy_cfg;

/* ---------- internal endpoints ---------- */

static void serve_stats(int fd, unsigned long rid) {
    char body[8192];
    int qsize = 0, qmax = 0, nthreads = 0;
    if (g_pool) {
        pthread_mutex_lock(&g_pool->qlock);
        qsize    = g_pool->qsize;
        qmax     = g_pool->max_qsize;
        nthreads = g_pool->num_threads;
        pthread_mutex_unlock(&g_pool->qlock);
    }
    int blen = metrics_render_stats_json(body, sizeof(body), qsize, qmax, nthreads);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Server: orchestrix-proxy/1.0\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "X-Request-Id: %lu\r\n"
        "Connection: close\r\n\r\n",
        blen, rid);

    http_write_all(fd, header, hlen);
    http_write_all(fd, body, blen);
}

static void serve_health(int fd, unsigned long rid) {
    time_t now = time(NULL);
    long uptime = (long)(now - g_metrics.start_time);

    char body[128];
    int blen = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"uptime_seconds\":%ld}\n", uptime);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Server: orchestrix-proxy/1.0\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "X-Request-Id: %lu\r\n"
        "Connection: close\r\n\r\n",
        blen, rid);

    http_write_all(fd, header, hlen);
    http_write_all(fd, body, blen);
}

static void serve_metrics(int fd, unsigned long rid) {
    char body[8192];
    int blen = metrics_render_prometheus(body, sizeof(body));

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Server: orchestrix-proxy/1.0\r\n"
        "Content-Type: text/plain; version=0.0.4\r\n"
        "Content-Length: %d\r\n"
        "X-Request-Id: %lu\r\n"
        "Connection: close\r\n\r\n",
        blen, rid);

    http_write_all(fd, header, hlen);
    http_write_all(fd, body, blen);
}

/* ---------- proxy handler ---------- */

int proxy_handle_client(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int client_fd            = ctx->fd;
    struct sockaddr_in caddr = ctx->addr;
    unsigned long rid        = ctx->request_id;
    free(ctx);

    tls_request_id = rid;
    atomic_fetch_add(&g_metrics.active_connections, 1);

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    char client_ip[INET_ADDRSTRLEN];
    sockaddr_to_ip(&caddr, client_ip, sizeof(client_ip));

    /* ---- read client request ---- */
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.content_length = -1;

    ssize_t n = read(client_fd, req.raw, sizeof(req.raw) - 1);
    if (n <= 0) {
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }
    req.raw_len = (int)n;
    req.raw[n]  = '\0';

    if (http_parse_request(&req) != 0) {
        http_send_error(client_fd, 400, "Bad Request",
                        "Malformed HTTP request.", rid);
        metrics_record_request(400, elapsed_ms(&t_start), 0);
        log_access(client_ip, "-", "-", 400, elapsed_ms(&t_start), 0, NULL, rid);
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }

    /* ---- built-in endpoints ---- */
    if (strcmp(req.path, "/stats") == 0) {
        serve_stats(client_fd, rid);
        metrics_record_request(200, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 200,
                   elapsed_ms(&t_start), 0, NULL, rid);
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }
    if (strcmp(req.path, "/health") == 0) {
        serve_health(client_fd, rid);
        metrics_record_request(200, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 200,
                   elapsed_ms(&t_start), 0, NULL, rid);
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }
    if (strcmp(req.path, "/metrics") == 0) {
        serve_metrics(client_fd, rid);
        metrics_record_request(200, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 200,
                   elapsed_ms(&t_start), 0, NULL, rid);
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }

    /* ---- rate limiting ---- */
    if (g_proxy_cfg->ratelimit_enabled && !ratelimit_allow(client_ip)) {
        http_send_error(client_fd, 429, "Too Many Requests",
                        "Rate limit exceeded. Try again later.", rid);
        atomic_fetch_add(&g_metrics.rate_limited, 1);
        metrics_record_request(429, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 429,
                   elapsed_ms(&t_start), 0, NULL, rid);
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }

    /* ---- request body size check ---- */
    if (req.content_length > g_proxy_cfg->max_request_body_bytes) {
        http_send_error(client_fd, 413, "Payload Too Large",
                        "Request body too large.", rid);
        metrics_record_request(413, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 413,
                   elapsed_ms(&t_start), 0, NULL, rid);
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }

    /* ---- chaos injection ---- */
    if (g_proxy_cfg->chaos_enabled) {
        if (chaos_should_drop(g_proxy_cfg)) {
            http_send_error(client_fd, 500, "Internal Server Error",
                            "[CHAOS] Simulated failure.", rid);
            metrics_record_request(500, elapsed_ms(&t_start), 0);
            log_access(client_ip, req.method, req.path, 500,
                       elapsed_ms(&t_start), 0, "chaos-drop", rid);
            atomic_fetch_sub(&g_metrics.active_connections, 1);
            close(client_fd);
            return 0;
        }
        chaos_inject_delay(g_proxy_cfg);
    }

    /* ---- select backend + failover loop ---- */
    int tried = 0;
    int max_tries = g_upstreams.count;
    int status_code = 502;
    double upstream_lat = 0;
    const char *backend_name = NULL;

    while (tried < max_tries) {
        int idx = upstreams_select(g_proxy_cfg);
        if (idx < 0) break;

        backend_t *b = &g_upstreams.backends[idx];
        backend_name = b->name;

        atomic_fetch_add(&b->active_connections, 1);

        struct timespec t_up;
        clock_gettime(CLOCK_MONOTONIC, &t_up);

        /* Connect to upstream. */
        int upstream_fd = http_connect_upstream(
            b->host, b->port, g_proxy_cfg->upstream_connect_timeout_ms);

        if (upstream_fd < 0) {
            atomic_fetch_sub(&b->active_connections, 1);
            upstreams_mark_failure(idx, g_proxy_cfg);
            metrics_record_backend(idx, 1);
            atomic_fetch_add(&g_metrics.upstream_failures, 1);
            tried++;
            continue;
        }

        /* Forward request. */
        if (http_forward_request(upstream_fd, &req, client_ip) < 0) {
            close(upstream_fd);
            atomic_fetch_sub(&b->active_connections, 1);
            upstreams_mark_failure(idx, g_proxy_cfg);
            metrics_record_backend(idx, 1);
            atomic_fetch_add(&g_metrics.upstream_failures, 1);
            tried++;
            continue;
        }

        /* Relay response. */
        long relayed = http_relay_response(
            client_fd, upstream_fd,
            g_proxy_cfg->upstream_read_timeout_ms,
            rid, &status_code);

        upstream_lat = elapsed_ms(&t_up);
        close(upstream_fd);
        atomic_fetch_sub(&b->active_connections, 1);

        if (relayed < 0) {
            upstreams_mark_failure(idx, g_proxy_cfg);
            metrics_record_backend(idx, 1);
            atomic_fetch_add(&g_metrics.upstream_failures, 1);
            tried++;
            continue;
        }

        /* Success. */
        upstreams_mark_success(idx);
        metrics_record_backend(idx, 0);
        atomic_fetch_add(&g_metrics.bytes_sent, (unsigned long)relayed);
        metrics_record_request(status_code, elapsed_ms(&t_start), upstream_lat);
        log_access(client_ip, req.method, req.path, status_code,
                   elapsed_ms(&t_start), upstream_lat, backend_name, rid);
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }

    /* All backends failed or none available. */
    int err_code = (g_upstreams.count == 0) ? 503 : 502;
    const char *err_reason = (err_code == 503) ? "Service Unavailable" : "Bad Gateway";
    const char *err_body   = (err_code == 503)
        ? "No healthy upstream backends available."
        : "All upstream backends failed.";

    http_send_error(client_fd, err_code, err_reason, err_body, rid);
    metrics_record_request(err_code, elapsed_ms(&t_start), upstream_lat);
    log_access(client_ip, req.method, req.path, err_code,
               elapsed_ms(&t_start), upstream_lat, backend_name, rid);
    atomic_fetch_sub(&g_metrics.active_connections, 1);
    close(client_fd);
    return 0;
}
