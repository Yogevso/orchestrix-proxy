# c-threaded-webserver

A production-style concurrent HTTP/1.0 server written in C, designed to demonstrate thread-pool-based request handling, bounded concurrency, and predictable behavior under load.

## Why This Project

This project demonstrates low-level systems programming and concurrent server design in C.
It focuses on building a correct, resource-safe, and predictable HTTP server using a custom thread pool and bounded work queue.

This design mirrors real-world server architectures where concurrency must be bounded and controlled rather than unbounded and optimistic.

The goal is to showcase understanding of:
- Concurrency and synchronization (pthreads, mutexes, condition variables)
- Lock-free instrumentation (C11 atomics for runtime metrics)
- Socket programming and the HTTP request lifecycle
- Graceful shutdown and resource cleanup under signals
- System robustness under load (back-pressure, timeouts, path validation)

## What Makes This Interesting

Unlike simple threaded servers, this implementation focuses on controlled concurrency and predictable behavior under load.

Key aspects include:
- Bounded work queue enforcing back-pressure
- Explicit overload behavior (503 instead of unbounded blocking)
- Per-request latency measurement for performance visibility
- `X-Request-Id` on every response for end-to-end tracing
- Clean shutdown semantics that drain in-flight work safely
- Built-in `/stats` and `/health` endpoints via lock-free atomic counters

## Key Capabilities

- Concurrent request handling via a fixed-size thread pool
- Back-pressure control using a bounded FIFO queue
- Static file and directory serving with correct HTTP responses
- Graceful shutdown with request draining and thread join
- Protection against path traversal and slow-client resource exhaustion
- Per-request latency measurement and structured access logging
- Overload detection with 503 responses when the queue is full
- `X-Request-Id` header on every response (atomic counter) for request tracing
- Built-in `/stats` endpoint with live JSON metrics (lock-free atomics)
- Built-in `/health` endpoint for container orchestration liveness probes

## Architecture

```
accept loop (main thread)
  │
  └── dispatch(client_fd) ──► bounded FIFO queue
                                  │
                          ┌───────┼───────┐
                          ▼       ▼       ▼
                       worker  worker  worker   (N threads)
                          │
                          ├── parse request line
                          ├── resolve path → file / directory
                          ├── build HTTP response
                          └── write + close
```

- **Thread pool** — fixed-size, created at startup. Workers block when the queue is empty; the accept loop blocks when the queue is full.
- **Bounded queue** — prevents unbounded memory growth under burst load.
- **Shutdown** — the server exits after a configured number of requests **or** on `SIGINT`/`SIGTERM`. In both cases the pool drains remaining work, joins all threads, and frees resources.

## Features

- Concurrent request handling via a reusable thread pool
- Static file serving with MIME type detection (20+ types)
- Automatic directory listing when no `index.html` is present
- `302` redirect for directory paths missing a trailing `/`
- Standard HTTP error responses: `400`, `403`, `404`, `500`, `501`
- **Graceful shutdown** — handles `SIGINT`/`SIGTERM` to drain the pool and exit cleanly
- **Access logging with latency** — per-request CLF log line with response time in milliseconds
- **Overload protection** — returns `503 Service Unavailable` when the work queue is full instead of blocking
- **Request tracing** — every response carries an `X-Request-Id` header (monotonic atomic counter), also logged for correlation
- **Live metrics endpoint** — `GET /stats` returns JSON with uptime, request counts, queue depth, avg latency, and status code breakdown
- **Health check endpoint** — `GET /health` returns `{"status":"ok"}` for Docker HEALTHCHECK / Kubernetes liveness probes
- **Path traversal protection** — `realpath()` validation blocks `../../` escape attempts
- **Client read timeout** — `SO_RCVTIMEO` prevents slow/stalled clients from holding worker threads
- Compile-time debug logging (`-DDEBUG`)

## Build

```bash
make          # build the server binary
make clean    # remove build artifacts
make debug    # build with debug logging enabled
make asan     # build with AddressSanitizer
make tsan     # build with ThreadSanitizer
make run      # build and start on port 8080
```

Requires `gcc` and `pthreads` (any modern Linux).

## Usage

```bash
./server <port> <pool-size> <max-queue-size> <max-requests>
```

| Argument | Description |
|----------|-------------|
| `port` | TCP port to listen on |
| `pool-size` | Number of worker threads |
| `max-queue-size` | Maximum pending jobs in the queue |
| `max-requests` | Total connections before shutdown |

### Example

```bash
./server 8080 4 8 100
```

Access log output appears on stdout (includes response time and request ID):

```
webserver/1.0 listening on port 8080  (threads=4, queue=8, max-requests=100)
127.0.0.1 - - [01/Apr/2026:12:00:00 +0000] "GET /index.html" 200 0.42ms rid=1
127.0.0.1 - - [01/Apr/2026:12:00:01 +0000] "GET /missing" 404 0.11ms rid=2
```

```bash
# In another terminal:
curl -i http://localhost:8080/
curl -i http://localhost:8080/file.txt
```

Stop the server cleanly with `Ctrl-C` (sends `SIGINT`).

### Metrics

```bash
curl -s http://localhost:8080/stats | python3 -m json.tool
```

```json
{
  "uptime_seconds": 42,
  "total_requests": 156,
  "active_connections": 2,
  "pool_threads": 4,
  "queue_depth": 0,
  "queue_capacity": 8,
  "avg_latency_ms": 0.312,
  "status": { "2xx": 148, "3xx": 3, "4xx": 4, "5xx": 1 },
  "overloaded_connections": 0
}
```

### Health Check

```bash
curl -s http://localhost:8080/health
```

```json
{"status":"ok","uptime_seconds":42}
```

Suitable for Docker `HEALTHCHECK` or Kubernetes `livenessProbe`.

## Project Structure

```
├── server.c              Entry point and HTTP request handling
├── threadpool.c           Thread pool implementation
├── threadpool.h           Thread pool public interface
├── Makefile               Build targets
├── Dockerfile             Linux build & smoke test
├── tests/
│   └── test_server.sh     Integration test suite
├── docs/
│   └── reference-responses/
└── README.md
```

## Testing

```bash
chmod +x tests/test_server.sh
./tests/test_server.sh 9090
```

The test script builds the server, starts it against a temporary document root, exercises every HTTP response path, validates headers, tests concurrent requests, and confirms clean shutdown.

### Docker

```bash
docker build -t webserver-test .                          # compile + smoke test
docker run --rm webserver-test bash -c 'cd /app && bash tests/test_server.sh 9091'  # full suite
```

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| Fixed thread pool | Avoids per-request thread creation overhead and bounds resource usage |
| Bounded work queue | Back-pressure: the accept loop blocks when workers are saturated rather than consuming unbounded memory |
| Mutex + condition variables | Direct, portable synchronization — no external dependencies |
| HTTP/1.0 only | Keeps the scope focused; one request per connection simplifies the worker lifecycle |
| `realpath()` path validation | Resolves symlinks and `..` segments, then verifies the result stays under the document root |
| `SO_RCVTIMEO` client timeout | Caps time a worker blocks on `read()`, preventing resource exhaustion from slow clients |
| Signal-based shutdown | `SIGINT`/`SIGTERM` close the listening socket, unblocking `accept()` for a clean exit path |
| 503 on overload | Non-blocking dispatch — rejects new connections immediately when the queue is full, keeping the server responsive |
| Per-request latency | `clock_gettime(CLOCK_MONOTONIC)` measures wall-clock time per request; easy to spot regressions |
| `/stats` with C11 atomics | Lock-free `_Atomic` counters give live observability without contending with worker threads |
| `X-Request-Id` header | Monotonic atomic counter stamped on every response and in access logs — enables end-to-end request tracing |
| `/health` endpoint | Minimal JSON liveness probe for container orchestration (Docker HEALTHCHECK, Kubernetes) |

## Failure Modes Considered

| Failure Mode | Mitigation |
|---|---|
| Slow clients holding connections | `SO_RCVTIMEO` socket timeout (5 s) |
| Path traversal attacks (`../../etc/passwd`) | `realpath()` + document root validation |
| Queue saturation under burst load | Immediate `503` response instead of unbounded blocking |
| Shutdown during active requests | Signal handler sets flag, pool drains in-flight work, threads join cleanly |
| Unchecked memory allocation | Every `malloc` checked; failure returns error to client |

## Limitations

- Supports only HTTP/1.0 (no keep-alive connections)
- Only the `GET` method is implemented
- No TLS/HTTPS support
- Designed for learning and demonstration, not production deployment

## Benchmark

Tested inside a Docker container (`gcc:latest`, 2 threads, 10 connections, 5 s):

```
$ wrk -t2 -c10 -d5s http://127.0.0.1:9090/index.html

  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   303.12us  208.48us   3.95ms   78.69%
    Req/Sec    10.91k   739.59    12.21k    65.22%
  49988 requests in 5.01s, 9.48MB read
Requests/sec:   9970.95
Transfer/sec:      1.89MB
```

Under sustained load the server returns `503 Service Unavailable` for requests that arrive when the queue is full, keeping average latency predictable.

## License

See [LICENSE](LICENSE).
