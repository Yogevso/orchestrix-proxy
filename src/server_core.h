/******************************************************************************
 * server_core.h — Listening socket, accept loop, server lifecycle.
 ******************************************************************************/

#ifndef SERVER_CORE_H
#define SERVER_CORE_H

#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>

#include "config.h"
#include "threadpool.h"

/* Per-connection context passed through the thread pool. */
typedef struct {
    int                  fd;
    struct sockaddr_in   addr;
    unsigned long        request_id;
} client_ctx_t;

/* Global shutdown flag (set by signal handler). */
extern volatile sig_atomic_t g_shutdown;

/* Global config reload flag (set by SIGHUP). */
extern volatile sig_atomic_t g_reload;

/* Global pool pointer (for /stats queue-depth snapshot). */
extern threadpool *g_pool;

/* Monotonic request-ID counter. */
extern atomic_ulong g_request_id;

/* Thread-local request ID for the current worker. */
extern __thread unsigned long tls_request_id;

/* Create a listening socket on the given port. Returns fd or -1. */
int  server_listen(int port);

/* Run the accept loop — blocks until g_shutdown is set.
 * handler is the dispatch_fn for the thread pool (proxy or static). */
void server_accept_loop(int server_fd, threadpool *pool, dispatch_fn handler);

/* Install signal handlers (SIGINT, SIGTERM, SIGHUP). */
void server_install_signals(void);

/* Elapsed milliseconds since a timespec. */
double elapsed_ms(const struct timespec *start);

/* Convert a sockaddr_in to a string IP. */
void sockaddr_to_ip(const struct sockaddr_in *addr, char *buf, int buf_size);

#endif /* SERVER_CORE_H */
