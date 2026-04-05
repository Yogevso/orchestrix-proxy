# Orchestrix Proxy

> **Part of the [Orchestrix Platform](https://github.com/Yogevso/Orchestrix-Platform)** — the edge layer handling routing, load balancing, and failure isolation.

[![CI](https://github.com/Yogevso/orchestrix-proxy/actions/workflows/ci.yml/badge.svg)](https://github.com/Yogevso/orchestrix-proxy/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C11](https://img.shields.io/badge/C-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Docker](https://img.shields.io/badge/Docker-ready-blue.svg)](Dockerfile)

A production-style **Layer 7 reverse proxy** in C, built to demonstrate bounded-concurrency request handling, health-aware load balancing, failover, rate limiting, and operational observability under load.

> This project began as a concurrent static HTTP server and was later evolved into a reverse proxy architecture to explore real infrastructure patterns such as upstream routing, health checks, circuit breaking, and failure handling.

Unlike thread-per-connection designs, Orchestrix Proxy uses a fixed worker pool and bounded queue to preserve predictable latency and memory use under overload.

---

## Features

| Category | Capability |
|----------|-----------|
| **Reverse Proxy** | Accepts HTTP traffic, selects a healthy upstream, forwards the request, relays the response |
| **Load Balancing** | Round-robin and least-connections strategies |
| **Health Checks** | Periodic backend probes; unhealthy backends skipped automatically |
| **Failover** | If the chosen backend fails, the proxy retries another healthy backend |
| **Circuit Breaker** | Per-backend failure tracking; opens after threshold, cools down, allows exactly one probe request (half-open via CAS), recovers on success |
| **Connection Draining** | Backends can be set to DRAINING — no new traffic, in-flight requests complete |
| **Rate Limiting** | Per-client-IP fixed-window rate limiter; returns 429 Too Many Requests |
| **Request Body Limit** | Rejects oversized payloads with 413 Payload Too Large |
| **Bounded Concurrency** | Fixed-size thread pool + bounded FIFO queue; 503 on saturation |
| **Observability** | `/stats` (JSON), `/metrics` (Prometheus), `/health` (liveness), structured JSONL access logs |
| **Request Tracing** | Monotonic `X-Request-Id` on every response; logged for end-to-end correlation |
| **Header Hygiene** | Hop-by-hop header stripping; `X-Forwarded-For` / `X-Forwarded-Host` injection |
| **Priority Queue** | Path-prefix classification — high-priority requests jump to the head of the worker queue |
| **Chaos Mode** | Configurable latency injection and random request dropping for resilience testing |
| **Admin API** | Runtime backend control on a separate port (`/admin/backends/<name>/drain`, `/admin/reload`) |
| **Config Reload** | `SIGHUP` triggers live config reload without restart |
| **Observability Stack** | Docker Compose includes Prometheus scraping and a pre-built Grafana dashboard |
| **Legacy Mode** | Original static file server preserved as `--static` mode |

---

## Quick Start

### Build

```bash
make            # release build
make debug      # debug symbols + debug logging
make asan       # AddressSanitizer build
make tsan       # ThreadSanitizer build
```

### Run (Proxy Mode)

```bash
# Edit proxy.conf to point at your backends, then:
./proxy proxy.conf
```

### Run (Legacy Static Mode)

```bash
./proxy --static 8080 4 16
# port=8080, threads=4, queue=16
```

### Docker Compose (Full Demo)

```bash
docker compose up --build
```

This starts the proxy, three echo backends, Prometheus, and Grafana. Then:

```bash
curl http://localhost:8080/          # proxied response
curl http://localhost:8080/stats     # JSON metrics with per-backend breakdown
curl http://localhost:8080/metrics   # Prometheus exposition format
curl http://localhost:8080/health    # liveness probe
```

Grafana dashboard: [http://localhost:3000](http://localhost:3000) (admin / admin)

---

## Architecture

For in-depth design details, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). For failure handling, see [docs/FAILURE-MODES.md](docs/FAILURE-MODES.md).

```
                     ┌─────────────────────────────────────────────┐
                     │            Orchestrix Proxy                 │
                     │                                             │
  Client ──────────► │  Accept Loop                                │
                     │    │                                        │
                     │    ▼                                        │
                     │  Bounded Queue ──► Thread Pool (N workers)  │
                     │                       │                     │
                     │    ┌──────────────────┤                     │
                     │    ▼                  ▼                     │
                     │  Rate Limiter    Parse Request              │
                     │    │                  │                     │
                     │    ▼                  ▼                     │
                     │  Select Backend (Round-Robin / Least-Conn)  │
                     │    │                                        │
                     │    ▼                                        │
                     │  Connect → Forward → Relay → Log → Close   │
                     │         (failover on failure)               │
                     └─────────┬──────────┬──────────┬────────────┘
                               │          │          │
                               ▼          ▼          ▼
                           Backend 1  Backend 2  Backend 3
```

### Concurrency Model

- **One accept thread** — the main loop
- **Bounded FIFO queue** — configurable max depth
- **Fixed-size worker pool** — configurable thread count
- **Back-pressure** — when the queue is full, new connections get an immediate 503

This ensures predictable resource usage and prevents cascading overload.

### Request Lifecycle

1. Accept client connection
2. Peek at request line to classify priority (if enabled)
3. Enqueue into bounded work queue — high-priority at head, normal at tail (or 503 if full)
4. Worker picks up connection
5. Parse HTTP request line + headers
6. Check rate limit → 429 if exceeded
7. Check body size → 413 if oversized
8. Chaos injection (if enabled) → random drop or latency delay
9. Select healthy backend (round-robin or least-connections)
10. Connect to upstream (with timeout)
11. Forward request (strip hop-by-hop headers, inject `X-Forwarded-For`)
12. Relay response (inject `X-Request-Id`)
13. Update metrics and access log
14. Close both sockets

If the upstream connect/forward/relay fails, the proxy retries another healthy backend (failover).

---

## Configuration

The proxy reads an INI-style config file:

```ini
[proxy]
listen_port              = 8080
admin_port               = 8081
worker_count             = 4
queue_capacity           = 16
upstream_connect_timeout_ms = 2000
upstream_read_timeout_ms = 5000
max_request_body_bytes   = 10485760   ; 10 MB
log_format               = clf        ; clf | jsonl
mode                     = proxy      ; proxy | static

[upstreams]
backend1 = 127.0.0.1:9001
backend2 = 127.0.0.1:9002
backend3 = 127.0.0.1:9003

[loadbalancer]
strategy = round_robin    ; round_robin | least_connections

[healthcheck]
enabled      = true
interval_sec = 5
timeout_ms   = 2000
path         = /health

[ratelimit]
enabled          = true
per_ip_per_minute = 120

[circuitbreaker]
enabled           = true
failure_threshold = 5
cooldown_sec      = 30

[priority]
enabled = true
prefix1 = /api/checkout     ; high-priority path prefixes
prefix2 = /api/payments

[chaos]
enabled          = false      ; enable for resilience testing
inject_delay_ms  = 50         ; add latency to every request (ms)
drop_rate        = 0.05       ; randomly drop 5% of requests
```

### Live Reload

Send `SIGHUP` to reload the config without restarting:

```bash
kill -HUP $(pgrep proxy)
```

Hot-reloaded on SIGHUP: upstream list, load-balancer strategy, rate-limit settings. New backends start receiving traffic; removed backends enter DRAINING state. Other settings (worker count, queue depth, chaos, priority prefixes) require a full restart.

---

## Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /health` | `{"status":"ok","uptime_seconds":N}` |
| `GET /stats` | Full JSON metrics with per-backend breakdown |
| `GET /metrics` | Prometheus exposition format |
| `GET /admin/config` | Running configuration (admin port) |
| `PUT /admin/backends/<name>/drain` | Set backend to DRAINING |
| `PUT /admin/backends/<name>/enable` | Set backend to UP |
| `PUT /admin/reload` | Reload config from disk |

### Example `/stats` Response

```json
{
  "uptime_seconds": 120,
  "total_requests": 5000,
  "active_connections": 8,
  "pool_threads": 4,
  "queue_depth": 1,
  "queue_capacity": 16,
  "avg_latency_ms": 2.450,
  "status": { "2xx": 4800, "3xx": 0, "4xx": 180, "5xx": 20 },
  "overloaded_connections": 3,
  "rate_limited_requests": 12,
  "proxy_success": 4800,
  "upstream_failures": 5,
  "backends": [
    {"name":"backend1","state":"up","active":2,"requests":2200,"failures":1},
    {"name":"backend2","state":"up","active":1,"requests":2100,"failures":0},
    {"name":"backend3","state":"down","active":0,"requests":700,"failures":4}
  ]
}
```

---

## Project Structure

```
orchestrix-proxy/
├── Makefile                # Build system (all, debug, asan, tsan, legacy)
├── proxy.conf              # Default configuration file
├── docker-compose.yml           # Full demo stack (proxy + 3 backends)
├── docker-compose.orchestrix.yml # Platform gateway (Engine + Insights + IAM)
├── Dockerfile                   # Container build with smoke test
├── server.c                # Original monolithic server (legacy, preserved)
├── threadpool.c / .h       # Original threadpool (legacy, preserved)
├── src/
│   ├── main.c              # Entry point — proxy or static mode
│   ├── server_core.c/.h    # Listen socket, accept loop, signals
│   ├── threadpool.c/.h     # Fixed-size thread pool + bounded queue
│   ├── config.c/.h         # INI config parser
│   ├── http_parse.c/.h     # HTTP/1.x request/response parsing
│   ├── http_io.c/.h        # Connect, forward, relay, error responses
│   ├── proxy_handler.c/.h  # Core proxy flow (select → forward → relay)
│   ├── static_handler.c/.h # Legacy static file serving mode
│   ├── upstreams.c/.h      # Backend registry + load balancing
│   ├── circuit_breaker.c/.h# Per-backend circuit breaker
│   ├── healthcheck.c/.h    # Periodic health probes (background thread)
│   ├── rate_limit.c/.h     # Per-IP fixed-window rate limiter
│   ├── admin_handler.c/.h  # Admin API on separate port
│   ├── metrics.c/.h        # Atomic counters, /stats JSON, /metrics Prometheus
│   ├── logger.c/.h         # Access logging (CLF + JSONL formats)
│   ├── priority.c/.h       # Path-prefix priority classification
│   └── chaos.c/.h          # Chaos/failure injection (delay + drop)
├── docker/
│   ├── proxy.conf                # Config for docker-compose stack
│   ├── orchestrix-platform.conf  # Config for platform gateway stack
│   ├── prometheus.yml            # Prometheus scrape config
│   └── grafana/                  # Grafana provisioning + dashboard JSON
├── scripts/
│   └── demo-traffic.sh     # Multi-phase traffic generator
├── tests/
│   ├── test_proxy.sh       # Integration test suite for proxy
│   ├── test_circuit_breaker.sh # Circuit breaker lifecycle test
│   ├── test_server.sh      # Integration tests (legacy server)
│   ├── test_stats.sh       # Stats endpoint validation
│   ├── bench.sh            # wrk-based performance benchmark (static mode)
│   └── bench_proxy.sh      # wrk-based benchmark (proxy mode via Docker)
└── docs/
    ├── ARCHITECTURE.md     # Internal design: thread model, state machines, memory
    ├── FAILURE-MODES.md    # Every failure scenario and recovery pattern
    └── reference-responses/ # Example HTTP response templates
```

---

## Testing

```bash
# Full integration test suite (proxy mode)
make test
# or: bash tests/test_proxy.sh

# Circuit breaker lifecycle test
bash tests/test_circuit_breaker.sh

# Legacy server tests
bash tests/test_server.sh

# Stats endpoint validation
bash tests/test_stats.sh

# Performance benchmark (requires wrk)
bash tests/bench.sh
```

---

## Demo Scenarios

### 1. Round-Robin Balancing
```bash
docker compose up --build
for i in $(seq 1 9); do curl -s http://localhost:8080/; echo; done
# Output alternates: backend1, backend2, backend3, backend1, ...
```

### 2. Backend Failure + Failover
```bash
docker compose stop backend2
curl http://localhost:8080/         # still works — routed to backend1 or backend3
curl http://localhost:8080/stats    # shows backend2 state:"down", failures incremented
```

### 3. Queue Saturation (503)
```bash
# Reduce queue capacity in config, then flood:
for i in $(seq 1 100); do curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/ & done
# Some requests return 503 — bounded queue in action
```

### 4. Rate Limiting (429)
```bash
for i in $(seq 1 200); do curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/; done
# After per_ip_per_minute threshold: 429 Too Many Requests
```

### 5. Admin Control
```bash
# Drain a backend
curl -X PUT http://localhost:8081/admin/backends/backend2/drain
# Re-enable
curl -X PUT http://localhost:8081/admin/backends/backend2/enable
# Dump config
curl http://localhost:8081/admin/config
```

### 6. Prometheus Metrics
```bash
curl http://localhost:8080/metrics
# proxy_requests_total 500
# proxy_backend_healthy{backend="backend1"} 1
# proxy_backend_healthy{backend="backend2"} 0
# ...
```

### 7. Priority Queue Traffic Shaping
```bash
# With [priority] enabled and prefix1=/api/checkout:
# High-priority requests jump ahead in the worker queue
curl http://localhost:8080/api/checkout    # dispatched at head of queue
curl http://localhost:8080/browse          # dispatched at tail (normal)
```

### 8. Chaos Mode
```bash
# Enable in config: [chaos] enabled=true, inject_delay_ms=100, drop_rate=0.1
# 10% of requests randomly return 500, all requests get +100ms latency
for i in $(seq 1 20); do
  curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/
done
# Mix of 200s and 500s (chaos drops)
```

### 9. Full Demo Traffic Script
```bash
bash scripts/demo-traffic.sh
# Runs 4 phases: steady state → spike → backend failure → recovery
# Watch live in Grafana: http://localhost:3000
```

### 10. Observability Stack
```bash
docker compose up --build
# Proxy:       http://localhost:8080
# Prometheus:  http://localhost:9090
# Grafana:     http://localhost:3000  (admin / admin)
```

---

## Design Tradeoffs

| Decision | Alternative | Why This Choice |
|----------|-------------|-----------------|
| **Thread pool + bounded queue** | `epoll` / `io_uring` event loop | Simpler mental model; demonstrates bounded-concurrency semantics clearly. An event loop would scale better at 10k+ connections but adds complexity that obscures the proxy logic. |
| **HTTP/1.x only** | HTTP/2, QUIC | Keeps the parser straightforward (~200 LOC). The architecture could layer an `h2` framing stage on top without changing the proxy core. |
| **Fixed queue depth** | Dynamic / resizable queue | Predictable memory ceiling. A dynamic queue hides overload instead of signaling it early via 503. |
| **Static circuit breaker thresholds** | Adaptive / exponential back-off | Easier to reason about and test. A production system could extend `cb_record_failure()` with adaptive windowing without touching the rest of the codebase. |
| **Per-IP fixed-window rate limit** | Token bucket / sliding window | Minimal state per client (one counter + timestamp). Sufficient for demo; a sliding window upgrade requires only changing `ratelimit_allow()`. |
| **`MSG_PEEK` for priority** | Full parse before dispatch | Avoids buffering the entire request before queueing. A 256-byte peek on the socket captures the request line for path classification without copying. |
| **INI config rather than YAML/JSON** | YAML, TOML, JSON | Zero external dependencies. The parser is ~150 LOC with no allocator; hot-reload via SIGHUP re-reads the same file. |

---

## Benchmark Comparison

Run the proxy benchmark yourself:

```bash
# Start the docker stack, then:
bash tests/bench_proxy.sh          # proxy-mode benchmark (requires wrk)
bash tests/bench.sh                # legacy static-mode benchmark
```

Expected behaviour under load:

| Scenario | What to expect |
|----------|----------------|
| **Direct to backend** | Baseline throughput — no proxy overhead |
| **Through proxy (round-robin)** | Small latency overhead from accept → forward → relay hop |
| **With rate limit enabled** | Negligible additional cost (per-IP hash lookup) |
| **With chaos delay enabled** | Throughput drops proportionally to injected delay |
| **1 of 3 backends down** | Failover retry adds latency on the failed slot; healthy backends absorb traffic |
| **Queue saturated** | Excess connections get immediate 503; served requests maintain stable latency |

**Key takeaway**: The proxy adds roughly one extra network hop of latency per request, while preserving stable latency under overload thanks to bounded concurrency. The exact numbers depend on your hardware — run `bench_proxy.sh` to measure.

Baseline from the legacy static server: ~9,970 req/s (2 threads, 10 connections, 5s with `wrk`).

---

## Evolution

This project evolved through deliberate milestones:

1. **Concurrent HTTP Server** — fixed thread pool, bounded queue, file serving
2. **Refactor into Reusable Core** — split server lifecycle from static-serving logic
3. **Proxy MVP** — config parser, round-robin upstream selection, request forwarding
4. **Reliability** — health checks, failover, circuit breaker, connection draining
5. **Traffic Control** — per-IP rate limiting, request body limits
6. **Observability** — Prometheus metrics, JSONL logging, admin API
7. **Operational Polish** — SIGHUP reload, Docker Compose demo stack
8. **Advanced Features** — priority queue traffic shaping, chaos/failure simulation, Prometheus + Grafana stack, circuit breaker lifecycle test

The original monolithic `server.c` is preserved and can still be built with `make legacy`.

---

## Part of the Orchestrix Portfolio

This proxy is the front door for the Orchestrix platform. It load-balances, health-checks, and observes all HTTP traffic across the backend services:

```
                    ┌──────────────────────────┐
    Clients ──────► │   orchestrix-proxy (:8080)│
                    └────┬────────┬────────┬───┘
                         │        │        │
              ┌──────────▼──┐ ┌───▼──────┐ ┌▼──────────────┐
              │  Engine     │ │ Insights │ │  IAM          │
              │  :8000      │ │  :8000   │ │  :8000        │
              └─────────────┘ └──────────┘ └───────────────┘
```

| Service | Repo | Role |
|---------|------|------|
| **Orchestrix Engine** | [Orchestrix-Engine](https://github.com/Yogevso/Orchestrix-Engine) | Task orchestration, workflow scheduling, async workers |
| **System Insights API** | [system-insights-api](https://github.com/Yogevso/system-insights-api) | System telemetry collection and analytics |
| **Identity Access Service** | [identity-access-service](https://github.com/Yogevso/identity-access-service) | Authentication, RBAC, and JWT management |
| **Orchestrix Proxy** | *(this repo)* | L7 reverse proxy — routing, health checks, rate limiting, circuit breaking |
| **Orchestrix Console** | [orchestrix-console](https://github.com/Yogevso/orchestrix-console) | Web dashboard for the platform |
| **Orchestrix AI** | [orchestrix-ai](https://github.com/Yogevso/orchestrix-ai) | AI-powered task analysis and recommendations |

### Platform Gateway Demo

Run the full platform through the proxy (requires sibling repos checked out):

```bash
docker compose -f docker-compose.orchestrix.yml up --build
```

Then:

```bash
curl http://localhost:8080/health          # proxy liveness
curl http://localhost:8080/stats            # per-backend metrics (JSON)
curl http://localhost:8081/admin/config     # running config
```

> **Note**: The proxy load-balances (round-robin) across all three services — it does not route by path prefix. Requests may land on any backend. All three services expose `/health`, so health-checking works correctly. In a production setup you would front each service with its own proxy instance or add path-based routing.

Prometheus: [http://localhost:9090](http://localhost:9090) · Grafana: [http://localhost:3000](http://localhost:3000) (admin / admin)

---

## License

MIT License — see [LICENSE](LICENSE).
