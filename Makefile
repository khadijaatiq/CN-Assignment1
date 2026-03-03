# Makefile – HTTP Proxy (CS3001 Assignment #1)

CC      = gcc
CFLAGS  = -Wall -Wextra -g
TARGET  = proxy
SRC     = proxy.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
