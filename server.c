/******************************************************************************
 * server.c — Multi-threaded HTTP/1.0 file server.
 *
 * Serves static files and directory listings from the current working
 * directory using a fixed-size thread pool with a bounded work queue.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <limits.h>
#include <stdatomic.h>

#include "threadpool.h"

#define BACKLOG 10
#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"
#define BUF_SIZE 4096
#define CLIENT_TIMEOUT_SEC 5

/* Global server fd for signal handler cleanup. */
static volatile sig_atomic_t g_shutdown = 0;
static int g_server_fd = -1;

/******************************************************************************
 * Runtime metrics — lock-free counters updated from worker threads.
 ******************************************************************************/
typedef struct {
    atomic_ulong  total_requests;
    atomic_ulong  active_connections;
    atomic_ulong  status_2xx;
    atomic_ulong  status_3xx;
    atomic_ulong  status_4xx;
    atomic_ulong  status_5xx;
    atomic_ulong  overloaded;
    atomic_ulong  bytes_sent;        /* rough total (header + body) */
    atomic_ulong  total_latency_us;  /* cumulative latency in microseconds */
    time_t        start_time;
} server_stats_t;

static server_stats_t g_stats;
static threadpool     *g_pool;       /* accessible for /stats qsize snapshot */

/* Monotonic request ID — stamped on every response and logged for tracing. */
static atomic_ulong  g_request_id;
static __thread unsigned long tls_request_id;

static void stats_init(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = time(NULL);
}

static void stats_record(int status, double latency_ms) {
    atomic_fetch_add(&g_stats.total_requests, 1);
    unsigned long us = (unsigned long)(latency_ms * 1000.0);
    atomic_fetch_add(&g_stats.total_latency_us, us);

    if (status >= 200 && status < 300)      atomic_fetch_add(&g_stats.status_2xx, 1);
    else if (status >= 300 && status < 400) atomic_fetch_add(&g_stats.status_3xx, 1);
    else if (status >= 400 && status < 500) atomic_fetch_add(&g_stats.status_4xx, 1);
    else if (status >= 500)                 atomic_fetch_add(&g_stats.status_5xx, 1);
}

//#define DEBUG
#ifdef DEBUG
#define DEBUG_PRINT(fmt, args...) fprintf(stderr, "[DEBUG] " fmt "\n", ##args)
#else
#define DEBUG_PRINT(fmt, args...)
#endif

/* Forward declarations */
static int  handle_client(void *arg);
static int  handle_get_request(int client_fd, const char *path);
static void serve_stats(int client_fd);
static void serve_health(int client_fd);

static void send_error(int client_fd, int code, const char *reason, const char *body_text);
static void send_302(int client_fd, const char *original_path);

static int  serve_file(int client_fd, const char *file_path);
static int  serve_directory(int client_fd, const char *dir_path, const char *original_path);

static void format_time_rfc1123(char *buf, size_t buf_size, time_t t);
static const char *get_mime_type(const char *name);

static void log_access(const struct sockaddr_in *addr, const char *method,
                       const char *path, int status, double elapsed_ms);
static int  validate_path(const char *docroot, const char *resolved);

/* Per-connection context passed through the thread pool. */
typedef struct {
    int fd;
    struct sockaddr_in addr;
} client_ctx_t;

/* Convenience wrappers for common error responses. */
static void send_400(int fd) { send_error(fd, 400, "Bad Request",           "Bad Request."); }
static void send_403(int fd) { send_error(fd, 403, "Forbidden",             "Access denied."); }
static void send_404(int fd) { send_error(fd, 404, "Not Found",             "File not found."); }
static void send_500(int fd) { send_error(fd, 500, "Internal Server Error", "Some server side error."); }
static void send_501(int fd) { send_error(fd, 501, "Not supported",         "Method is not supported."); }
static void send_503(int fd) { send_error(fd, 503, "Service Unavailable",   "Server overloaded. Try again later."); }

static double elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000.0
         + (now.tv_nsec - start->tv_nsec) / 1e6;
}


/******************************************************************************
 * Signal handling — SIGINT/SIGTERM trigger graceful shutdown.
 ******************************************************************************/
static void handle_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
    /* Close the listening socket to unblock accept(). */
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}


/******************************************************************************
 * main
 ******************************************************************************/
int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: server <port> <pool-size> <max-queue-size> <max-number-of-requests>\n");
        exit(EXIT_FAILURE);
    }

    int port           = atoi(argv[1]);
    int pool_size      = atoi(argv[2]);
    int max_queue_size = atoi(argv[3]);
    int max_requests   = atoi(argv[4]);

    if (port <= 0 || pool_size <= 0 || max_queue_size <= 0 || max_requests <= 0) {
        fprintf(stderr, "Usage: server <port> <pool-size> <max-queue-size> <max-number-of-requests>\n");
        exit(EXIT_FAILURE);
    }

    /* Install signal handlers for graceful shutdown. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Reuse address
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Bind
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }



    // Create thread pool
    threadpool *pool = create_threadpool(pool_size, max_queue_size);
    if (!pool) {
        fprintf(stderr, "Failed to create threadpool.\n");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    g_pool = pool;

    stats_init();

    g_server_fd = server_fd;

    fprintf(stderr, "webserver/1.0 listening on port %d  "
            "(threads=%d, queue=%d, max-requests=%d)\n",
            port, pool_size, max_queue_size, max_requests);

    struct sockaddr_in client_addr;
    socklen_t client_len;

    int request_count = 0;
    while (request_count < max_requests && !g_shutdown) {
        client_len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            if (g_shutdown) break;
            perror("accept");
            continue;
        }

        /* Set a receive timeout so slow/stalled clients don't hold a worker thread forever. */
        struct timeval tv = { .tv_sec = CLIENT_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        DEBUG_PRINT("Accepted connection on fd=%d", client_sock);

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) {
            perror("malloc");
            close(client_sock);
            continue;
        }
        ctx->fd   = client_sock;
        ctx->addr = client_addr;

        if (dispatch(pool, handle_client, ctx) < 0) {
            /* Queue full or pool shutting down — reject immediately. */
            fprintf(stderr, "[OVERLOAD] Queue full, rejecting connection fd=%d\n", client_sock);
            send_503(client_sock);
            atomic_fetch_add(&g_stats.overloaded, 1);
            atomic_fetch_add(&g_stats.status_5xx, 1);
            atomic_fetch_add(&g_stats.total_requests, 1);
            close(client_sock);
            free(ctx);
        }
        request_count++;
    }

    /* Shutdown: drain pool, close socket, exit. */
    if (g_shutdown)
        fprintf(stderr, "\nCaught signal, shutting down...\n");

    destroy_threadpool(pool);
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    fprintf(stderr, "Server exited after %d request(s).\n", request_count);
    return 0;
}


/******************************************************************************
 * handle_client — Worker entry point for each accepted connection.
 *
 * Reads the request line, validates method/path/protocol, and dispatches
 * to the appropriate handler. Always closes the client socket before returning.
 ******************************************************************************/
static int handle_client(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int client_fd = ctx->fd;
    struct sockaddr_in client_addr = ctx->addr;
    free(ctx);

    atomic_fetch_add(&g_stats.active_connections, 1);
    tls_request_id = atomic_fetch_add(&g_request_id, 1) + 1;

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    DEBUG_PRINT("handle_client: fd=%d", client_fd);

    char buffer[BUF_SIZE];
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        if (n < 0) perror("read");
        DEBUG_PRINT("handle_client: read <= 0, closing fd=%d", client_fd);
        atomic_fetch_sub(&g_stats.active_connections, 1);
        close(client_fd);
        return 0;
    }
    buffer[n] = '\0';

    // Find first line (stop at \r\n or \n)
    char *end_line = strstr(buffer, "\r\n");
    if (!end_line) {
        end_line = strchr(buffer, '\n');
    }
    if (end_line) {
        *end_line = '\0';
    }

    DEBUG_PRINT("Request line: '%s'", buffer);

    char method[16], path[1024], proto[16];
    if (sscanf(buffer, "%15s %1023s %15s", method, path, proto) != 3) {
        DEBUG_PRINT("Malformed request line => 400");
        send_400(client_fd);
        log_access(&client_addr, "-", "-", 400, elapsed_ms(&t_start));
        atomic_fetch_sub(&g_stats.active_connections, 1);
        close(client_fd);
        return 0;
    }

    if (strcmp(proto, "HTTP/1.0") != 0 && strcmp(proto, "HTTP/1.1") != 0) {
        DEBUG_PRINT("Bad protocol => 400");
        send_400(client_fd);
        log_access(&client_addr, method, path, 400, elapsed_ms(&t_start));
        atomic_fetch_sub(&g_stats.active_connections, 1);
        close(client_fd);
        return 0;
    }

    if (strcasecmp(method, "GET") != 0) {
        DEBUG_PRINT("Method is not GET => 501");
        send_501(client_fd);
        log_access(&client_addr, method, path, 501, elapsed_ms(&t_start));
        atomic_fetch_sub(&g_stats.active_connections, 1);
        close(client_fd);
        return 0;
    }

    /* Built-in endpoints — served directly without filesystem lookup. */
    if (strcmp(path, "/stats") == 0) {
        serve_stats(client_fd);
        log_access(&client_addr, method, path, 200, elapsed_ms(&t_start));
        atomic_fetch_sub(&g_stats.active_connections, 1);
        close(client_fd);
        return 0;
    }
    if (strcmp(path, "/health") == 0) {
        serve_health(client_fd);
        log_access(&client_addr, method, path, 200, elapsed_ms(&t_start));
        atomic_fetch_sub(&g_stats.active_connections, 1);
        close(client_fd);
        return 0;
    }

    DEBUG_PRINT("Calling handle_get_request for path='%s'", path);
    int status = handle_get_request(client_fd, path);
    log_access(&client_addr, method, path, status, elapsed_ms(&t_start));

    DEBUG_PRINT("Closing fd=%d", client_fd);
    atomic_fetch_sub(&g_stats.active_connections, 1);
    close(client_fd);
    return 0;
}


/******************************************************************************
 * handle_get_request — Resolve the request path and serve content.
 *
 * Maps the URL path to the local filesystem (prefixed with ".") and serves
 * the file, directory listing, or appropriate error response.
 ******************************************************************************/
static int handle_get_request(int client_fd, const char *path) {
    char full_path[4096];
    memset(full_path, 0, sizeof(full_path));

    {
        const char *prefix = ".";
        size_t prefix_len  = strlen(prefix);
        size_t path_len    = strlen(path);

        if (prefix_len + path_len + 2 >= sizeof(full_path)) {
            DEBUG_PRINT("Too long path => 500");
            send_500(client_fd);
            return 500;
        }

        memcpy(full_path, prefix, prefix_len);
        memcpy(full_path + prefix_len, path, path_len);
        full_path[prefix_len + path_len] = '\0';
    }

    /* Path traversal protection: resolve to an absolute path and verify
       it still falls within the document root (current working directory). */
    char docroot[PATH_MAX];
    if (!getcwd(docroot, sizeof(docroot))) {
        send_500(client_fd);
        return 500;
    }
    char resolved[PATH_MAX];
    if (!realpath(full_path, resolved)) {
        if (errno == ENOENT) { send_404(client_fd); return 404; }
        if (errno == EACCES) { send_403(client_fd); return 403; }
        send_500(client_fd); return 500;
    }
    if (!validate_path(docroot, resolved)) {
        DEBUG_PRINT("Path traversal blocked: '%s'", resolved);
        send_403(client_fd);
        return 403;
    }

    /* Use the resolved absolute path from here on. */
    snprintf(full_path, sizeof(full_path), "%s", resolved);

    DEBUG_PRINT("Full local path: '%s'", full_path);

    struct stat st;
    if (stat(full_path, &st) < 0) {
        if (errno == ENOENT) {
            DEBUG_PRINT("stat => ENOENT => 404");
            send_404(client_fd);
            return 404;
        } else if (errno == EACCES) {
            DEBUG_PRINT("stat => EACCES => 403");
            send_403(client_fd);
            return 403;
        } else {
            DEBUG_PRINT("stat => errno=%d => 500", errno);
            send_500(client_fd);
            return 500;
        }
    }

    if (S_ISDIR(st.st_mode)) {
        DEBUG_PRINT("Path is a directory");
        size_t len = strlen(path);

        if (len == 0 || path[len - 1] != '/') {
            DEBUG_PRINT("Missing trailing slash => 302");
            send_302(client_fd, path);
            return 302;
        }

        char index_path[4096 + 16];
        snprintf(index_path, sizeof(index_path), "%s/index.html", full_path);

        DEBUG_PRINT("Checking for index.html => '%s'", index_path);

        struct stat st_index;
        if (stat(index_path, &st_index) < 0) {
            if (errno == ENOENT) {
                DEBUG_PRINT("No index.html => serving directory listing");
                serve_directory(client_fd, full_path, path);
                return 200;
            } else if (errno == EACCES) {
                DEBUG_PRINT("index.html => EACCES => 403");
                send_403(client_fd);
                return 403;
            } else {
                DEBUG_PRINT("stat(index.html) => errno=%d => 500", errno);
                send_500(client_fd);
                return 500;
            }
        } else {
            if (S_ISREG(st_index.st_mode)) {
                DEBUG_PRINT("index.html found => serving file");
                serve_file(client_fd, index_path);
            } else {
                DEBUG_PRINT("index.html is not a regular file => listing dir");
                serve_directory(client_fd, full_path, path);
            }
            return 200;
        }
    }

    else if (S_ISREG(st.st_mode)) {
        if (!(st.st_mode & S_IROTH)) {
            DEBUG_PRINT("No read permission => 403");
            send_403(client_fd);
            return 403;
        }
        DEBUG_PRINT("Serving file => 200 OK");
        serve_file(client_fd, full_path);
        return 200;
    }

    else {
        DEBUG_PRINT("Not a dir or regular file => 403");
        send_403(client_fd);
        return 403;
    }
}


/******************************************************************************
 * serve_file
 ******************************************************************************/
static int serve_file(int client_fd, const char *file_path) {
    DEBUG_PRINT("serve_file: '%s'", file_path);

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
            if (errno == EACCES) {
            DEBUG_PRINT("fopen => EACCES => 403");
            send_403(client_fd);
        } else {
            DEBUG_PRINT("fopen failed => 500");
            send_500(client_fd);
        }
        return 0;
    }

    struct stat st;
    if (stat(file_path, &st) < 0) {
        fclose(fp);
        if (errno == EACCES) {
            DEBUG_PRINT("stat => EACCES => 403");
            send_403(client_fd);
        } else {
            DEBUG_PRINT("stat => fail => 500");
            send_500(client_fd);
        }
        return 0;
    }
    size_t file_size = (size_t)st.st_size;
    time_t mod_time  = st.st_mtime;

    const char *mime = get_mime_type(file_path);
    DEBUG_PRINT("File size=%zu, mime='%s'", file_size, (mime ? mime : "NULL"));

    time_t now = time(NULL);
    char datebuf[128], modbuf[128];
    format_time_rfc1123(datebuf, sizeof(datebuf), now);
    format_time_rfc1123(modbuf, sizeof(modbuf), mod_time);

    char header[1024];
    int offset = 0;
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "HTTP/1.0 200 OK\r\n");
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Server: webserver/1.0\r\n");
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Date: %s\r\n", datebuf);
    if (mime) {
        offset += snprintf(header + offset, sizeof(header) - offset,
                           "Content-Type: %s\r\n", mime);
    }
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Content-Length: %zu\r\n", file_size);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Last-Modified: %s\r\n", modbuf);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "X-Request-Id: %lu\r\n", tls_request_id);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Connection: close\r\n\r\n");

    write(client_fd, header, offset);
    DEBUG_PRINT("Wrote header => %s", header);

    char buf[BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (write(client_fd, buf, n) < 0) {
            perror("write");
            break;
        }
    }

    fclose(fp);
    return 0;
}


/******************************************************************************
 * serve_directory
 ******************************************************************************/
static int serve_directory(int client_fd, const char *dir_path, const char *original_path) {
    DEBUG_PRINT("serve_directory: local='%s', request='%s'", dir_path, original_path);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (errno == EACCES) {
            DEBUG_PRINT("opendir => EACCES => 403");
            send_403(client_fd);
        } else {
            DEBUG_PRINT("opendir failed => 500");
            send_500(client_fd);
        }
        return 0;
    }

    char entries[8192];
    entries[0] = '\0';

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char full_entry_path[4096];
        snprintf(full_entry_path, sizeof(full_entry_path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(full_entry_path, &st) == 0) {
            char timebuf[128];
            format_time_rfc1123(timebuf, sizeof(timebuf), st.st_mtime);

            char link_name[1024];
            snprintf(link_name, sizeof(link_name), "%s", de->d_name);
            if (S_ISDIR(st.st_mode)) {
                size_t ln_len = strlen(link_name);
                if (ln_len + 1 < sizeof(link_name)) {
                    link_name[ln_len]   = '/';
                    link_name[ln_len+1] = '\0';
                }
            }

            char link_path[1024];
            snprintf(link_path, sizeof(link_path), "%s%s", original_path, link_name);

            char sizebuf[64];
            if (S_ISREG(st.st_mode)) {
                snprintf(sizebuf, sizeof(sizebuf), "%ld", (long)st.st_size);
            } else {
                sizebuf[0] = '\0';
            }

            char line[1024];
            int needed = snprintf(line, sizeof(line),
                                  "<tr><td><A HREF=\"%s\">%s</A></td><td>%s</td><td>%s</td></tr>\n",
                                  link_path, link_name, timebuf, sizebuf);
            if (needed < 0 || (size_t)needed >= sizeof(line)) {
                continue;
            }

            size_t cur_len = strlen(entries);
            if (cur_len + (size_t)needed < sizeof(entries)) {
                strncat(entries, line, sizeof(entries) - cur_len - 1);
            }
        }
    }
    closedir(dir);

    char body[16384];
    snprintf(body, sizeof(body),
             "<HTML>\n"
             "<HEAD><TITLE>Index of %s</TITLE></HEAD>\n"
             "<BODY>\n"
             "<H4>Index of %s</H4>\n"
             "<table CELLSPACING=8>\n"
             "<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\n"
             "%s"
             "</table>\n"
             "<HR>\n"
             "<ADDRESS>webserver/1.0</ADDRESS>\n"
             "</BODY></HTML>\n",
             original_path, original_path, entries);

    size_t body_len = strlen(body);

    time_t now = time(NULL);
    char datebuf[128];
    format_time_rfc1123(datebuf, sizeof(datebuf), now);

    char header[1024];
    int offset = 0;
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "HTTP/1.0 200 OK\r\n");
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Server: webserver/1.0\r\n");
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Date: %s\r\n", datebuf);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Content-Type: text/html\r\n");
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Content-Length: %zu\r\n", body_len);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Last-Modified: %s\r\n", datebuf);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "X-Request-Id: %lu\r\n", tls_request_id);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Connection: close\r\n\r\n");

    write(client_fd, header, offset);
    write(client_fd, body, body_len);

    DEBUG_PRINT("Directory listing sent => 200 OK");
    return 0;
}


/******************************************************************************
 * send_error — Construct and send an HTTP error response.
 *
 * Builds a standard error page with the given status code, reason phrase,
 * and a short description in the HTML body.
 ******************************************************************************/
static void send_error(int client_fd, int code, const char *reason, const char *body_text) {
    DEBUG_PRINT("send_%d", code);

    char body[512];
    snprintf(body, sizeof(body),
             "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD>\r\n"
             "<BODY><H4>%d %s</H4>\r\n"
             "%s\r\n"
             "</BODY></HTML>\r\n",
             code, reason, code, reason, body_text);

    char datebuf[128];
    time_t now = time(NULL);
    format_time_rfc1123(datebuf, sizeof(datebuf), now);

    size_t body_len = strlen(body);
    char header[512];
    int offset = 0;
    offset += snprintf(header + offset, sizeof(header) - offset,
                          "HTTP/1.0 %d %s\r\n"
                          "Server: webserver/1.0\r\n"
                          "Date: %s\r\n"
                          "Content-Type: text/html\r\n"
                          "Content-Length: %zu\r\n",
                          code, reason, datebuf, body_len);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "X-Request-Id: %lu\r\n", tls_request_id);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Connection: close\r\n\r\n");

    write(client_fd, header, offset);
    write(client_fd, body, body_len);
}


/******************************************************************************
 * send_302 — Redirect a directory request to include a trailing slash.
 ******************************************************************************/
static void send_302(int client_fd, const char *original_path) {
    DEBUG_PRINT("send_302 => redirecting path='%s'", original_path);

    char location[1024];
    size_t len = strlen(original_path);
    if (len + 2 > sizeof(location)) {
        send_500(client_fd);
        return;
    }

    snprintf(location, sizeof(location), "%s/", original_path);

    const char *body =
            "<HTML><HEAD><TITLE>302 Found</TITLE></HEAD>\r\n"
            "<BODY><H4>302 Found</H4>\r\n"
            "Directories must end with a slash.\r\n"
            "</BODY></HTML>\r\n";

    char datebuf[128];
    time_t now = time(NULL);
    format_time_rfc1123(datebuf, sizeof(datebuf), now);

    size_t body_len = strlen(body);
    char header[512];
    int offset = 0;
    offset += snprintf(header + offset, sizeof(header) - offset,
                          "HTTP/1.0 302 Found\r\n"
                          "Server: webserver/1.0\r\n"
                          "Date: %s\r\n"
                          "Location: %s\r\n"
                          "Content-Type: text/html\r\n"
                          "Content-Length: %zu\r\n",
                          datebuf, location, body_len);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "X-Request-Id: %lu\r\n", tls_request_id);
    offset += snprintf(header + offset, sizeof(header) - offset,
                       "Connection: close\r\n\r\n");

    write(client_fd, header, offset);
    write(client_fd, body, body_len);
}


/******************************************************************************
 * get_mime_type
 ******************************************************************************/
static const char *get_mime_type(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return NULL;
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)  return "text/html";
    if (strcmp(dot, ".css") == 0)                               return "text/css";
    if (strcmp(dot, ".js") == 0)                                return "application/javascript";
    if (strcmp(dot, ".json") == 0)                              return "application/json";
    if (strcmp(dot, ".txt") == 0)                               return "text/plain";
    if (strcmp(dot, ".xml") == 0)                               return "application/xml";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)  return "image/jpeg";
    if (strcmp(dot, ".png") == 0)                               return "image/png";
    if (strcmp(dot, ".gif") == 0)                               return "image/gif";
    if (strcmp(dot, ".svg") == 0)                               return "image/svg+xml";
    if (strcmp(dot, ".ico") == 0)                               return "image/x-icon";
    if (strcmp(dot, ".pdf") == 0)                               return "application/pdf";
    if (strcmp(dot, ".mp3") == 0)                               return "audio/mpeg";
    if (strcmp(dot, ".wav") == 0)                               return "audio/wav";
    if (strcmp(dot, ".au") == 0)                                return "audio/basic";
    if (strcmp(dot, ".avi") == 0)                               return "video/x-msvideo";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpg") == 0)  return "video/mpeg";
    if (strcmp(dot, ".mp4") == 0)                               return "video/mp4";
    return NULL;
}


/******************************************************************************
 * format_time_rfc1123
 ******************************************************************************/
static void format_time_rfc1123(char *buf, size_t buf_size, time_t t) {
    struct tm *tm = gmtime(&t);
    strftime(buf, buf_size, RFC1123FMT, tm);
}


/******************************************************************************
 * log_access — Print a one-line access log entry to stdout.
 *
 * Format approximates Common Log Format with request ID and latency:
 *   <client_ip> - - [<date>] "<method> <path>" <status> <latency> rid=<id>
 ******************************************************************************/
static void log_access(const struct sockaddr_in *addr, const char *method,
                       const char *path, int status, double elapsed) {
    char timebuf[64];
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S +0000", tm);

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

    printf("%s - - [%s] \"%s %s\" %d %.2fms rid=%lu\n",
           ip, timebuf, method, path, status, elapsed, tls_request_id);
    fflush(stdout);

    stats_record(status, elapsed);
}


/******************************************************************************
 * validate_path — Ensure the resolved path is under the document root.
 *
 * Prevents path traversal attacks (e.g. GET /../../etc/passwd).
 * Returns 1 if the path is safe, 0 if it escapes the document root.
 ******************************************************************************/
static int validate_path(const char *docroot, const char *resolved) {
    size_t root_len = strlen(docroot);
    if (strncmp(resolved, docroot, root_len) != 0)
        return 0;
    /* The next char must be '/' or '\0' (the root itself). */
    return resolved[root_len] == '/' || resolved[root_len] == '\0';
}


/******************************************************************************
 * serve_stats — Return live server metrics as JSON.
 *
 * Endpoint: GET /stats
 *
 * Provides a snapshot of runtime counters including uptime, total requests,
 * active connections, queue depth, status code breakdown, average latency,
 * and overload events. All counters are lock-free atomics.
 ******************************************************************************/
static void serve_stats(int client_fd) {
    time_t now       = time(NULL);
    long uptime      = (long)(now - g_stats.start_time);
    unsigned long total    = atomic_load(&g_stats.total_requests);
    unsigned long active   = atomic_load(&g_stats.active_connections);
    unsigned long s2xx     = atomic_load(&g_stats.status_2xx);
    unsigned long s3xx     = atomic_load(&g_stats.status_3xx);
    unsigned long s4xx     = atomic_load(&g_stats.status_4xx);
    unsigned long s5xx     = atomic_load(&g_stats.status_5xx);
    unsigned long overload = atomic_load(&g_stats.overloaded);
    unsigned long lat_us   = atomic_load(&g_stats.total_latency_us);

    /* Snapshot the threadpool queue depth (under lock). */
    int qsize = 0, qmax = 0, nthreads = 0;
    if (g_pool) {
        pthread_mutex_lock(&g_pool->qlock);
        qsize    = g_pool->qsize;
        qmax     = g_pool->max_qsize;
        nthreads = g_pool->num_threads;
        pthread_mutex_unlock(&g_pool->qlock);
    }

    double avg_latency_ms = total > 0 ? (lat_us / 1000.0) / total : 0.0;

    char body[1024];
    int blen = snprintf(body, sizeof(body),
        "{\n"
        "  \"uptime_seconds\": %ld,\n"
        "  \"total_requests\": %lu,\n"
        "  \"active_connections\": %lu,\n"
        "  \"pool_threads\": %d,\n"
        "  \"queue_depth\": %d,\n"
        "  \"queue_capacity\": %d,\n"
        "  \"avg_latency_ms\": %.3f,\n"
        "  \"status\": { \"2xx\": %lu, \"3xx\": %lu, \"4xx\": %lu, \"5xx\": %lu },\n"
        "  \"overloaded_connections\": %lu\n"
        "}\n",
        uptime, total, active, nthreads, qsize, qmax,
        avg_latency_ms, s2xx, s3xx, s4xx, s5xx, overload);

    char datebuf[128];
    format_time_rfc1123(datebuf, sizeof(datebuf), now);

    char header[512];
    int hlen = 0;
    hlen += snprintf(header + hlen, sizeof(header) - hlen,
        "HTTP/1.0 200 OK\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n",
        datebuf, blen);
    hlen += snprintf(header + hlen, sizeof(header) - hlen,
        "X-Request-Id: %lu\r\n", tls_request_id);
    hlen += snprintf(header + hlen, sizeof(header) - hlen,
        "Connection: close\r\n\r\n");

    write(client_fd, header, hlen);
    write(client_fd, body, blen);
}


/******************************************************************************
 * serve_health — Lightweight liveness probe.
 *
 * Endpoint: GET /health
 *
 * Returns a minimal JSON body suitable for container orchestration health
 * checks (Docker HEALTHCHECK, Kubernetes livenessProbe, etc.).
 ******************************************************************************/
static void serve_health(int client_fd) {
    time_t now  = time(NULL);
    long uptime = (long)(now - g_stats.start_time);

    char body[128];
    int blen = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"uptime_seconds\":%ld}\n", uptime);

    char datebuf[128];
    format_time_rfc1123(datebuf, sizeof(datebuf), now);

    char header[512];
    int hlen = 0;
    hlen += snprintf(header + hlen, sizeof(header) - hlen,
        "HTTP/1.0 200 OK\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n",
        datebuf, blen);
    hlen += snprintf(header + hlen, sizeof(header) - hlen,
        "X-Request-Id: %lu\r\n", tls_request_id);
    hlen += snprintf(header + hlen, sizeof(header) - hlen,
        "Connection: close\r\n\r\n");

    write(client_fd, header, hlen);
    write(client_fd, body, blen);
}
