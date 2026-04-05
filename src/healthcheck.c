/******************************************************************************
 * healthcheck.c — Periodic backend health probes.
 *
 * Runs a background thread that periodically connects to each backend's
 * health-check path.  A successful 2xx response marks the backend UP;
 * a failed connect/timeout/non-2xx marks it DOWN.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "healthcheck.h"
#include "upstreams.h"
#include "http_io.h"
#include "http_parse.h"

static pthread_t hc_thread;
static volatile int hc_running = 0;

/* Local copy of config values needed by the health-check loop. */
static int  hc_interval_sec;
static int  hc_timeout_ms;
static char hc_path[256];

static void probe_backend(int idx) {
    backend_t *b = &g_upstreams.backends[idx];

    int st = atomic_load(&b->state);
    if (st == BACKEND_DRAINING) return;   /* don't probe draining backends */

    int fd = http_connect_upstream(b->host, b->port, hc_timeout_ms);
    if (fd < 0) {
        if (st == BACKEND_UP) {
            fprintf(stderr, "[HEALTHCHECK] %s FAILED (connect)\n", b->name);
            upstreams_set_state(idx, BACKEND_DOWN);
        }
        return;
    }

    /* Send a minimal GET request. */
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        hc_path, b->host, b->port);

    if (http_write_all(fd, req, rlen) < 0) {
        close(fd);
        if (st == BACKEND_UP) {
            fprintf(stderr, "[HEALTHCHECK] %s FAILED (write)\n", b->name);
            upstreams_set_state(idx, BACKEND_DOWN);
        }
        return;
    }

    /* Read response — we only need the status line. */
    struct timeval tv = { .tv_sec = hc_timeout_ms / 1000,
                          .tv_usec = (hc_timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        if (st == BACKEND_UP) {
            fprintf(stderr, "[HEALTHCHECK] %s FAILED (read)\n", b->name);
            upstreams_set_state(idx, BACKEND_DOWN);
        }
        return;
    }
    buf[n] = '\0';

    /* Parse status code from first line: "HTTP/1.x NNN ..." */
    int status = 0;
    char proto[16];
    if (sscanf(buf, "%15s %d", proto, &status) < 2 || status < 200 || status >= 300) {
        if (st == BACKEND_UP) {
            fprintf(stderr, "[HEALTHCHECK] %s FAILED (status=%d)\n", b->name, status);
            upstreams_set_state(idx, BACKEND_DOWN);
        }
        return;
    }

    /* Success. */
    if (st == BACKEND_DOWN) {
        fprintf(stderr, "[HEALTHCHECK] %s RECOVERED\n", b->name);
        upstreams_set_state(idx, BACKEND_UP);
    }
}

static void *hc_loop(void *arg) {
    (void)arg;
    while (hc_running) {
        for (int i = 0; i < g_upstreams.count && hc_running; i++)
            probe_backend(i);

        /* Sleep in 1-second increments so we can exit quickly. */
        for (int s = 0; s < hc_interval_sec && hc_running; s++)
            sleep(1);
    }
    return NULL;
}

int healthcheck_start(const proxy_config_t *cfg) {
    if (!cfg->healthcheck_enabled) return 0;

    hc_interval_sec = cfg->healthcheck_interval_sec;
    hc_timeout_ms   = cfg->healthcheck_timeout_ms;
    snprintf(hc_path, sizeof(hc_path), "%s", cfg->healthcheck_path);

    hc_running = 1;
    if (pthread_create(&hc_thread, NULL, hc_loop, NULL) != 0) {
        perror("healthcheck thread");
        hc_running = 0;
        return -1;
    }
    pthread_detach(hc_thread);
    fprintf(stderr, "[HEALTHCHECK] Started (interval=%ds, path=%s)\n",
            hc_interval_sec, hc_path);
    return 0;
}

void healthcheck_stop(void) {
    hc_running = 0;
    /* Thread is detached — it will exit on next iteration. */
}
