/******************************************************************************
 * config.h — INI-style configuration parser for Orchestrix Proxy.
 ******************************************************************************/

#ifndef CONFIG_H
#define CONFIG_H

#define MAX_BACKENDS        16
#define MAX_CONFIG_LINE     512
#define MAX_CONFIG_VALUE    256
#define MAX_PRIORITY_PREFIXES 8

typedef struct {
    char name[64];
    char host[64];
    int  port;
} backend_config_t;

typedef struct {
    /* [proxy] */
    int  listen_port;
    int  admin_port;
    int  worker_count;
    int  queue_capacity;
    int  upstream_connect_timeout_ms;
    int  upstream_read_timeout_ms;
    int  max_request_body_bytes;
    int  log_format;                     /* 0 = CLF, 1 = JSONL */
    char mode[16];                       /* "proxy" or "static" */

    /* [upstreams] */
    int              backend_count;
    backend_config_t backends[MAX_BACKENDS];

    /* [loadbalancer] */
    int  lb_strategy;                    /* 0 = round_robin, 1 = least_connections */

    /* [healthcheck] */
    int  healthcheck_enabled;
    int  healthcheck_interval_sec;
    int  healthcheck_timeout_ms;
    char healthcheck_path[256];

    /* [ratelimit] */
    int  ratelimit_enabled;
    int  ratelimit_per_ip_per_minute;

    /* [circuitbreaker] */
    int  circuit_breaker_enabled;
    int  circuit_breaker_threshold;
    int  circuit_breaker_cooldown_sec;

    /* [priority] */
    int  priority_enabled;
    int  priority_prefix_count;
    char priority_prefixes[MAX_PRIORITY_PREFIXES][128];

    /* [chaos] */
    int  chaos_enabled;
    int  chaos_inject_delay_ms;
    double chaos_drop_rate;              /* 0.0 – 1.0 */
} proxy_config_t;

/* Parse config from an INI file.  Returns 0 on success, -1 on error. */
int  config_load(const char *path, proxy_config_t *cfg);

/* Fill *cfg with sane defaults. */
void config_defaults(proxy_config_t *cfg);

/* Print resolved config to stderr. */
void config_dump(const proxy_config_t *cfg);

#endif /* CONFIG_H */
