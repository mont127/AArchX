# Build ocerz, the guest tests and the unit tests. `make check` runs the lot.

CC := clang
ARCHFLAGS := -arch arm64
CFLAGS := $(ARCHFLAGS) -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Iinclude -MMD -MP
LDFLAGS := $(ARCHFLAGS)

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)
CORE_OBJS := $(filter-out src/main.o,$(OBJS))

UNIT_SRCS := $(wildcard tests/unit/*.c)
UNIT_BINS := $(UNIT_SRCS:tests/unit/%.c=tests/unit/bin/%)

ocerz: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

tests/unit/bin/%: tests/unit/%.c $(CORE_OBJS)
	@mkdir -p tests/unit/bin
	$(CC) $(CFLAGS) -o $@ $< $(CORE_OBJS)

unit: $(UNIT_BINS)
	@for t in $(UNIT_BINS); do echo "== $$t"; $$t || exit 1; done

guest:
	$(MAKE) -C tests/guest

check: ocerz unit guest
	bash tests/run_guest_tests.sh --no-jit
	bash tests/run_guest_tests.sh
	bash tests/run_diff_test.sh
	bash tests/run_dynamic_tests.sh

clean:
	rm -f $(OBJS) $(DEPS) ocerz
	rm -rf tests/unit/bin
	$(MAKE) -C tests/guest clean

-include $(DEPS)

.PHONY: unit guest check clean
