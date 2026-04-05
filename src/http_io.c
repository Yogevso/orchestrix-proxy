/******************************************************************************
 * http_io.c — HTTP socket I/O helpers (connect, forward, relay).
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#include "http_io.h"
#include "http_parse.h"

/* ---------- write_all ---------- */

int http_write_all(int fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (int)n;
    }
    return 0;
}

/* ---------- connect with timeout ---------- */

int http_connect_upstream(const char *host, int port, int connect_timeout_ms) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        /* Try DNS resolution. */
        struct hostent *he = gethostbyname(host);
        if (!he) return -1;
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* Set non-blocking for connect timeout. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (rc < 0) {
        /* Wait for connect to complete. */
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int r = poll(&pfd, 1, connect_timeout_ms);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }

    /* Restore blocking mode. */
    fcntl(fd, F_SETFL, flags);

    /* Set read timeout. */
    /* (caller should set via setsockopt after connect if needed) */

    return fd;
}

/* ---------- forward request to upstream ---------- */

int http_forward_request(int upstream_fd, const http_request_t *req,
                         const char *client_ip) {
    char buf[HTTP_BUF_SIZE * 2];
    int off = 0;

    /* Request line. */
    off += snprintf(buf + off, sizeof(buf) - off, "%s %s %s\r\n",
                    req->method, req->path, req->proto);

    /* Forward non-hop-by-hop headers. */
    int has_host = 0;
    for (int i = 0; i < req->header_count; i++) {
        if (http_is_hop_by_hop(req->headers[i].key))
            continue;
        off += snprintf(buf + off, sizeof(buf) - off, "%s: %s\r\n",
                        req->headers[i].key, req->headers[i].value);
        if (strcasecmp(req->headers[i].key, "Host") == 0)
            has_host = 1;
    }

    /* Inject proxy headers. */
    if (client_ip) {
        /* Append to existing X-Forwarded-For or create new. */
        const char *existing = http_header_get(req->headers, req->header_count,
                                               "X-Forwarded-For");
        if (existing)
            off += snprintf(buf + off, sizeof(buf) - off,
                            "X-Forwarded-For: %s, %s\r\n", existing, client_ip);
        else
            off += snprintf(buf + off, sizeof(buf) - off,
                            "X-Forwarded-For: %s\r\n", client_ip);
    }

    /* X-Forwarded-Host from original Host header. */
    const char *host_hdr = http_header_get(req->headers, req->header_count, "Host");
    if (host_hdr)
        off += snprintf(buf + off, sizeof(buf) - off,
                        "X-Forwarded-Host: %s\r\n", host_hdr);

    /* Connection: close for the upstream. */
    off += snprintf(buf + off, sizeof(buf) - off, "Connection: close\r\n");

    /* End of headers. */
    off += snprintf(buf + off, sizeof(buf) - off, "\r\n");

    (void)has_host;

    if (http_write_all(upstream_fd, buf, off) < 0)
        return -1;

    /* Forward request body if present. */
    if (req->content_length > 0) {
        int body_in_buf = req->raw_len - req->head_len;
        if (body_in_buf > 0) {
            if (http_write_all(upstream_fd, req->raw + req->head_len, body_in_buf) < 0)
                return -1;
        }
        /* Read and forward remaining body from client — not needed for HTTP/1.0
           without keep-alive since we already read all available data.
           For a more complete implementation this would loop on the client fd. */
    }

    return 0;
}

/* ---------- relay response back to client ---------- */

long http_relay_response(int client_fd, int upstream_fd,
                         int read_timeout_ms, unsigned long request_id,
                         int *status_code) {
    /* Set read timeout on upstream socket. */
    struct timeval tv;
    tv.tv_sec  = read_timeout_ms / 1000;
    tv.tv_usec = (read_timeout_ms % 1000) * 1000;
    setsockopt(upstream_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[HTTP_BUF_SIZE];
    int  total_head = 0;
    long total_relayed = 0;

    /* Phase 1: read response head. */
    while (total_head < (int)sizeof(buf) - 1) {
        ssize_t n = read(upstream_fd, buf + total_head, sizeof(buf) - 1 - total_head);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        total_head += (int)n;
        buf[total_head] = '\0';

        /* Check if we have the full header. */
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n"))
            break;
    }

    /* Parse response head. */
    http_response_head_t resp;
    int head_len = 0;
    if (http_parse_response_head(buf, total_head, &resp, &head_len) != 0)
        return -1;

    *status_code = resp.status_code;

    /* Build modified response head: inject X-Request-Id, strip hop-by-hop. */
    char out[HTTP_BUF_SIZE * 2];
    int  ooff = 0;

    ooff += snprintf(out + ooff, sizeof(out) - ooff, "%s %d %s\r\n",
                     resp.proto, resp.status_code, resp.status_text);

    for (int i = 0; i < resp.header_count; i++) {
        if (http_is_hop_by_hop(resp.headers[i].key))
            continue;
        ooff += snprintf(out + ooff, sizeof(out) - ooff, "%s: %s\r\n",
                         resp.headers[i].key, resp.headers[i].value);
    }

    ooff += snprintf(out + ooff, sizeof(out) - ooff,
                     "X-Request-Id: %lu\r\n", request_id);
    ooff += snprintf(out + ooff, sizeof(out) - ooff,
                     "Connection: close\r\n\r\n");

    if (http_write_all(client_fd, out, ooff) < 0)
        return -1;
    total_relayed += ooff;

    /* Send any body bytes already in our buffer past the head. */
    int body_already = total_head - head_len;
    if (body_already > 0) {
        if (http_write_all(client_fd, buf + head_len, body_already) < 0)
            return -1;
        total_relayed += body_already;
    }

    /* Phase 2: stream remaining body. */
    while (1) {
        ssize_t n = read(upstream_fd, buf, sizeof(buf));
        if (n <= 0) break;    /* EOF or error */
        if (http_write_all(client_fd, buf, (int)n) < 0)
            return -1;
        total_relayed += n;
    }

    return total_relayed;
}

/* ---------- send canned error ---------- */

void http_send_error(int fd, int code, const char *reason,
                     const char *body_text, unsigned long request_id) {
    char body[512];
    int blen = snprintf(body, sizeof(body),
        "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD>\r\n"
        "<BODY><H4>%d %s</H4>\r\n"
        "%s\r\n"
        "</BODY></HTML>\r\n",
        code, reason, code, reason, body_text);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Server: orchestrix-proxy/1.0\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "X-Request-Id: %lu\r\n"
        "Connection: close\r\n\r\n",
        code, reason, blen, request_id);

    http_write_all(fd, header, hlen);
    http_write_all(fd, body, blen);
}
