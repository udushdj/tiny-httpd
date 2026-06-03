CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS =
TARGET  = tiny-httpd

SRCS    = src/main.c
OBJS    = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) src/*.o

run: $(TARGET)
	./$(TARGET) 8080

debug: CFLAGS += -DDEBUG -O0
debug: $(TARGET)

.PHONY: clean run debug
