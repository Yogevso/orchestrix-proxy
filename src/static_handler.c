/******************************************************************************
 * static_handler.c — Legacy static-file serving mode.
 *
 * Provides the original webserver behaviour: serve files and directory
 * listings from the current working directory.  Kept as an optional mode
 * so the proxy repo retains its heritage and can still demo file serving.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <arpa/inet.h>

#include "static_handler.h"
#include "server_core.h"
#include "http_parse.h"
#include "http_io.h"
#include "metrics.h"
#include "logger.h"

#define BUF_SIZE       4096
#define RFC1123FMT     "%a, %d %b %Y %H:%M:%S GMT"
#define SERVER_NAME    "orchestrix-proxy/1.0"

/* ---- helpers ---- */

static void format_rfc1123(char *buf, size_t sz, time_t t) {
    struct tm *tm = gmtime(&t);
    strftime(buf, sz, RFC1123FMT, tm);
}

static const char *mime_type(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return NULL;
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".txt")  == 0) return "text/plain";
    if (strcmp(dot, ".xml")  == 0) return "application/xml";
    if (strcmp(dot, ".jpg")  == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".gif")  == 0) return "image/gif";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".pdf")  == 0) return "application/pdf";
    if (strcmp(dot, ".mp3")  == 0) return "audio/mpeg";
    if (strcmp(dot, ".mp4")  == 0) return "video/mp4";
    return NULL;
}

static int validate_path(const char *docroot, const char *resolved) {
    size_t root_len = strlen(docroot);
    if (strncmp(resolved, docroot, root_len) != 0) return 0;
    return resolved[root_len] == '/' || resolved[root_len] == '\0';
}

/* ---- serve file ---- */

static void serve_file(int fd, const char *path, unsigned long rid) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        int code = (errno == EACCES) ? 403 : 500;
        const char *reason = (code == 403) ? "Forbidden" : "Internal Server Error";
        http_send_error(fd, code, reason, reason, rid);
        return;
    }

    struct stat st;
    if (stat(path, &st) < 0) {
        fclose(fp);
        http_send_error(fd, 500, "Internal Server Error", "stat failed.", rid);
        return;
    }

    const char *mime = mime_type(path);
    char datebuf[128], modbuf[128];
    time_t now = time(NULL);
    format_rfc1123(datebuf, sizeof(datebuf), now);
    format_rfc1123(modbuf,  sizeof(modbuf),  st.st_mtime);

    char header[1024];
    int hlen = 0;
    hlen += snprintf(header + hlen, sizeof(header) - hlen,
        "HTTP/1.0 200 OK\r\nServer: %s\r\nDate: %s\r\n", SERVER_NAME, datebuf);
    if (mime)
        hlen += snprintf(header + hlen, sizeof(header) - hlen,
            "Content-Type: %s\r\n", mime);
    hlen += snprintf(header + hlen, sizeof(header) - hlen,
        "Content-Length: %ld\r\nLast-Modified: %s\r\n"
        "X-Request-Id: %lu\r\nConnection: close\r\n\r\n",
        (long)st.st_size, modbuf, rid);

    http_write_all(fd, header, hlen);

    char buf[BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        if (http_write_all(fd, buf, (int)n) < 0) break;

    fclose(fp);
}

/* ---- serve directory listing ---- */

static void serve_directory(int fd, const char *dir_path,
                            const char *url_path, unsigned long rid) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        int code = (errno == EACCES) ? 403 : 500;
        http_send_error(fd, code, code == 403 ? "Forbidden" : "Internal Server Error",
                        "Cannot open directory.", rid);
        return;
    }

    char entries[8192];
    entries[0] = '\0';
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        char link_name[1024];
        snprintf(link_name, sizeof(link_name), "%s", de->d_name);
        if (S_ISDIR(st.st_mode)) {
            size_t ln = strlen(link_name);
            if (ln + 1 < sizeof(link_name)) {
                link_name[ln] = '/';
                link_name[ln + 1] = '\0';
            }
        }

        char timeb[128], sizeb[64] = "";
        format_rfc1123(timeb, sizeof(timeb), st.st_mtime);
        if (S_ISREG(st.st_mode))
            snprintf(sizeb, sizeof(sizeb), "%ld", (long)st.st_size);

        char line[1024];
        int need = snprintf(line, sizeof(line),
            "<tr><td><a href=\"%s%s\">%s</a></td><td>%s</td><td>%s</td></tr>\n",
            url_path, link_name, link_name, timeb, sizeb);
        if (need < 0 || (size_t)need >= sizeof(line)) continue;

        size_t cur = strlen(entries);
        if (cur + (size_t)need < sizeof(entries))
            strncat(entries, line, sizeof(entries) - cur - 1);
    }
    closedir(dir);

    char body[16384];
    int blen = snprintf(body, sizeof(body),
        "<html><head><title>Index of %s</title></head>\n"
        "<body><h4>Index of %s</h4>\n"
        "<table cellspacing=8>\n"
        "<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\n"
        "%s</table><hr><address>%s</address></body></html>\n",
        url_path, url_path, entries, SERVER_NAME);

    char datebuf[128];
    format_rfc1123(datebuf, sizeof(datebuf), time(NULL));

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\nServer: %s\r\nDate: %s\r\n"
        "Content-Type: text/html\r\nContent-Length: %d\r\n"
        "X-Request-Id: %lu\r\nConnection: close\r\n\r\n",
        SERVER_NAME, datebuf, blen, rid);

    http_write_all(fd, header, hlen);
    http_write_all(fd, body, blen);
}

/* ---- main entry point for static mode ---- */

int static_handle_client(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int client_fd            = ctx->fd;
    struct sockaddr_in caddr = ctx->addr;
    unsigned long rid        = ctx->request_id;
    free(ctx);

    tls_request_id = rid;
    atomic_fetch_add(&g_metrics.active_connections, 1);

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    char client_ip[INET_ADDRSTRLEN];
    sockaddr_to_ip(&caddr, client_ip, sizeof(client_ip));

    /* Read request. */
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.content_length = -1;

    ssize_t n = read(client_fd, req.raw, sizeof(req.raw) - 1);
    if (n <= 0) {
        atomic_fetch_sub(&g_metrics.active_connections, 1);
        close(client_fd);
        return 0;
    }
    req.raw_len = (int)n;
    req.raw[n]  = '\0';

    if (http_parse_request(&req) != 0) {
        http_send_error(client_fd, 400, "Bad Request", "Malformed request.", rid);
        metrics_record_request(400, elapsed_ms(&t_start), 0);
        log_access(client_ip, "-", "-", 400, elapsed_ms(&t_start), 0, NULL, rid);
        goto done;
    }

    /* Only GET supported in static mode. */
    if (strcasecmp(req.method, "GET") != 0) {
        http_send_error(client_fd, 501, "Not Implemented",
                        "Method not supported.", rid);
        metrics_record_request(501, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 501,
                   elapsed_ms(&t_start), 0, NULL, rid);
        goto done;
    }

    /* Built-in endpoints. */
    if (strcmp(req.path, "/stats") == 0 || strcmp(req.path, "/health") == 0) {
        /* Reuse proxy handler's stats/health (import not needed — inline). */
        if (strcmp(req.path, "/health") == 0) {
            time_t now = time(NULL);
            long uptime = (long)(now - g_metrics.start_time);
            char body[128];
            int blen = snprintf(body, sizeof(body),
                "{\"status\":\"ok\",\"uptime_seconds\":%ld}\n", uptime);
            char header[512];
            int hlen = snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\nServer: %s\r\n"
                "Content-Type: application/json\r\nContent-Length: %d\r\n"
                "X-Request-Id: %lu\r\nConnection: close\r\n\r\n",
                SERVER_NAME, blen, rid);
            http_write_all(client_fd, header, hlen);
            http_write_all(client_fd, body, blen);
        } else {
            char body[8192];
            int qsize = 0, qmax = 0, nthreads = 0;
            if (g_pool) {
                pthread_mutex_lock(&g_pool->qlock);
                qsize    = g_pool->qsize;
                qmax     = g_pool->max_qsize;
                nthreads = g_pool->num_threads;
                pthread_mutex_unlock(&g_pool->qlock);
            }
            int blen = metrics_render_stats_json(body, sizeof(body),
                                                  qsize, qmax, nthreads);
            char header[512];
            int hlen = snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\nServer: %s\r\n"
                "Content-Type: application/json\r\nContent-Length: %d\r\n"
                "X-Request-Id: %lu\r\nConnection: close\r\n\r\n",
                SERVER_NAME, blen, rid);
            http_write_all(client_fd, header, hlen);
            http_write_all(client_fd, body, blen);
        }
        metrics_record_request(200, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 200,
                   elapsed_ms(&t_start), 0, NULL, rid);
        goto done;
    }

    /* ---- filesystem path resolution ---- */
    char full_path[4096];
    snprintf(full_path, sizeof(full_path), ".%s", req.path);

    char docroot[PATH_MAX];
    if (!getcwd(docroot, sizeof(docroot))) {
        http_send_error(client_fd, 500, "Internal Server Error", "getcwd failed.", rid);
        metrics_record_request(500, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 500,
                   elapsed_ms(&t_start), 0, NULL, rid);
        goto done;
    }

    char resolved[PATH_MAX];
    if (!realpath(full_path, resolved)) {
        int code = (errno == ENOENT) ? 404 : (errno == EACCES) ? 403 : 500;
        const char *reason = (code == 404) ? "Not Found" :
                             (code == 403) ? "Forbidden" : "Internal Server Error";
        http_send_error(client_fd, code, reason, reason, rid);
        metrics_record_request(code, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, code,
                   elapsed_ms(&t_start), 0, NULL, rid);
        goto done;
    }

    if (!validate_path(docroot, resolved)) {
        http_send_error(client_fd, 403, "Forbidden", "Path traversal blocked.", rid);
        metrics_record_request(403, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, 403,
                   elapsed_ms(&t_start), 0, NULL, rid);
        goto done;
    }

    struct stat st;
    if (stat(resolved, &st) < 0) {
        int code = (errno == ENOENT) ? 404 : (errno == EACCES) ? 403 : 500;
        const char *reason = (code == 404) ? "Not Found" :
                             (code == 403) ? "Forbidden" : "Internal Server Error";
        http_send_error(client_fd, code, reason, reason, rid);
        metrics_record_request(code, elapsed_ms(&t_start), 0);
        log_access(client_ip, req.method, req.path, code,
                   elapsed_ms(&t_start), 0, NULL, rid);
        goto done;
    }

    int status = 200;

    if (S_ISDIR(st.st_mode)) {
        size_t plen = strlen(req.path);
        if (plen == 0 || req.path[plen - 1] != '/') {
            /* Redirect to add trailing slash. */
            char loc[4096];
            snprintf(loc, sizeof(loc), "%.4090s/", req.path);
            char body[] = "<html><body>302 Found</body></html>";
            int blen = (int)strlen(body);
            char header[512];
            int hlen = snprintf(header, sizeof(header),
                "HTTP/1.0 302 Found\r\nServer: %s\r\nLocation: %s\r\n"
                "Content-Type: text/html\r\nContent-Length: %d\r\n"
                "X-Request-Id: %lu\r\nConnection: close\r\n\r\n",
                SERVER_NAME, loc, blen, rid);
            http_write_all(client_fd, header, hlen);
            http_write_all(client_fd, body, blen);
            status = 302;
        } else {
            /* Try index.html, else listing. */
            char idx[PATH_MAX];
            snprintf(idx, sizeof(idx), "%.4080s/index.html", resolved);
            struct stat idx_st;
            if (stat(idx, &idx_st) == 0 && S_ISREG(idx_st.st_mode))
                serve_file(client_fd, idx, rid);
            else
                serve_directory(client_fd, resolved, req.path, rid);
        }
    } else if (S_ISREG(st.st_mode)) {
        if (!(st.st_mode & S_IROTH)) {
            http_send_error(client_fd, 403, "Forbidden", "No read permission.", rid);
            status = 403;
        } else {
            serve_file(client_fd, resolved, rid);
        }
    } else {
        http_send_error(client_fd, 403, "Forbidden", "Not a file or directory.", rid);
        status = 403;
    }

    metrics_record_request(status, elapsed_ms(&t_start), 0);
    log_access(client_ip, req.method, req.path, status,
               elapsed_ms(&t_start), 0, NULL, rid);

done:
    atomic_fetch_sub(&g_metrics.active_connections, 1);
    close(client_fd);
    return 0;
}
