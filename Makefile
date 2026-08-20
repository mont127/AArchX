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
	bash tests/run_diff32.sh .
	bash tests/run_dynamic_tests.sh

# The 32-bit half of the differential.  run_diff_test.sh cannot cover i386 --
# there is no i386 Mach-O to load -- so this one builds the sequences itself and
# runs each under the interpreter and under the JIT.  Unlike i386diff it IS a
# pass/fail gate and IS part of `check`.  With stage 9 landed the JIT really
# compiles the 32-bit side, so run_diff32.sh passes --jit-required and a JIT
# that stops translating 32-bit blocks fails the gate rather than passing it.
diff32:
	bash tests/run_diff32.sh .

# 32-bit decode conformance vs capstone CS_MODE_32.  Reports a coverage
# percentage; deliberately NOT part of `check`, because i386 support is being
# built up in stages and the number is a progress measure, not a pass/fail.
# Pass --min-coverage N to turn it into a gate.  See tools/i386diff.sh.
i386diff:
	bash tools/i386diff.sh .

clean:
	rm -f $(OBJS) $(DEPS) ocerz
	rm -rf tests/unit/bin
	$(MAKE) -C tests/guest clean

-include $(DEPS)

.PHONY: unit guest check clean i386diff diff32
