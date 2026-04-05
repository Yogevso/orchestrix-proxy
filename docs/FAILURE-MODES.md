# Failure Modes

> How orchestrix-proxy handles each failure scenario. For architecture details, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Quick Reference

| Trigger | Response | HTTP Status | Metric Incremented |
|---------|----------|-------------|-------------------|
| Work queue full | Immediate rejection | **503** Service Unavailable | `overloaded` |
| Rate limit exceeded | Rejection | **429** Too Many Requests | `rate_limited` |
| Request body too large | Rejection | **413** Payload Too Large | `status_4xx` |
| Chaos drop (if enabled) | Simulated failure | **500** Internal Server Error | `status_5xx` |
| All backends exhausted | No upstream available | **502** Bad Gateway | `upstream_failures` |
| Upstream connect timeout | Failover to next backend | (retry) | per-backend `failures` |
| Upstream read timeout | Failover to next backend | (retry) | per-backend `failures` |
| Circuit breaker open | Backend skipped in selection | (next backend tried) | — |
| Backend draining | Backend skipped in selection | (next backend tried) | — |
| Backend health check fails | Backend marked DOWN | (auto-recovery when healthy) | — |
| Malformed HTTP request | Connection closed | — | — |
| Client read timeout (5s) | Connection closed | — | — |

---

## Detailed Scenarios

### Queue Full → 503

**When**: The bounded work queue has reached `queue_capacity` and a new connection arrives.

**What happens**:
1. `dispatch()` returns `-1` (non-blocking, does not wait).
2. Accept loop sends `HTTP/1.0 503 Service Unavailable` directly on the socket.
3. `g_metrics.overloaded` incremented atomically.
4. Socket closed immediately.

**Why 503**: Signals back-pressure to the client (or upstream load balancer). Preferable to unbounded queueing, which would increase latency for everyone.

**Tuning**: Increase `queue_capacity` in config. Consider whether high queue depth actually helps — deeper queues trade rejection rate for latency.

---

### Upstream Timeout → Failover / 504

**When**: `connect()` or `read()` to a backend exceeds the configured timeout (`upstream_connect_timeout_ms` / `upstream_read_timeout_ms`).

**What happens**:
1. Connect or relay fails with `ETIMEDOUT` or `ECONNREFUSED`.
2. `cb_record_failure()` called — increments the backend's `consecutive_failures`.
3. Proxy tries the **next healthy backend** (failover loop iterates all backends).
4. If all backends fail or are circuit-broken → `502 Bad Gateway`.

**Key detail**: The failover loop skips backends where `cb_is_open() == true`. If the circuit breaker has tripped, the backend isn't even attempted.

---

### Circuit Breaker Open → Backend Skipped

**When**: A backend has accumulated `failure_threshold` consecutive failures.

**What happens**:
1. `cb_record_failure()` sets `circuit_open_until = now + cooldown_sec`.
2. All subsequent `upstreams_select()` calls skip this backend (`cb_is_open()` returns 1).
3. After `cooldown_sec` elapses, `cb_is_open()` returns 0 — the backend is in **half-open** state.
4. The next request routed to it acts as a **probe**:
   - Success → `cb_record_success()` resets both atomics to 0 (CLOSED).
   - Failure → `cb_record_failure()` re-trips the breaker for another cooldown.

**Visibility**: The `/stats` JSON includes `circuit_open_until` per backend. Non-zero = breaker is open.

---

### Draining Backend → No New Traffic

**When**: An operator sends `PUT /admin/backends/<name>/drain`, or a backend is removed during config reload.

**What happens**:
1. `backend_t.state` set to `BACKEND_DRAINING` (atomically).
2. Both `select_round_robin()` and `select_least_connections()` skip any backend where `state != BACKEND_UP`.
3. In-flight requests on that backend complete normally — draining only affects new routing decisions.

**Re-enabling**: `PUT /admin/backends/<name>/enable` sets state back to `BACKEND_UP`.

**Limitation**: There is no grace period or active-connection countdown. Draining takes effect immediately for new requests. Requests already being forwarded are not interrupted.

---

### Rate-Limited Client → 429

**When**: A single client IP exceeds `per_ip_per_minute` requests within a 60-second fixed window.

**What happens**:
1. `ratelimit_allow(client_ip)` returns 0.
2. Handler sends `HTTP/1.0 429 Too Many Requests`.
3. `g_metrics.rate_limited` incremented.
4. Connection closed.

**Implementation**: djb2 hash into a 1024-bucket chained hash table protected by a single mutex. Each entry tracks `count` and `window_start`. The window resets when `now - window_start >= 60`.

**Edge case**: The rate limiter does not evict stale entries. Unique IPs accumulate until the rate limiter is destroyed (on SIGHUP reload or shutdown). Acceptable for bounded environments; would need an LRU eviction policy for public-facing deployment.

---

### All Backends Down → 502

**When**: Every backend is either DOWN, DRAINING, or circuit-broken, and no healthy upstream can be selected.

**What happens**:
1. `upstreams_select()` returns `-1`.
2. Or the failover loop exhausts all backends without a successful relay.
3. Handler sends `HTTP/1.0 502 Bad Gateway`.
4. `g_metrics.upstream_failures` incremented.

**Recovery**: The health-check thread continues probing backends in the background. Once a backend responds to the health check, it's marked UP and becomes available for the next request.

---

### Chaos Drop → 500

**When**: Chaos mode is enabled and `rand()` produces a value below `drop_rate`.

**What happens**:
1. `chaos_should_drop()` returns 1.
2. Handler sends `HTTP/1.0 500 Internal Server Error` with body `[CHAOS] Simulated failure.`
3. Request is logged with upstream field `chaos-drop`.
4. No backend is contacted.

**Purpose**: Resilience testing — verify that clients handle 500s gracefully and that monitoring dashboards react to error spikes.

---

### Chaos Delay → Increased Latency

**When**: Chaos mode is enabled and `inject_delay_ms > 0`.

**What happens**:
1. `chaos_inject_delay()` calls `nanosleep()` with the configured delay.
2. The worker thread is blocked for that duration before proceeding to backend selection.
3. Total request latency increases by approximately `inject_delay_ms`.

**Purpose**: Simulate slow upstream or network latency. Useful for testing timeout handling in clients and verifying that the bounded queue applies back-pressure under artificially high latency.

---

## Recovery Patterns

| Failure | Recovery mechanism | Automatic? |
|---------|-------------------|-----------|
| Backend crash | Health check detects failure → marks DOWN → re-probes → marks UP on recovery | Yes |
| Circuit breaker trip | Cooldown timer expires → half-open → probe succeeds → CLOSED | Yes |
| Rate limit window | 60-second window resets automatically | Yes |
| Queue saturation | Clears as workers complete in-flight requests | Yes |
| Draining backend | Requires manual `PUT /admin/backends/<name>/enable` or config reload | No |
| Config change | `SIGHUP` or `PUT /admin/reload` | Manual trigger |
