# Architecture

> Internal design of orchestrix-proxy. For usage and configuration, see [README.md](../README.md).

---

## Thread Model

```
Main Thread                     Worker Thread 1
─────────────                   ───────────────
accept()                        wait(q_not_empty)
  ↓                               ↓
MSG_PEEK → priority_classify    dequeue from head
  ↓                               ↓
dispatch() or dispatch_priority()  proxy_handle_client()
  ↓                               ↓
  ↓                             parse → rate-limit → chaos
  ↓                               ↓
  ↓                             select backend → connect
  ↓                               ↓
  ↓                             forward → relay → log → close
  ↓
(loop)                          Worker Thread 2 ...
                                Worker Thread N ...

Background Threads
──────────────────
Health Check Thread   — periodic probes to all backends
Admin API Thread      — accept loop on admin_port (separate socket)
```

There is exactly **one accept thread** (the main thread), **N worker threads** (configurable via `worker_count`), one health-check thread, and one admin-API thread. All share the same process address space.

### Synchronization

| Resource | Protection | Details |
|----------|-----------|---------|
| Work queue | `pthread_mutex_t qlock` + 3 condition variables | `q_not_empty`, `q_not_full`, `q_empty` |
| Backend state | `atomic_int` per field | `state`, `active_connections`, `consecutive_failures`, `circuit_open_until` |
| Backend list | `pthread_mutex_t lock` | Only held during `upstreams_reload()` (add/remove) |
| Metrics counters | Lock-free `atomic_ulong` | `atomic_fetch_add` for increments, `atomic_load` for reads |
| Rate limit table | `pthread_mutex_t rl_lock` | Single global lock for hash table access |
| Request ID | `atomic_ulong g_request_id` | Monotonically incrementing; `__thread` TLS copy for logging |

---

## Queue Behaviour

The work queue is a **singly-linked list** of `work_t` nodes with head/tail pointers.

```
dispatch() appends at TAIL     dispatch_priority() inserts at HEAD
             ↓                              ↓
  ┌───┬───┬───┬───┐            ┌───┬───┬───┬───┐
  │ H │   │   │ T │            │ H │   │   │ T │
  └───┴───┴───┴───┘            └───┴───┴───┴───┘
    ↑ dequeue                    ↑ dequeue (priority items first)
```

**Bounded**: `qsize` is tracked; if `qsize >= max_qsize`, `dispatch()` returns `-1` immediately (non-blocking). The accept loop then sends HTTP 503 and closes the socket.

**Back-pressure signal**: The 503 is intentional — it tells clients and load balancers upstream that the proxy is at capacity. This is preferable to unbounded queueing, which would increase latency for all requests.

**Drain on shutdown**: `destroy_threadpool()` sets `dont_accept = 1`, waits for the queue to drain via `q_empty`, then sets `shutdown = 1` and joins all threads. In-flight work completes before exit.

---

## Upstream State Machine

Each backend has a `state` field (`atomic_int`) with three values:

```
            upstreams_init() / admin enable
                    │
                    ▼
              ┌──────────┐
              │    UP     │ ◄──────── admin enable
              │  (active) │
              └────┬─────┘
                   │
         admin drain / reload removes
                   │
                   ▼
              ┌──────────┐
              │ DRAINING  │  ─── no new traffic routed here
              │           │      in-flight requests complete
              └────┬─────┘
                   │
         health check fails / connect fails
                   │
                   ▼
              ┌──────────┐
              │   DOWN    │  ─── skipped by selection;
              │           │      health check probes for recovery
              └────┬─────┘
                   │
         health check succeeds
                   │
                   ▼
              ┌──────────┐
              │    UP     │
              └──────────┘
```

**Selection rules**: Both `select_round_robin()` and `select_least_connections()` skip any backend where `state != BACKEND_UP` or `cb_is_open() == true`.

**Reload behaviour** (`upstreams_reload()`): Backends in the old config but not the new one → `DRAINING`. New backends → appended as `UP`. Existing backends get their host/port updated in-place.

---

## Circuit Breaker Lifecycle

State is encoded in three atomics on `backend_t`: `consecutive_failures`, `circuit_open_until`, and `half_open_probe`.

```
                 consecutive_failures < threshold
                          │
                          ▼
                    ┌──────────┐
           ┌───────│  CLOSED   │◄───────────────────────┐
           │       │ (open=0)  │                         │
           │       └─────┬─────┘                         │
           │             │                               │
           │   failures >= threshold                     │
           │             │                               │
           │             ▼                               │
           │       ┌──────────┐                          │
           │       │   OPEN    │                         │
           │       │ (open=T)  │  requests skipped       │
           │       └─────┬─────┘                          │
           │             │                               │
           │   time(NULL) >= circuit_open_until           │
           │             │                               │
           │             ▼                               │
           │       ┌──────────┐                          │
           │       │HALF-OPEN  │  CAS(half_open_probe,   │
  success  │       │ (probe)   │    0→1) gates exactly   │
  resets   │       │           │    one probe request    │
  all 3    │       └─────┬─────┘                          │
  fields   │             │                               │
           │      success│         failure               │
           │             │            │                  │
           └─────────────┘            │  re-trips:       │
                                      │  open_until =    │
                                      │  now + cooldown  │
                                      │  probe flag = 0  │
                                      └──────────────────┘
```

**Implementation**:
- `cb_is_open()` — returns 1 when `now < circuit_open_until` (OPEN). When cooldown expires, uses `atomic_compare_exchange_strong(&b->half_open_probe, 0, 1)` to allow exactly one probe request through. Subsequent requests see the CAS fail and remain blocked.
- `cb_record_failure()` — atomically increments `consecutive_failures`. If ≥ threshold, sets `circuit_open_until = now + cooldown_sec` and resets `half_open_probe = 0` so the next half-open window can issue a fresh probe.
- `cb_record_success()` — atomically stores 0 to all three fields (resets to CLOSED).

The CAS-based probe gate ensures that under concurrent access, exactly one worker thread acts as the probe during half-open — all others see the breaker as still open.

---

## Admin API Flow

The admin API runs on a **separate thread** with its own accept loop on `admin_port`.

```
Admin Thread
────────────
  listen(admin_port)
      │
  accept() ──► parse minimal HTTP ──► route:
      │
      ├── GET  /admin/config     → serialize proxy_config_t as JSON
      ├── PUT  /admin/backends/<name>/drain → upstreams_set_state(idx, DRAINING)
      ├── PUT  /admin/backends/<name>/enable → upstreams_set_state(idx, UP)
      └── PUT  /admin/reload     → config_load() + upstreams_reload()
```

The admin thread shares `g_proxy_cfg` and `g_upstreams` with the worker threads. State changes through atomics (`atomic_store(&b->state, ...)`) are immediately visible to workers on the next backend selection.

Admin endpoints are simple enough to handle synchronously on one thread — no pool needed.

---

## Config Reload Behaviour

On `SIGHUP`:

```
Signal arrives → sig_reload() sets g_reload = 1
                               │
accept() returns EINTR ────────┘
                               │
main loop checks g_reload ─────┘
           │
           ▼
  config_load(path, &new_cfg)  ← stack-local proxy_config_t
           │
           ├── upstreams_reload(&new_cfg)   ← add/drain/update backends
           ├── ratelimit_destroy() + init() ← new per_ip_per_minute
           └── hot-swap on g_proxy_cfg:
                 ├── lb_strategy           ✓ reloaded
                 ├── ratelimit_enabled     ✓ reloaded
                 ├── ratelimit_per_ip_per_minute ✓ reloaded
                 ├── worker_count          ✗ requires restart
                 ├── queue_capacity        ✗ requires restart
                 ├── listen_port           ✗ requires restart
                 ├── priority config       ✗ requires restart
                 ├── chaos config          ✗ requires restart
                 └── healthcheck config    ✗ requires restart
```

The reload path is intentionally conservative — only fields that can be safely swapped without disrupting the thread pool or accept loop are updated at runtime.

---

## Memory Ownership

| Object | Allocated by | Freed by | Lifetime |
|--------|-------------|----------|----------|
| `proxy_config_t cfg` | Stack in `main()` | Automatic | Process lifetime |
| `threadpool` | `create_threadpool()` via `malloc` | `destroy_threadpool()` | Process lifetime |
| `work_t` | `dispatch()` / `dispatch_priority()` via `malloc` | `do_work()` after handler returns | One request |
| `client_ctx_t` | `server_accept_loop()` via `malloc` | Handler (`proxy_handle_client` or `static_handle_client`), or accept loop on 503 | One request |
| `http_request_t` | Stack in handler | Automatic | One request |
| `rl_entry_t` | `ratelimit_allow()` via `calloc` | `ratelimit_destroy()` | Until rate limiter reset |

**Per-request heap allocations**: Exactly two — `client_ctx_t` (32 bytes) and `work_t` (24 bytes). The HTTP request buffer (`raw[8192]`) is stack-allocated in the worker thread. This keeps allocator pressure minimal under load.

**No global heap growth** under normal operation, except the rate-limit hash table which grows monotonically with unique client IPs. Entries are reclaimed only on shutdown or SIGHUP reload (which destroys and re-inits the table).

---

## Health Check Thread

```
healthcheck_start()
       │
  pthread_create → healthcheck_loop():
       │
  loop forever (interval_sec sleep):
       │
       ├── for each backend:
       │     ├── if state == DRAINING → skip
       │     ├── TCP connect + "GET <path> HTTP/1.0"
       │     ├── if 200 OK → state = UP, reset failures
       │     └── if timeout/connect fail → state = DOWN
       │
       └── sleep(interval_sec)
```

The health-check thread runs completely independently of the worker threads. It writes `backend_t.state` atomically; workers see the update on their next `upstreams_select()` call.

---

## Request Lifecycle (Complete)

```
     Client
       │
       │ TCP connect
       ▼
  ┌─ Accept Loop (main thread) ─────────────────────────────┐
  │                                                          │
  │  accept() → set SO_RCVTIMEO                              │
  │  MSG_PEEK (512 bytes) → extract path                     │
  │  priority_classify(path) → HIGH or NORMAL                │
  │                                                          │
  │  HIGH → dispatch_priority() (head of queue)              │
  │  NORMAL → dispatch() (tail of queue)                     │
  │  FULL → HTTP 503, close, increment overloaded counter    │
  └──────────────────────────┬───────────────────────────────┘
                             │
                     ┌───────▼─────────┐
                     │  Worker Thread   │
                     │                  │
                     │  1. read(fd) into stack buffer        │
                     │  2. http_parse_request()              │
                     │  3. route: /health → serve_health     │
                     │          /stats → serve_stats          │
                     │          /metrics → serve_metrics      │
                     │          else → proxy flow             │
                     │                                        │
                     │  4. ratelimit_allow(ip)                │
                     │     → 429 if exceeded                  │
                     │                                        │
                     │  5. body size check                    │
                     │     → 413 if over limit                │
                     │                                        │
                     │  6. chaos_should_drop()                │
                     │     → 500 if chaos drop                │
                     │     chaos_inject_delay()               │
                     │                                        │
                     │  7. upstreams_select()                 │
                     │     → skip DOWN, DRAINING, cb_open     │
                     │                                        │
                     │  8. connect(backend, timeout)          │
                     │  9. forward_request()                  │
                     │     (strip hop-by-hop, add XFF/XFH)    │
                     │  10. relay_response()                  │
                     │      (inject X-Request-Id)             │
                     │                                        │
                     │  11. cb_record_success()               │
                     │  12. metrics_record_request()          │
                     │  13. log_access()                      │
                     │  14. close(client_fd)                  │
                     │                                        │
                     │  On connect/forward/relay FAILURE:     │
                     │    cb_record_failure()                 │
                     │    retry next healthy backend          │
                     │    if all exhausted → 502 Bad Gateway  │
                     └────────────────────────────────────────┘
```
