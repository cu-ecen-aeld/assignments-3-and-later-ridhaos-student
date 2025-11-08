CC = $(CROSS_COMPILE)gcc
CFLAGS = -g -Wall
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)
TARGET = aesdsocket

all : $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


.PHONY: clean

clean:
	rm -f $(OBJS) $(TARGET)