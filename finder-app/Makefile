CC = $(CROSS_COMPILE)gcc
CFLAGS = -g -Wall
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)
TARGET = writer

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


.PHONY: clean

clean:
	rm -f $(OBJS) $(TARGET)