#!/usr/bin/env bash
###############################################################################
# demo-traffic.sh — Generate a realistic traffic pattern against the proxy.
#
# Phases:
#   1. Steady state    — moderate load for 10 s
#   2. Traffic spike   — burst of concurrent requests for 5 s
#   3. Backend failure  — kill backend2, keep sending for 10 s
#   4. Recovery         — restart backend2, send for 10 s
#
# Requirements: curl, docker compose already running
#
# Usage:
#   bash scripts/demo-traffic.sh [proxy-url]
#   Default proxy-url: http://localhost:8080
###############################################################################
set -euo pipefail

PROXY="${1:-http://localhost:8080}"
CONCURRENCY=8

green()  { printf '\033[1;32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[1;33m%s\033[0m\n' "$*"; }
red()    { printf '\033[1;31m%s\033[0m\n' "$*"; }

send_requests() {
    local duration=$1 rps=$2 label=$3
    green "=== Phase: $label  (${duration}s @ ~${rps} req/s) ==="
    local end=$((SECONDS + duration))
    local count=0
    while [ $SECONDS -lt $end ]; do
        for _ in $(seq 1 "$rps"); do
            curl -s -o /dev/null -w '' "$PROXY/" &
        done
        wait
        count=$((count + rps))
        sleep 1
    done
    echo "  Sent ~$count requests"
}

# ---------- Phase 1: Steady state ----------
send_requests 10 5 "Steady State"

# ---------- Phase 2: Traffic spike ----------
send_requests 5 30 "Traffic Spike"

# ---------- Phase 3: Backend failure ----------
yellow ">>> Stopping backend2 to simulate failure..."
docker compose stop backend2 2>/dev/null || docker-compose stop backend2 2>/dev/null
send_requests 10 5 "Backend Down"

# ---------- Phase 4: Recovery ----------
yellow ">>> Restarting backend2..."
docker compose start backend2 2>/dev/null || docker-compose start backend2 2>/dev/null
send_requests 10 5 "Recovery"

# ---------- Summary ----------
green "=== Demo complete ==="
echo ""
echo "Check live metrics:"
echo "  curl $PROXY/stats   (JSON)"
echo "  curl $PROXY/metrics (Prometheus)"
echo "  Grafana: http://localhost:3000"
