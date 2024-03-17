CC = gcc
LDLIBS = -lwayland-client -lxkbcommon -lrt
CFLAGS = -Wall -Wextra -std=c17 -Wpedantic
SRCS = snake.c xdg-shell-protocol.c
OBJS = $(SRCS:.c=.o)
TARGET = snake

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TARGET)

