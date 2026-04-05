/******************************************************************************
 * config.c — INI-style configuration parser.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"

void config_defaults(proxy_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port                = 8080;
    cfg->admin_port                 = 8081;
    cfg->worker_count               = 4;
    cfg->queue_capacity             = 16;
    cfg->upstream_connect_timeout_ms = 2000;
    cfg->upstream_read_timeout_ms   = 5000;
    cfg->max_request_body_bytes     = 10 * 1024 * 1024;  /* 10 MB */
    cfg->log_format                 = 0;                  /* CLF   */
    strncpy(cfg->mode, "proxy", sizeof(cfg->mode) - 1);

    cfg->lb_strategy                = 0;                  /* round-robin */

    cfg->healthcheck_enabled        = 1;
    cfg->healthcheck_interval_sec   = 5;
    cfg->healthcheck_timeout_ms     = 2000;
    strncpy(cfg->healthcheck_path, "/health", sizeof(cfg->healthcheck_path) - 1);

    cfg->ratelimit_enabled          = 0;
    cfg->ratelimit_per_ip_per_minute = 60;

    cfg->circuit_breaker_enabled    = 0;
    cfg->circuit_breaker_threshold  = 5;
    cfg->circuit_breaker_cooldown_sec = 30;

    cfg->priority_enabled           = 0;
    cfg->priority_prefix_count      = 0;

    cfg->chaos_enabled              = 0;
    cfg->chaos_inject_delay_ms      = 0;
    cfg->chaos_drop_rate            = 0.0;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

int config_load(const char *path, proxy_config_t *cfg) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("config_load");
        return -1;
    }

    config_defaults(cfg);

    char line[MAX_CONFIG_LINE];
    char section[64] = "";

    while (fgets(line, sizeof(line), fp)) {
        char *s = trim(line);

        if (*s == '\0' || *s == '#' || *s == ';') continue;

        /* Section header */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", s + 1);
            }
            continue;
        }

        /* key=value */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcmp(section, "proxy") == 0) {
            if      (strcmp(key, "listen_port") == 0)               cfg->listen_port = atoi(val);
            else if (strcmp(key, "admin_port") == 0)                cfg->admin_port = atoi(val);
            else if (strcmp(key, "worker_count") == 0)              cfg->worker_count = atoi(val);
            else if (strcmp(key, "queue_capacity") == 0)            cfg->queue_capacity = atoi(val);
            else if (strcmp(key, "upstream_connect_timeout_ms") == 0) cfg->upstream_connect_timeout_ms = atoi(val);
            else if (strcmp(key, "upstream_read_timeout_ms") == 0)  cfg->upstream_read_timeout_ms = atoi(val);
            else if (strcmp(key, "max_request_body_bytes") == 0)    cfg->max_request_body_bytes = atoi(val);
            else if (strcmp(key, "log_format") == 0)                cfg->log_format = (strcmp(val, "jsonl") == 0) ? 1 : 0;
            else if (strcmp(key, "mode") == 0)                      snprintf(cfg->mode, sizeof(cfg->mode), "%s", val);
        }
        else if (strcmp(section, "upstreams") == 0) {
            if (cfg->backend_count < MAX_BACKENDS) {
                backend_config_t *b = &cfg->backends[cfg->backend_count];
                snprintf(b->name, sizeof(b->name), "%s", key);
                char *colon = strrchr(val, ':');
                if (colon) {
                    *colon = '\0';
                    snprintf(b->host, sizeof(b->host), "%s", val);
                    b->port = atoi(colon + 1);
                    cfg->backend_count++;
                }
            }
        }
        else if (strcmp(section, "loadbalancer") == 0) {
            if (strcmp(key, "strategy") == 0) {
                cfg->lb_strategy = (strcmp(val, "least_connections") == 0) ? 1 : 0;
            }
        }
        else if (strcmp(section, "healthcheck") == 0) {
            if      (strcmp(key, "enabled") == 0)      cfg->healthcheck_enabled = (strcmp(val, "true") == 0);
            else if (strcmp(key, "interval_sec") == 0)  cfg->healthcheck_interval_sec = atoi(val);
            else if (strcmp(key, "timeout_ms") == 0)    cfg->healthcheck_timeout_ms = atoi(val);
            else if (strcmp(key, "path") == 0)          snprintf(cfg->healthcheck_path, sizeof(cfg->healthcheck_path), "%s", val);
        }
        else if (strcmp(section, "ratelimit") == 0) {
            if      (strcmp(key, "enabled") == 0)         cfg->ratelimit_enabled = (strcmp(val, "true") == 0);
            else if (strcmp(key, "per_ip_per_minute") == 0) cfg->ratelimit_per_ip_per_minute = atoi(val);
        }
        else if (strcmp(section, "circuitbreaker") == 0) {
            if      (strcmp(key, "enabled") == 0)         cfg->circuit_breaker_enabled = (strcmp(val, "true") == 0);
            else if (strcmp(key, "failure_threshold") == 0) cfg->circuit_breaker_threshold = atoi(val);
            else if (strcmp(key, "cooldown_sec") == 0)     cfg->circuit_breaker_cooldown_sec = atoi(val);
        }
        else if (strcmp(section, "priority") == 0) {
            if (strcmp(key, "enabled") == 0)
                cfg->priority_enabled = (strcmp(val, "true") == 0);
            else if (strcmp(key, "prefix") == 0 &&
                     cfg->priority_prefix_count < MAX_PRIORITY_PREFIXES) {
                snprintf(cfg->priority_prefixes[cfg->priority_prefix_count],
                         sizeof(cfg->priority_prefixes[0]), "%s", val);
                cfg->priority_prefix_count++;
            }
        }
        else if (strcmp(section, "chaos") == 0) {
            if      (strcmp(key, "enabled") == 0)          cfg->chaos_enabled = (strcmp(val, "true") == 0);
            else if (strcmp(key, "inject_delay_ms") == 0)  cfg->chaos_inject_delay_ms = atoi(val);
            else if (strcmp(key, "drop_rate") == 0)        cfg->chaos_drop_rate = atof(val);
        }
    }

    fclose(fp);
    return 0;
}

void config_dump(const proxy_config_t *cfg) {
    fprintf(stderr, "=== Orchestrix Proxy Configuration ===\n");
    fprintf(stderr, "  mode:                %s\n", cfg->mode);
    fprintf(stderr, "  listen_port:         %d\n", cfg->listen_port);
    fprintf(stderr, "  admin_port:          %d\n", cfg->admin_port);
    fprintf(stderr, "  workers:             %d\n", cfg->worker_count);
    fprintf(stderr, "  queue_capacity:      %d\n", cfg->queue_capacity);
    fprintf(stderr, "  connect_timeout_ms:  %d\n", cfg->upstream_connect_timeout_ms);
    fprintf(stderr, "  read_timeout_ms:     %d\n", cfg->upstream_read_timeout_ms);
    fprintf(stderr, "  max_body_bytes:      %d\n", cfg->max_request_body_bytes);
    fprintf(stderr, "  log_format:          %s\n", cfg->log_format ? "jsonl" : "clf");
    fprintf(stderr, "  lb_strategy:         %s\n", cfg->lb_strategy ? "least_connections" : "round_robin");
    fprintf(stderr, "  backends (%d):\n", cfg->backend_count);
    for (int i = 0; i < cfg->backend_count; i++)
        fprintf(stderr, "    [%s] %s:%d\n",
                cfg->backends[i].name, cfg->backends[i].host, cfg->backends[i].port);
    fprintf(stderr, "  healthcheck:         %s (interval=%ds, path=%s)\n",
            cfg->healthcheck_enabled ? "on" : "off",
            cfg->healthcheck_interval_sec, cfg->healthcheck_path);
    fprintf(stderr, "  ratelimit:           %s (%d/min/ip)\n",
            cfg->ratelimit_enabled ? "on" : "off", cfg->ratelimit_per_ip_per_minute);
    fprintf(stderr, "  circuit_breaker:     %s (threshold=%d, cooldown=%ds)\n",
            cfg->circuit_breaker_enabled ? "on" : "off",
            cfg->circuit_breaker_threshold, cfg->circuit_breaker_cooldown_sec);
    fprintf(stderr, "  priority:            %s (%d prefixes)\n",
            cfg->priority_enabled ? "on" : "off", cfg->priority_prefix_count);
    for (int i = 0; i < cfg->priority_prefix_count; i++)
        fprintf(stderr, "    high: %s\n", cfg->priority_prefixes[i]);
    fprintf(stderr, "  chaos:               %s (delay=%dms, drop=%.1f%%)\n",
            cfg->chaos_enabled ? "on" : "off",
            cfg->chaos_inject_delay_ms, cfg->chaos_drop_rate * 100);
    fprintf(stderr, "======================================\n");
}
