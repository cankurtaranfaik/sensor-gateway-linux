CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -std=gnu11

telemd: telemd.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f telemd

.PHONY: clean
