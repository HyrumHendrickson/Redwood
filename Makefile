CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
CORE    := sha256.c sim.c

all: redwood redwood-cli

redwood: $(CORE) gui.c redwood.h sha256.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gtk+-3.0) -o $@ $(CORE) gui.c $(shell pkg-config --libs gtk+-3.0) -lm

redwood-cli: $(CORE) cli.c redwood.h sha256.h
	$(CC) $(CFLAGS) -o $@ $(CORE) cli.c -lm

clean:
	rm -f redwood redwood-cli

.PHONY: all clean
