CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=gnu11
LDFLAGS ?=
LDLIBS  := -pthread

.PHONY: all clean

all: vm

vm: vm.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f vm *.o
