#!/usr/bin/env bash
###############################################################################
# test_circuit_breaker.sh — Circuit breaker lifecycle integration test
#
# Verifies the full state machine:
#   CLOSED → OPEN (after consecutive failures) → HALF-OPEN (cooldown) → CLOSED
#
# Requires: curl, python3, a built ./proxy binary.
#
# Usage:  bash tests/test_circuit_breaker.sh
###############################################################################
set -euo pipefail

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); printf "  ✓ %s\n" "$1"; }
fail() { FAIL=$((FAIL+1)); printf "  ✗ %s\n" "$1"; }

BINARY="./proxy"
if [ ! -f "$BINARY" ]; then
    echo "Building..."
    make -j"$(nproc)" >/dev/null 2>&1
fi

TMPDIR=$(mktemp -d)
cleanup() {
    [ -n "${PROXY_PID:-}" ]   && kill "$PROXY_PID"   2>/dev/null; wait "$PROXY_PID"   2>/dev/null || true
    [ -n "${BACKEND1_PID:-}" ] && kill "$BACKEND1_PID" 2>/dev/null; wait "$BACKEND1_PID" 2>/dev/null || true
    [ -n "${BACKEND2_PID:-}" ] && kill "$BACKEND2_PID" 2>/dev/null; wait "$BACKEND2_PID" 2>/dev/null || true
    rm -rf "$TMPDIR" 2>/dev/null || true
}
trap cleanup EXIT

# ---------- Helper: start a Python echo backend ----------
start_echo_backend() {
    local port="$1" name="$2"
    python3 -c "
import http.server, socketserver
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type','text/plain')
        body = '$name'
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body.encode())
    def log_message(self, *a): pass
with socketserver.TCPServer(('', $port), H) as s:
    s.serve_forever()
" &
}

PORT_PROXY=9300
PORT_ADMIN=9310
PORT_B1=9301
PORT_B2=9302

# ---------- Start backends ----------
start_echo_backend $PORT_B1 "backend1"
BACKEND1_PID=$!
start_echo_backend $PORT_B2 "backend2"
BACKEND2_PID=$!
sleep 0.3

# ---------- Write config: threshold=3, cooldown=3s ----------
cat > "$TMPDIR/cb_proxy.conf" <<EOF
[proxy]
listen_port     = $PORT_PROXY
admin_port      = $PORT_ADMIN
worker_count    = 2
queue_capacity  = 16
upstream_connect_timeout_ms = 1000
upstream_read_timeout_ms    = 2000
max_request_body_bytes      = 1048576
log_format = clf
mode = proxy

[upstreams]
backend1 = 127.0.0.1:$PORT_B1
backend2 = 127.0.0.1:$PORT_B2

[loadbalancer]
strategy = round_robin

[healthcheck]
enabled      = false
interval_sec = 60
timeout_ms   = 1000
path         = /health

[ratelimit]
enabled = false
per_ip_per_minute = 600

[circuitbreaker]
enabled            = true
failure_threshold  = 3
cooldown_sec       = 3
EOF

"$BINARY" "$TMPDIR/cb_proxy.conf" &
PROXY_PID=$!
sleep 0.5

echo ""
echo "=== Circuit Breaker Lifecycle Test ==="
echo ""

# ---------- Phase 1: CLOSED — both backends healthy ----------
echo "--- Phase 1: CLOSED (both backends healthy) ---"
OK=0
for _ in $(seq 1 6); do
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:$PORT_PROXY/)
    [ "$STATUS" = "200" ] && OK=$((OK+1))
done
[ "$OK" -eq 6 ] && pass "All 6 requests return 200 (breaker CLOSED)" \
                 || fail "Expected 6x200, got $OK successes"

# ---------- Phase 2: Trigger OPEN — kill backend2 ----------
echo "--- Phase 2: Force OPEN (kill backend2, trigger failures) ---"
kill "$BACKEND2_PID" 2>/dev/null; wait "$BACKEND2_PID" 2>/dev/null || true
unset BACKEND2_PID
sleep 0.3

# Send enough requests to hit backend2 (via round-robin) 3+ times
# so the failure threshold is reached.
for _ in $(seq 1 12); do
    curl -s -o /dev/null http://127.0.0.1:$PORT_PROXY/ || true
done
sleep 0.3

# Check /stats for circuit_open_until > 0 on backend2
STATS=$(curl -s http://127.0.0.1:$PORT_PROXY/stats)
if echo "$STATS" | grep -q '"circuit_open_until"' ; then
    # Extract circuit_open_until for backend2
    # The stats JSON includes per-backend info; just check that at least one is non-zero
    OPEN_VAL=$(echo "$STATS" | grep -o '"circuit_open_until":[0-9]*' | head -1 | cut -d: -f2)
    [ "${OPEN_VAL:-0}" -gt 0 ] && pass "Circuit breaker OPEN (circuit_open_until=$OPEN_VAL)" \
                                 || fail "Circuit breaker not tripped (circuit_open_until=$OPEN_VAL)"
else
    fail "Stats don't include circuit_open_until field"
fi

# All requests should now go only to backend1
BODIES=""
for _ in $(seq 1 4); do
    BODY=$(curl -s http://127.0.0.1:$PORT_PROXY/)
    BODIES="$BODIES $BODY"
done
echo "$BODIES" | grep -q "backend2" \
    && fail "backend2 still receiving (breaker should be OPEN)" \
    || pass "No requests routed to backend2 (breaker OPEN)"

# ---------- Phase 3: HALF-OPEN → CLOSED — restart backend2, wait for cooldown ----------
echo "--- Phase 3: HALF-OPEN → CLOSED (restart backend2, wait cooldown) ---"
start_echo_backend $PORT_B2 "backend2"
BACKEND2_PID=$!
sleep 4  # cooldown_sec=3, wait a bit extra

# After cooldown, breaker should be half-open; a probe should succeed and close it
BODIES=""
for _ in $(seq 1 8); do
    BODY=$(curl -s http://127.0.0.1:$PORT_PROXY/)
    BODIES="$BODIES $BODY"
done
echo "$BODIES" | grep -q "backend2" \
    && pass "backend2 receiving traffic again (breaker CLOSED)" \
    || fail "backend2 still not receiving after cooldown"

# Verify circuit_open_until is back to 0
STATS=$(curl -s http://127.0.0.1:$PORT_PROXY/stats)
ALL_ZEROS=true
for val in $(echo "$STATS" | grep -o '"circuit_open_until":[0-9]*' | cut -d: -f2); do
    [ "$val" != "0" ] && ALL_ZEROS=false
done
$ALL_ZEROS && pass "All circuit breakers returned to CLOSED (0)" \
           || fail "Some circuit breakers still open after recovery"

# ---------- Summary ----------
echo ""
echo "==========================================="
echo "  Circuit Breaker Test: PASSED=$PASS  FAILED=$FAIL"
echo "==========================================="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
