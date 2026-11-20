CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=gnu11
LDFLAGS ?=
LDLIBS  := -pthread

.PHONY: all clean test

all: vm

vm: vm.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: vm
	@sh ./run_tests.sh

clean:
	rm -f vm *.o