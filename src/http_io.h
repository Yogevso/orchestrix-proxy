/******************************************************************************
 * http_io.h — HTTP socket I/O helpers (connect, forward, relay).
 ******************************************************************************/

#ifndef HTTP_IO_H
#define HTTP_IO_H

#include "http_parse.h"

/* Write exactly len bytes to fd.  Returns 0 on success, -1 on error. */
int  http_write_all(int fd, const char *buf, int len);

/* Connect to host:port with a millisecond timeout.  Returns fd or -1. */
int  http_connect_upstream(const char *host, int port, int connect_timeout_ms);

/*
 * Build and send the proxied request to an upstream fd.
 * Strips hop-by-hop headers, injects X-Forwarded-For / X-Forwarded-Host.
 * Returns 0 on success, -1 on error.
 */
int  http_forward_request(int upstream_fd, const http_request_t *req,
                          const char *client_ip);

/*
 * Read and relay the upstream response back to the client.
 * Inserts X-Request-Id into the response.
 * Sets *status_code to the upstream status code on success.
 * Returns total bytes relayed (>= 0) or -1 on error.
 */
long http_relay_response(int client_fd, int upstream_fd,
                         int read_timeout_ms, unsigned long request_id,
                         int *status_code);

/* Send a canned error response (e.g. 502, 503, 429, 413). */
void http_send_error(int fd, int code, const char *reason,
                     const char *body_text, unsigned long request_id);

#endif /* HTTP_IO_H */
