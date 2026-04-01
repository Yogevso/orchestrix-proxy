#!/usr/bin/env bash
set -e

mkdir -p /tmp/docroot
echo "hello" > /tmp/docroot/index.html
cd /tmp/docroot

/app/server 9090 4 8 100 &
PID=$!
sleep 0.5

# Hit it a few times
for i in $(seq 1 10); do
    curl -s http://127.0.0.1:9090/index.html > /dev/null
done
# One 404
curl -s http://127.0.0.1:9090/nope > /dev/null || true

echo "=== /stats output ==="
curl -s http://127.0.0.1:9090/stats
echo ""

kill $PID 2>/dev/null
wait $PID 2>/dev/null || true
