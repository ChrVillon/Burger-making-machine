CC = gcc
CFLAGS = -O2 -Wall -pthread

all: server control

server: burger_machine.c burger_machine.h
	$(CC) $(CFLAGS) burger_machine.c -o burger_server

control: control.c burger_machine.h
	$(CC) $(CFLAGS) control.c -o burger_control

clean:
	rm -f burger_server burger_control

.PHONY: all clean