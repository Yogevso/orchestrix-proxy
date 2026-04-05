/******************************************************************************
 * admin_handler.c — Admin API on a separate port for runtime control.
 *
 * Runs a lightweight accept loop on a dedicated thread, serving simple
 * HTTP endpoints for backend management and config reload.
 *
 * Endpoints:
 *   GET  /admin/config                  — dump running configuration
 *   PUT  /admin/backends/<name>/drain   — set backend to DRAINING
 *   PUT  /admin/backends/<name>/enable  — set backend to UP
 *   PUT  /admin/reload                  — re-read config from disk
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "admin_handler.h"
#include "config.h"
#include "upstreams.h"
#include "http_io.h"
#include "server_core.h"

static pthread_t  admin_thread;
static volatile int admin_running = 0;
static int        admin_fd = -1;
static proxy_config_t *admin_cfg = NULL;
static char       admin_config_path[512];

/* ---- simple response helpers ---- */

static void admin_json_response(int fd, int code, const char *reason,
                                const char *json_body) {
    int blen = (int)strlen(json_body);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        code, reason, blen);
    http_write_all(fd, header, hlen);
    http_write_all(fd, json_body, blen);
}

/* ---- request handling ---- */

static void handle_admin_request(int client_fd) {
    char buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    char method[16], path[512], proto[16];
    if (sscanf(buf, "%15s %511s %15s", method, path, proto) != 3) {
        admin_json_response(client_fd, 400, "Bad Request",
                            "{\"error\":\"malformed request\"}\n");
        close(client_fd);
        return;
    }

    /* GET /admin/config */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/admin/config") == 0) {
        char body[4096];
        int off = 0;
        off += snprintf(body + off, sizeof(body) - off,
            "{\n  \"listen_port\": %d,\n  \"admin_port\": %d,\n"
            "  \"worker_count\": %d,\n  \"queue_capacity\": %d,\n"
            "  \"mode\": \"%s\",\n  \"lb_strategy\": \"%s\",\n"
            "  \"backend_count\": %d,\n  \"backends\": [\n",
            admin_cfg->listen_port, admin_cfg->admin_port,
            admin_cfg->worker_count, admin_cfg->queue_capacity,
            admin_cfg->mode,
            admin_cfg->lb_strategy ? "least_connections" : "round_robin",
            admin_cfg->backend_count);

        for (int i = 0; i < admin_cfg->backend_count; i++) {
            off += snprintf(body + off, sizeof(body) - off,
                "    {\"name\":\"%s\",\"host\":\"%s\",\"port\":%d}%s\n",
                admin_cfg->backends[i].name,
                admin_cfg->backends[i].host,
                admin_cfg->backends[i].port,
                i < admin_cfg->backend_count - 1 ? "," : "");
        }
        off += snprintf(body + off, sizeof(body) - off, "  ]\n}\n");

        admin_json_response(client_fd, 200, "OK", body);
        close(client_fd);
        return;
    }

    /* PUT /admin/backends/<name>/drain */
    if (strcmp(method, "PUT") == 0) {
        char name[64];

        if (sscanf(path, "/admin/backends/%63[^/]/drain", name) == 1) {
            int idx = upstreams_find(name);
            if (idx < 0) {
                admin_json_response(client_fd, 404, "Not Found",
                                    "{\"error\":\"backend not found\"}\n");
            } else {
                upstreams_set_state(idx, BACKEND_DRAINING);
                char body[128];
                snprintf(body, sizeof(body),
                         "{\"status\":\"ok\",\"backend\":\"%s\",\"state\":\"draining\"}\n",
                         name);
                admin_json_response(client_fd, 200, "OK", body);
            }
            close(client_fd);
            return;
        }

        if (sscanf(path, "/admin/backends/%63[^/]/enable", name) == 1) {
            int idx = upstreams_find(name);
            if (idx < 0) {
                admin_json_response(client_fd, 404, "Not Found",
                                    "{\"error\":\"backend not found\"}\n");
            } else {
                upstreams_set_state(idx, BACKEND_UP);
                char body[128];
                snprintf(body, sizeof(body),
                         "{\"status\":\"ok\",\"backend\":\"%s\",\"state\":\"up\"}\n",
                         name);
                admin_json_response(client_fd, 200, "OK", body);
            }
            close(client_fd);
            return;
        }

        /* PUT /admin/reload */
        if (strcmp(path, "/admin/reload") == 0) {
            proxy_config_t new_cfg;
            if (config_load(admin_config_path, &new_cfg) == 0) {
                upstreams_reload(&new_cfg);
                /* Update live config for ratelimit / health-check changes. */
                admin_cfg->ratelimit_per_ip_per_minute = new_cfg.ratelimit_per_ip_per_minute;
                admin_cfg->healthcheck_interval_sec    = new_cfg.healthcheck_interval_sec;
                admin_cfg->lb_strategy                 = new_cfg.lb_strategy;
                admin_json_response(client_fd, 200, "OK",
                                    "{\"status\":\"ok\",\"action\":\"config reloaded\"}\n");
            } else {
                admin_json_response(client_fd, 500, "Internal Server Error",
                                    "{\"error\":\"failed to parse config\"}\n");
            }
            close(client_fd);
            return;
        }
    }

    admin_json_response(client_fd, 404, "Not Found",
                        "{\"error\":\"unknown admin endpoint\"}\n");
    close(client_fd);
}

/* ---- admin loop ---- */

static void *admin_loop(void *arg) {
    (void)arg;
    while (admin_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(admin_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (!admin_running) break;
            if (errno == EINTR) continue;
            continue;
        }

        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_admin_request(cfd);
    }
    return NULL;
}

/* ---- public API ---- */

int admin_start(proxy_config_t *cfg, const char *config_path) {
    admin_cfg = cfg;
    snprintf(admin_config_path, sizeof(admin_config_path), "%s", config_path);

    admin_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (admin_fd < 0) { perror("admin socket"); return -1; }

    int opt = 1;
    setsockopt(admin_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* admin binds only to localhost */
    addr.sin_port        = htons(cfg->admin_port);

    if (bind(admin_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("admin bind");
        close(admin_fd);
        admin_fd = -1;
        return -1;
    }

    if (listen(admin_fd, 8) < 0) {
        perror("admin listen");
        close(admin_fd);
        admin_fd = -1;
        return -1;
    }

    admin_running = 1;
    if (pthread_create(&admin_thread, NULL, admin_loop, NULL) != 0) {
        perror("admin thread");
        admin_running = 0;
        close(admin_fd);
        admin_fd = -1;
        return -1;
    }
    pthread_detach(admin_thread);

    fprintf(stderr, "[ADMIN] Listening on 127.0.0.1:%d\n", cfg->admin_port);
    return 0;
}

void admin_stop(void) {
    admin_running = 0;
    if (admin_fd >= 0) {
        close(admin_fd);
        admin_fd = -1;
    }
}
