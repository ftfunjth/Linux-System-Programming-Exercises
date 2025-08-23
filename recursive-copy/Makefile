GCC     ?= gcc
CFLAGS  ?= -g -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?=

recursive_copy: recursive_copy.o
	$(GCC) $(LDFLAGS) -o $@ $^

recursive_copy.o: recursive_copy.c
	$(GCC) $(CFLAGS) -c -o $@ $^

.PHONY: clean
clean:
	rm -f recursive_copy.o recursive_copy
