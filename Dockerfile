FROM gcc:latest

WORKDIR /app

# Copy source tree
COPY Makefile proxy.conf ./
COPY src/ src/

# Keep legacy source for the "legacy" build target
COPY server.c threadpool.c threadpool.h ./

COPY tests/ tests/

RUN make clean && make

# Smoke-test: start proxy in static mode, verify it responds
RUN bash -c '\
    mkdir -p /tmp/docroot && \
    echo "hello world" > /tmp/docroot/index.html && \
    cd /tmp/docroot && \
    /app/proxy --static 9090 4 16 & \
    PID=$! && \
    sleep 0.5 && \
    RESP=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9090/index.html) && \
    kill $PID 2>/dev/null; wait $PID 2>/dev/null; \
    echo "HTTP status: $RESP" && \
    [ "$RESP" = "200" ] && echo "SMOKE TEST PASSED" || { echo "SMOKE TEST FAILED"; exit 1; }'

EXPOSE 8080 8081

ENTRYPOINT ["/app/proxy"]
CMD ["proxy.conf"]
