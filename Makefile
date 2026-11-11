CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -std=gnu11
HARDEN   ?= -D_FORTIFY_SOURCE=2 -fstack-protector-strong
CORE     := sha256.c sim.c
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
SRCS     := $(CORE) cli.c gui.c redwood.h sha256.h

all: redwood redwood-cli

redwood: $(CORE) gui.c redwood.h sha256.h
	$(CC) $(CFLAGS) $(HARDEN) $(shell pkg-config --cflags gtk+-3.0) -o $@ $(CORE) gui.c $(shell pkg-config --libs gtk+-3.0) -lm

redwood-cli: $(CORE) cli.c redwood.h sha256.h
	$(CC) $(CFLAGS) $(HARDEN) -o $@ $(CORE) cli.c -lm

# Headless build with AddressSanitizer and UndefinedBehaviorSanitizer.
redwood-cli-san: $(CORE) cli.c redwood.h sha256.h
	$(CC) -Wall -Wextra -std=gnu11 $(SAN) -o $@ $(CORE) cli.c -lm

# Regression tests (no GUI dependencies).
test: redwood-cli
	bash ./tests/run_tests.sh ./redwood-cli

# Same tests under the sanitizers.
check: redwood-cli-san
	bash ./tests/run_tests.sh ./redwood-cli-san

format:
	clang-format -i $(SRCS)

format-check:
	clang-format --dry-run --Werror $(SRCS)

clean:
	rm -f redwood redwood-cli redwood-cli-san

.PHONY: all test check format format-check clean
