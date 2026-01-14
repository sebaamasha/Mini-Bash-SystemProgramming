CC=gcc
CFLAGS=-Wall -Wextra -O2

all: mini_bash

mini_bash: mini_bash.c
	$(CC) $(CFLAGS) -o mini_bash mini_bash.c

clean:
	rm -f mini_bash
