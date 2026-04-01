#!/usr/bin/env bash
set -e
apt-get update -qq
apt-get install -y -qq wrk >/dev/null 2>&1

mkdir -p /tmp/docroot
echo "hello" > /tmp/docroot/index.html
cd /tmp/docroot

/app/server 9090 4 8 50000 &
SERVER_PID=$!
sleep 0.5

wrk -t2 -c10 -d5s http://127.0.0.1:9090/index.html

kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null || true
