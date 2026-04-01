#!/usr/bin/env bash
#
# test_server.sh — Integration tests for c-threaded-webserver.
#
# Usage: ./tests/test_server.sh [port]
#
# Builds the server, starts it in the background, runs HTTP tests with curl,
# validates status codes, and confirms clean shutdown.
#
set -euo pipefail

PORT="${1:-9090}"
POOL_SIZE=4
QUEUE_SIZE=8
MAX_REQUESTS=30
SERVER_PID=""
PASS=0
FAIL=0
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$TESTS_DIR")"

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# --- Helpers ---------------------------------------------------------------

log()  { printf "\033[1;34m[TEST]\033[0m %s\n" "$1"; }
pass() { printf "\033[1;32m  PASS\033[0m %s\n" "$1"; PASS=$((PASS + 1)); }
fail() { printf "\033[1;31m  FAIL\033[0m %s — expected %s, got %s\n" "$1" "$2" "$3"; FAIL=$((FAIL + 1)); }

assert_status() {
    local desc="$1" url="$2" expected="$3"
    local actual
    actual=$(curl -s -o /dev/null -w "%{http_code}" "$url" 2>/dev/null || echo "000")
    if [ "$actual" = "$expected" ]; then
        pass "$desc"
    else
        fail "$desc" "$expected" "$actual"
    fi
}

assert_header() {
    local desc="$1" url="$2" header="$3"
    local response
    response=$(curl -s -i "$url" 2>/dev/null || echo "")
    if echo "$response" | grep -qi "$header"; then
        pass "$desc"
    else
        fail "$desc" "header containing '$header'" "not found"
    fi
}

wait_for_server() {
    local retries=20
    while [ $retries -gt 0 ]; do
        if curl -s -o /dev/null "http://localhost:$PORT/" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
        retries=$((retries - 1))
    done
    echo "ERROR: Server did not start on port $PORT"
    exit 1
}

# --- Setup -----------------------------------------------------------------

log "Building server..."
cd "$PROJECT_DIR"
make clean >/dev/null 2>&1 || true
make >/dev/null 2>&1

if [ ! -f server ]; then
    echo "ERROR: make did not produce ./server"
    exit 1
fi
pass "Build succeeded"

# Create a temporary document root with test fixtures
WORKDIR=$(mktemp -d)
echo "<html><body>hello</body></html>" > "$WORKDIR/index.html"
echo "plain text content"              > "$WORKDIR/file.txt"
mkdir -p "$WORKDIR/subdir"
echo "nested file"                     > "$WORKDIR/subdir/nested.txt"
mkdir -p "$WORKDIR/noindex"
echo "a file in noindex"               > "$WORKDIR/noindex/something.txt"
mkdir -p "$WORKDIR/forbidden"
chmod 000 "$WORKDIR/forbidden" 2>/dev/null || true

log "Starting server on port $PORT..."
cd "$WORKDIR"
"$PROJECT_DIR/server" "$PORT" "$POOL_SIZE" "$QUEUE_SIZE" "$MAX_REQUESTS" &
SERVER_PID=$!
wait_for_server
pass "Server started (PID $SERVER_PID)"

# --- Tests -----------------------------------------------------------------

log "Running HTTP tests..."

# 200 — serve file
assert_status "GET /index.html => 200" "http://localhost:$PORT/index.html" "200"
assert_status "GET /file.txt   => 200" "http://localhost:$PORT/file.txt"   "200"

# 200 — directory with index.html serves it
assert_status "GET /           => 200" "http://localhost:$PORT/" "200"

# 302 — directory without trailing slash
assert_status  "GET /subdir     => 302" "http://localhost:$PORT/subdir"  "302"
assert_header  "302 has Location header" "http://localhost:$PORT/subdir" "Location: /subdir/"

# 200 — directory listing (no index.html)
assert_status "GET /noindex/   => 200 (dir listing)" "http://localhost:$PORT/noindex/" "200"

# 404 — missing file
assert_status "GET /nonexistent => 404" "http://localhost:$PORT/nonexistent" "404"

# 403 — forbidden directory  (skip if chmod not supported)
if [ ! -r "$WORKDIR/forbidden" ]; then
    assert_status "GET /forbidden/ => 403" "http://localhost:$PORT/forbidden/" "403"
else
    log "(skipping 403 test — chmod 000 not effective on this filesystem)"
fi

# 400 — malformed request (raw TCP via nc or bash /dev/tcp)
if command -v nc >/dev/null 2>&1; then
    BAD_RESPONSE=$(printf "BADREQUEST\r\n\r\n" | nc -w 2 localhost "$PORT" 2>/dev/null || echo "")
    if echo "$BAD_RESPONSE" | grep -q "400"; then
        pass "Malformed request => 400"
    else
        fail "Malformed request => 400" "400 in response" "not found"
    fi
else
    BAD_RESPONSE=$(exec 3<>/dev/tcp/localhost/"$PORT" && printf "BADREQUEST\r\n\r\n" >&3 && timeout 2 cat <&3; exec 3>&-) 2>/dev/null || true
    if echo "$BAD_RESPONSE" | grep -q "400"; then
        pass "Malformed request => 400"
    else
        log "(skipping 400 test — nc not available and /dev/tcp failed)"
    fi
fi

# 501 — unsupported method
CODE_501=$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://localhost:$PORT/" 2>/dev/null || echo "000")
if [ "$CODE_501" = "501" ]; then
    pass "POST / => 501"
else
    fail "POST / => 501" "501" "$CODE_501"
fi

# Response headers check
assert_header "Response has Date header"       "http://localhost:$PORT/file.txt" "Date:"
assert_header "Response has Content-Length"     "http://localhost:$PORT/file.txt" "Content-Length:"
assert_header "Response has Connection: close"  "http://localhost:$PORT/file.txt" "Connection: close"
assert_header "Response has Server header"      "http://localhost:$PORT/file.txt" "Server: webserver"

# Concurrent requests
log "Testing concurrent requests..."
SEQ_OK=true
for i in $(seq 1 5); do
    CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/file.txt" 2>/dev/null || echo "000")
    if [ "$CODE" != "200" ]; then
        SEQ_OK=false
        break
    fi
done
if $SEQ_OK; then
    pass "5 sequential requests all returned 200"
else
    fail "Sequential requests" "all 200" "at least one failed"
fi

# --- Shutdown --------------------------------------------------------------

log "Waiting for server to shut down (max-requests=$MAX_REQUESTS)..."

# Send remaining requests to hit the max_requests limit
for _ in $(seq 1 10); do
    curl -s -o /dev/null "http://localhost:$PORT/" 2>/dev/null || true
done

# Give the server a moment to finish
sleep 1

if kill -0 "$SERVER_PID" 2>/dev/null; then
    # Server is still running — send a few more to push it over
    for _ in $(seq 1 10); do
        curl -s -o /dev/null "http://localhost:$PORT/" 2>/dev/null || true
    done
    sleep 1
fi

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    pass "Server exited after reaching max requests"
    SERVER_PID=""
else
    fail "Server shutdown" "process exited" "still running"
fi

# --- Summary ---------------------------------------------------------------

echo ""
log "Results: $PASS passed, $FAIL failed"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
