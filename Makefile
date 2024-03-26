CC = gcc
LDLIBS = -lwayland-client -lxkbcommon -lrt
CFLAGS = -Wall -std=c17 -Wpedantic
SRCS = main.c snake.c font.c glue_code/xdg-shell-protocol.c
OBJS = $(SRCS:.c=.o)
TARGET = snake

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TARGET)

