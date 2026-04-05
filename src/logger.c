/******************************************************************************
 * logger.c — Access logging (CLF and JSONL formats).
 ******************************************************************************/

#include <stdio.h>
#include <time.h>
#include <string.h>
#include "logger.h"

static int g_log_format;   /* 0 = CLF, 1 = JSONL */

void logger_init(int format) {
    g_log_format = format;
}

void log_access(const char *client_ip,
                const char *method,
                const char *path,
                int         status,
                double      latency_ms,
                double      upstream_latency_ms,
                const char *backend_name,
                unsigned long request_id) {
    char timebuf[64];
    time_t now = time(NULL);

    if (g_log_format == 1) {
        /* JSONL */
        struct tm *tm = gmtime(&now);
        char iso[32];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", tm);

        printf("{\"ts\":\"%s\",\"req_id\":%lu,\"method\":\"%s\","
               "\"path\":\"%s\",\"client_ip\":\"%s\","
               "\"backend\":\"%s\",\"status\":%d,"
               "\"latency_us\":%.0f,\"upstream_latency_us\":%.0f}\n",
               iso, request_id, method, path, client_ip,
               backend_name ? backend_name : "-",
               status,
               latency_ms * 1000.0,
               upstream_latency_ms * 1000.0);
    } else {
        /* CLF-like */
        struct tm *tm = gmtime(&now);
        strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S +0000", tm);

        printf("%s - - [%s] \"%s %s\" %d %.2fms",
               client_ip, timebuf, method, path, status, latency_ms);

        if (backend_name)
            printf(" backend=%s upstream=%.2fms", backend_name, upstream_latency_ms);

        printf(" rid=%lu\n", request_id);
    }
    fflush(stdout);
}
