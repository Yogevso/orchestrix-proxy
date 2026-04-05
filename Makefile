# ===========================================================================
#  Orchestrix Proxy — Makefile
# ===========================================================================

CC       = gcc
CFLAGS   = -Wall -Wextra -pthread -std=c11 -D_GNU_SOURCE
LDFLAGS  = -pthread

SRCDIR   = src
TARGET   = proxy

SRCS     = $(SRCDIR)/main.c          \
           $(SRCDIR)/server_core.c    \
           $(SRCDIR)/threadpool.c     \
           $(SRCDIR)/config.c         \
           $(SRCDIR)/http_parse.c     \
           $(SRCDIR)/http_io.c        \
           $(SRCDIR)/proxy_handler.c  \
           $(SRCDIR)/static_handler.c \
           $(SRCDIR)/upstreams.c      \
           $(SRCDIR)/circuit_breaker.c\
           $(SRCDIR)/healthcheck.c    \
           $(SRCDIR)/rate_limit.c     \
           $(SRCDIR)/admin_handler.c  \
           $(SRCDIR)/metrics.c        \
           $(SRCDIR)/logger.c         \
           $(SRCDIR)/priority.c       \
           $(SRCDIR)/chaos.c

OBJS     = $(SRCS:.c=.o)

# ---------------------------------------------------------------------------
#  Targets
# ---------------------------------------------------------------------------

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

# ---------------------------------------------------------------------------
#  Build variants
# ---------------------------------------------------------------------------

debug: CFLAGS += -DDEBUG -g -O0
debug: clean all

asan: CFLAGS  += -fsanitize=address -g -O1
asan: LDFLAGS += -fsanitize=address
asan: clean all

tsan: CFLAGS  += -fsanitize=thread -g -O1
tsan: LDFLAGS += -fsanitize=thread
tsan: clean all

# ---------------------------------------------------------------------------
#  Run helpers
# ---------------------------------------------------------------------------

run: all
	./$(TARGET) proxy.conf

run-static: all
	./$(TARGET) --static 8080 4 16

# ---------------------------------------------------------------------------
#  Legacy server (original webserver code, kept for reference)
# ---------------------------------------------------------------------------

LEGACY_SRCS  = server.c threadpool.c
LEGACY_OBJS  = $(LEGACY_SRCS:.c=.o)

legacy: $(LEGACY_OBJS)
	$(CC) $(LDFLAGS) -o server $^

server.o: server.c threadpool.h
	$(CC) $(CFLAGS) -c -o $@ $<

threadpool.o: threadpool.c threadpool.h
	$(CC) $(CFLAGS) -c -o $@ $<

run-legacy: legacy
	./server 8080 4 8 100

# ---------------------------------------------------------------------------
#  Housekeeping
# ---------------------------------------------------------------------------

clean:
	rm -f $(OBJS) $(TARGET) $(LEGACY_OBJS) server

test: all
	bash tests/test_proxy.sh

.PHONY: all clean debug asan tsan run run-static legacy run-legacy test
