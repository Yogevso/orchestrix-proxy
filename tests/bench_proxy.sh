#!/usr/bin/env bash
###############################################################################
# bench_proxy.sh — Benchmark the proxy in round-robin mode via Docker Compose.
#
# Prerequisites:
#   - docker compose running: docker compose up --build -d
#   - wrk installed: https://github.com/wg/wrk
#
# Usage:
#   bash tests/bench_proxy.sh [proxy-url]
#   Default: http://localhost:8080
###############################################################################
set -euo pipefail

PROXY="${1:-http://localhost:8080}"
THREADS=4
CONNECTIONS=50
DURATION=10s

green() { printf '\033[1;32m%s\033[0m\n' "$*"; }

if ! command -v wrk &>/dev/null; then
    echo "Error: wrk not found. Install it first: https://github.com/wg/wrk"
    exit 1
fi

# Verify proxy is reachable
if ! curl -sf -o /dev/null "$PROXY/health"; then
    echo "Error: Proxy not reachable at $PROXY"
    echo "Start the stack first: docker compose up --build -d"
    exit 1
fi

echo ""
green "=== Orchestrix Proxy Benchmark ==="
echo "Target:      $PROXY"
echo "Threads:     $THREADS"
echo "Connections: $CONNECTIONS"
echo "Duration:    $DURATION"
echo ""

# ---------- Benchmark 1: Direct to a backend (baseline) ----------
green "--- Baseline: direct to backend1 (no proxy) ---"
# Find the backend1 port (default 9001 mapped in docker-compose)
BACKEND_URL="http://localhost:9001"
if curl -sf -o /dev/null "$BACKEND_URL" 2>/dev/null; then
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$BACKEND_URL/"
else
    echo "(Skipped — backend1 not exposed on host. Expose port 9001 to compare.)"
fi

echo ""

# ---------- Benchmark 2: Through proxy (round-robin) ----------
green "--- Through proxy (round-robin) ---"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$PROXY/"

echo ""

# ---------- Benchmark 3: /health endpoint (minimal processing) ----------
green "--- /health endpoint (lightest path) ---"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$PROXY/health"

echo ""

# ---------- Benchmark 4: /stats endpoint (JSON serialization) ----------
green "--- /stats endpoint (JSON rendering under load) ---"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$PROXY/stats"

echo ""

green "=== Benchmark complete ==="
echo ""
echo "Tips:"
echo "  - Compare 'Requests/sec' between baseline and proxy to see overhead."
echo "  - Check 'Latency' distribution for P50/P99 under load."
echo "  - To test failover: docker compose stop backend2, then re-run."
echo "  - To test saturation: increase -c to exceed queue_capacity."
