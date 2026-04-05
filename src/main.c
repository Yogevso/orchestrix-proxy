/******************************************************************************
 * main.c — Orchestrix Proxy entry point.
 *
 * Usage:
 *   ./proxy proxy.conf            (proxy mode — from config file)
 *   ./proxy --static <port> <pool> <queue>     (legacy static file server)
 *
 * In proxy mode the config file drives all behaviour.
 * In static mode the old command-line interface is preserved.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "config.h"
#include "threadpool.h"
#include "server_core.h"
#include "metrics.h"
#include "logger.h"
#include "upstreams.h"
#include "healthcheck.h"
#include "rate_limit.h"
#include "admin_handler.h"
#include "proxy_handler.h"
#include "static_handler.h"
#include "chaos.h"

/* Global config pointer shared with proxy_handler. */
const proxy_config_t *g_proxy_cfg = NULL;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s <config-file>                      Proxy mode (read config from file)\n"
        "  %s --static <port> <pool> <queue>     Legacy static file-server mode\n",
        prog, prog);
}

int main(int argc, char *argv[]) {
    proxy_config_t cfg;
    const char *config_path = NULL;
    int static_mode = 0;

    /* ---- parse arguments ---- */
    if (argc == 2) {
        /* proxy mode: single arg = config file */
        config_path = argv[1];
        if (config_load(config_path, &cfg) != 0) {
            fprintf(stderr, "Failed to load config: %s\n", config_path);
            return EXIT_FAILURE;
        }
    } else if (argc >= 2 && strcmp(argv[1], "--static") == 0) {
        if (argc != 5) { usage(argv[0]); return EXIT_FAILURE; }
        static_mode = 1;
        config_defaults(&cfg);
        cfg.listen_port   = atoi(argv[2]);
        cfg.worker_count  = atoi(argv[3]);
        cfg.queue_capacity = atoi(argv[4]);
        strncpy(cfg.mode, "static", sizeof(cfg.mode) - 1);
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(cfg.mode, "static") == 0) static_mode = 1;

    g_proxy_cfg = &cfg;
    config_dump(&cfg);

    /* ---- install signal handlers ---- */
    server_install_signals();

    /* ---- init subsystems ---- */
    metrics_init();
    logger_init(cfg.log_format);
    chaos_init();

    if (!static_mode) {
        upstreams_init(&cfg);

        if (cfg.ratelimit_enabled)
            ratelimit_init(cfg.ratelimit_per_ip_per_minute);
    }

    /* ---- create thread pool ---- */
    threadpool *pool = create_threadpool(cfg.worker_count, cfg.queue_capacity);
    if (!pool) {
        fprintf(stderr, "Failed to create thread pool\n");
        return EXIT_FAILURE;
    }
    g_pool = pool;

    /* ---- create listening socket ---- */
    int server_fd = server_listen(cfg.listen_port);
    if (server_fd < 0) {
        destroy_threadpool(pool);
        return EXIT_FAILURE;
    }

    /* ---- start background services (proxy mode only) ---- */
    if (!static_mode) {
        healthcheck_start(&cfg);
        admin_start((proxy_config_t *)&cfg, config_path ? config_path : "");
    }

    fprintf(stderr, "orchestrix-proxy/1.0 listening on port %d [%s mode] "
            "(threads=%d, queue=%d)\n",
            cfg.listen_port, static_mode ? "static" : "proxy",
            cfg.worker_count, cfg.queue_capacity);

    /* ---- SIGHUP reload support ---- */
    dispatch_fn handler = static_mode
        ? (dispatch_fn)static_handle_client
        : (dispatch_fn)proxy_handle_client;

    /* Run the accept loop. */
    while (!g_shutdown) {
        server_accept_loop(server_fd, pool, handler);

        /* Check for config reload (SIGHUP). */
        if (g_reload && !static_mode && config_path) {
            g_reload = 0;
            fprintf(stderr, "[SIGHUP] Reloading config from %s\n", config_path);
            proxy_config_t new_cfg;
            if (config_load(config_path, &new_cfg) == 0) {
                upstreams_reload(&new_cfg);
                /* Update rate limit. */
                if (new_cfg.ratelimit_enabled) {
                    ratelimit_destroy();
                    ratelimit_init(new_cfg.ratelimit_per_ip_per_minute);
                }
                ((proxy_config_t *)g_proxy_cfg)->lb_strategy = new_cfg.lb_strategy;
                ((proxy_config_t *)g_proxy_cfg)->ratelimit_enabled = new_cfg.ratelimit_enabled;
                ((proxy_config_t *)g_proxy_cfg)->ratelimit_per_ip_per_minute = new_cfg.ratelimit_per_ip_per_minute;
                fprintf(stderr, "[SIGHUP] Config reloaded successfully\n");
            } else {
                fprintf(stderr, "[SIGHUP] Failed to reload config\n");
            }
        }
    }

    /* ---- shutdown ---- */
    fprintf(stderr, "\nShutting down...\n");
    healthcheck_stop();
    admin_stop();
    destroy_threadpool(pool);
    ratelimit_destroy();

    if (server_fd >= 0) close(server_fd);
    fprintf(stderr, "Goodbye.\n");
    return 0;
}
