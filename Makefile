CC      = gcc
CFLAGS  = -Wall -Wextra -pthread
LDFLAGS = -pthread

TARGET  = server
SRCS    = server.c threadpool.c
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c threadpool.h
	$(CC) $(CFLAGS) -c -o $@ $<

debug: CFLAGS += -DDEBUG -g -O0
debug: clean $(TARGET)

asan: CFLAGS += -fsanitize=address -g -O1
asan: LDFLAGS += -fsanitize=address
asan: clean $(TARGET)

tsan: CFLAGS += -fsanitize=thread -g -O1
tsan: LDFLAGS += -fsanitize=thread
tsan: clean $(TARGET)

run: $(TARGET)
	./$(TARGET) 8080 4 8 100

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean debug asan tsan run
