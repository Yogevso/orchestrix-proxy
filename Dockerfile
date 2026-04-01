FROM gcc:latest

WORKDIR /app
COPY server.c threadpool.c threadpool.h Makefile ./
COPY tests/ tests/

RUN make clean && make

# Smoke-test: start server, hit it, verify response
RUN bash -c '\
    mkdir -p /tmp/docroot && \
    echo "hello world" > /tmp/docroot/index.html && \
    cd /tmp/docroot && \
    /app/server 9090 4 8 5 & \
    SERVER_PID=$! && \
    sleep 0.5 && \
    RESP=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9090/index.html) && \
    kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null; \
    echo "HTTP status: $RESP" && \
    [ "$RESP" = "200" ] && echo "SMOKE TEST PASSED" || { echo "SMOKE TEST FAILED"; exit 1; }'
