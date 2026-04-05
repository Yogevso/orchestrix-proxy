/******************************************************************************
 * server_core.c — Listening socket, accept loop, server lifecycle.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdatomic.h>

#include "server_core.h"
#include "config.h"
#include "metrics.h"
#include "http_io.h"
#include "priority.h"

extern const proxy_config_t *g_proxy_cfg;

#define BACKLOG 128

volatile sig_atomic_t g_shutdown = 0;
volatile sig_atomic_t g_reload   = 0;

threadpool   *g_pool = NULL;
atomic_ulong  g_request_id = 0;

__thread unsigned long tls_request_id = 0;

static int g_server_fd = -1;

/* ---------- signal handlers ---------- */

static void sig_shutdown(int sig) {
    (void)sig;
    g_shutdown = 1;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

static void sig_reload(int sig) {
    (void)sig;
    g_reload = 1;
}

void server_install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sig_shutdown;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = sig_reload;
    sigaction(SIGHUP, &sa, NULL);
}

/* ---------- create listening socket ---------- */

int server_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    g_server_fd = fd;
    return fd;
}

/* ---------- accept loop ---------- */

void server_accept_loop(int server_fd, threadpool *pool, dispatch_fn handler) {
    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (!g_shutdown) {
        client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0) {
            if (g_shutdown) break;
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        /* Receive timeout for slow clients. */
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) {
            perror("malloc");
            close(client_fd);
            continue;
        }

        ctx->fd         = client_fd;
        ctx->addr       = client_addr;
        ctx->request_id = atomic_fetch_add(&g_request_id, 1) + 1;

        /* Peek at request line to classify priority. */
        int use_priority = 0;
        if (g_proxy_cfg && g_proxy_cfg->priority_enabled) {
            char peek[512];
            ssize_t pn = recv(client_fd, peek, sizeof(peek) - 1, MSG_PEEK);
            if (pn > 0) {
                peek[pn] = '\0';
                /* Extract path from "METHOD /path HTTP/x.x" */
                char *sp1 = memchr(peek, ' ', pn);
                if (sp1) {
                    sp1++;
                    char *sp2 = memchr(sp1, ' ', pn - (sp1 - peek));
                    if (sp2) {
                        *sp2 = '\0';
                        if (priority_classify(sp1, g_proxy_cfg) == PRIORITY_HIGH)
                            use_priority = 1;
                    }
                }
            }
        }

        int rc = use_priority
            ? dispatch_priority(pool, handler, ctx)
            : dispatch(pool, handler, ctx);

        if (rc < 0) {
            /* Queue full — reject immediately. */
            fprintf(stderr, "[OVERLOAD] Queue full, rejecting fd=%d\n", client_fd);
            http_send_error(client_fd, 503, "Service Unavailable",
                            "Server overloaded. Try again later.", ctx->request_id);
            atomic_fetch_add(&g_metrics.overloaded, 1);
            atomic_fetch_add(&g_metrics.status_5xx, 1);
            atomic_fetch_add(&g_metrics.total_requests, 1);
            close(client_fd);
            free(ctx);
        }
    }
}

/* ---------- utility ---------- */

double elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000.0
         + (now.tv_nsec - start->tv_nsec) / 1e6;
}

void sockaddr_to_ip(const struct sockaddr_in *addr, char *buf, int buf_size) {
    inet_ntop(AF_INET, &addr->sin_addr, buf, buf_size);
}
