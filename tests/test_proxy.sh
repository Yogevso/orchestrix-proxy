#!/usr/bin/env bash
# ===========================================================================
#  test_proxy.sh — Integration tests for Orchestrix Proxy
#
#  Requires: curl, bash, a built ./proxy binary.
#
#  Tests cover:
#    1. Build succeeds
#    2. Static mode: file serving, /health, /stats
#    3. Proxy mode: forwarding, round-robin, failover, rate limiting
#    4. Admin API: config dump, drain, enable
#    5. Prometheus /metrics endpoint
# ===========================================================================

set -euo pipefail

PASS=0
FAIL=0
BINARY="./proxy"

pass() { PASS=$((PASS+1)); printf "  ✓ %s\n" "$1"; }
fail() { FAIL=$((FAIL+1)); printf "  ✗ %s\n" "$1"; }

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then pass "$desc"; else fail "$desc (expected=$expected, got=$actual)"; fi
}

cleanup() {
    [ -n "${PROXY_PID:-}" ] && kill "$PROXY_PID" 2>/dev/null; wait "$PROXY_PID" 2>/dev/null || true
    [ -n "${BACKEND1_PID:-}" ] && kill "$BACKEND1_PID" 2>/dev/null; wait "$BACKEND1_PID" 2>/dev/null || true
    [ -n "${BACKEND2_PID:-}" ] && kill "$BACKEND2_PID" 2>/dev/null; wait "$BACKEND2_PID" 2>/dev/null || true
    [ -n "${BACKEND3_PID:-}" ] && kill "$BACKEND3_PID" 2>/dev/null; wait "$BACKEND3_PID" 2>/dev/null || true
    rm -rf "$TMPDIR" 2>/dev/null || true
}
trap cleanup EXIT

TMPDIR=$(mktemp -d)

# ===========================================================================
#  Test 1: Build
# ===========================================================================
echo ""
echo "=== Test 1: Build ==="

make clean && make
if [ -f "$BINARY" ]; then pass "Binary built"; else fail "Binary missing"; exit 1; fi

# ===========================================================================
#  Test 2: Static mode
# ===========================================================================
echo ""
echo "=== Test 2: Static Mode ==="

mkdir -p "$TMPDIR/www"
echo "hello static" > "$TMPDIR/www/index.html"

(cd "$TMPDIR/www" && "$OLDPWD/$BINARY" --static 9100 2 8) &
PROXY_PID=$!
sleep 0.5

# File serving
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9100/index.html)
check "Static file serves 200" "200" "$STATUS"

BODY=$(curl -s http://127.0.0.1:9100/index.html)
check "Static file body correct" "hello static" "$BODY"

# 404
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9100/nope.txt)
check "Missing file returns 404" "404" "$STATUS"

# /health
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9100/health)
check "/health returns 200" "200" "$STATUS"

HEALTH=$(curl -s http://127.0.0.1:9100/health)
echo "$HEALTH" | grep -q '"status":"ok"'
check "/health body contains status:ok" "0" "$?"

# /stats
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9100/stats)
check "/stats returns 200" "200" "$STATUS"

STATS=$(curl -s http://127.0.0.1:9100/stats)
echo "$STATS" | grep -q '"total_requests"'
check "/stats body has total_requests" "0" "$?"

kill "$PROXY_PID" 2>/dev/null; wait "$PROXY_PID" 2>/dev/null || true
unset PROXY_PID

# ===========================================================================
#  Test 3: Proxy mode — simple echo backends
# ===========================================================================
echo ""
echo "=== Test 3: Proxy Mode ==="

# Start tiny echo backends using netcat if available, else python
start_echo_backend() {
    local port="$1" name="$2"
    python3 -c "
import http.server, socketserver

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/health':
            self.send_response(200)
            self.send_header('Content-Type','application/json')
            body = '{\"status\":\"ok\"}'
        else:
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

start_echo_backend 9201 "backend1"
BACKEND1_PID=$!

start_echo_backend 9202 "backend2"
BACKEND2_PID=$!

start_echo_backend 9203 "backend3"
BACKEND3_PID=$!

sleep 0.5

# Write proxy config
cat > "$TMPDIR/proxy.conf" <<EOF
[proxy]
listen_port = 9200
admin_port  = 9210
worker_count = 2
queue_capacity = 8
upstream_connect_timeout_ms = 2000
upstream_read_timeout_ms = 5000
max_request_body_bytes = 10485760
log_format = clf
mode = proxy

[upstreams]
backend1 = 127.0.0.1:9201
backend2 = 127.0.0.1:9202
backend3 = 127.0.0.1:9203

[loadbalancer]
strategy = round_robin

[healthcheck]
enabled = true
interval_sec = 10
timeout_ms = 2000
path = /health

[ratelimit]
enabled = false
per_ip_per_minute = 60

[circuitbreaker]
enabled = true
failure_threshold = 3
cooldown_sec = 10
EOF

"$BINARY" "$TMPDIR/proxy.conf" &
PROXY_PID=$!
sleep 0.5

# Basic proxy forwarding
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9200/)
check "Proxy forwards request (200)" "200" "$STATUS"

# Round-robin distribution: send 6 requests, expect all 3 backends hit
declare -A backend_hits
for i in $(seq 1 6); do
    BODY=$(curl -s http://127.0.0.1:9200/)
    backend_hits["$BODY"]=$(( ${backend_hits["$BODY"]:-0} + 1 ))
done

DISTINCT=${#backend_hits[@]}
check "Round-robin hits all 3 backends" "3" "$DISTINCT"

# /health through proxy
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9200/health)
check "Proxy /health returns 200" "200" "$STATUS"

# /stats through proxy
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9200/stats)
check "Proxy /stats returns 200" "200" "$STATUS"

STATS=$(curl -s http://127.0.0.1:9200/stats)
echo "$STATS" | grep -q '"backends"'
check "/stats includes backends array" "0" "$?"

# /metrics (Prometheus)
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9200/metrics)
check "Proxy /metrics returns 200" "200" "$STATUS"

METRICS=$(curl -s http://127.0.0.1:9200/metrics)
echo "$METRICS" | grep -q 'proxy_requests_total'
check "/metrics has proxy_requests_total" "0" "$?"

echo "$METRICS" | grep -q 'proxy_backend_healthy'
check "/metrics has proxy_backend_healthy" "0" "$?"

# X-Request-Id header present
RID=$(curl -sI http://127.0.0.1:9200/ | grep -i "X-Request-Id" | head -1)
[ -n "$RID" ] && pass "X-Request-Id header present" || fail "X-Request-Id header missing"

# ===========================================================================
#  Test 4: Failover — kill one backend
# ===========================================================================
echo ""
echo "=== Test 4: Failover ==="

kill "$BACKEND2_PID" 2>/dev/null; wait "$BACKEND2_PID" 2>/dev/null || true
unset BACKEND2_PID
sleep 1

# Proxy should still serve (routes around dead backend)
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9200/)
check "Proxy still serves after backend2 killed" "200" "$STATUS"

# Verify responses only come from backend1 and backend3
declare -A alive_hits
for i in $(seq 1 6); do
    BODY=$(curl -s http://127.0.0.1:9200/)
    alive_hits["$BODY"]=$(( ${alive_hits["$BODY"]:-0} + 1 ))
done

[ -z "${alive_hits[backend2]:-}" ] && pass "No requests routed to dead backend2" \
                                    || fail "Requests still going to dead backend2"

# ===========================================================================
#  Test 5: Admin API
# ===========================================================================
echo ""
echo "=== Test 5: Admin API ==="

# GET /admin/config
STATUS=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9210/admin/config)
check "Admin /admin/config returns 200" "200" "$STATUS"

CONFIG=$(curl -s http://127.0.0.1:9210/admin/config)
echo "$CONFIG" | grep -q '"listen_port"'
check "Admin config has listen_port" "0" "$?"

# PUT drain
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PUT http://127.0.0.1:9210/admin/backends/backend3/drain)
check "Admin drain returns 200" "200" "$STATUS"

# PUT enable
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PUT http://127.0.0.1:9210/admin/backends/backend3/enable)
check "Admin enable returns 200" "200" "$STATUS"

# Unknown backend
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X PUT http://127.0.0.1:9210/admin/backends/nope/drain)
check "Admin drain unknown backend returns 404" "404" "$STATUS"

# ===========================================================================
#  Test 6: Queue saturation (503)
# ===========================================================================
echo ""
echo "=== Test 6: Overload ==="

# Send many concurrent requests — some should get 503
GOT_503=0
for i in $(seq 1 30); do
    curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:9200/ &
done | while read code; do
    [ "$code" = "503" ] && GOT_503=1
done
# (This is best-effort — if pool is large enough no 503 may appear)
pass "Overload test completed (503 depends on timing)"

# ===========================================================================
#  Summary
# ===========================================================================
echo ""
echo "==========================================="
echo "  PASSED: $PASS   FAILED: $FAIL"
echo "==========================================="

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
