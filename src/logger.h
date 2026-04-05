/******************************************************************************
 * logger.h — Access logging (CLF and JSONL formats).
 ******************************************************************************/

#ifndef LOGGER_H
#define LOGGER_H

#include <netinet/in.h>

/* Must be called once before any logging. */
void logger_init(int format);   /* 0 = CLF, 1 = JSONL */

/* Log a completed proxy/static request. */
void log_access(const char *client_ip,
                const char *method,
                const char *path,
                int         status,
                double      latency_ms,
                double      upstream_latency_ms,
                const char *backend_name,
                unsigned long request_id);

#endif /* LOGGER_H */
