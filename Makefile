# Build ocerz, the guest tests and the unit tests. `make check` runs the lot.
#
# `unit` runs the harnesses with OCERZ_NO_ARM_EXEC=1 because they drive
# ocerz_jit_step without a CPU run loop, so the self-modifying-code write trap
# has no fault recovery to land on when a harness's data window shares a page
# with its code; SMC is covered by the guest smc and dynamic smc_io tests
# instead.
#
# `diff32` is the 32-bit half of the differential. run_diff_test.sh cannot
# cover i386 -- there is no i386 Mach-O to load -- so this one builds the
# sequences itself and runs each under the interpreter and under the JIT.
# Unlike i386diff it IS a pass/fail gate and IS part of `check`, and it passes
# --jit-required so a JIT that stops translating 32-bit blocks fails the gate
# rather than quietly degrading into a second interpreter run that passes.
#
# `i386diff` is 32-bit decode conformance against capstone CS_MODE_32. It
# reports a coverage percentage and is deliberately NOT part of `check`,
# because i386 support is being built up in stages and the number is a progress
# measure, not a pass/fail. Pass --min-coverage N to turn it into a gate.
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
	@for t in $(UNIT_BINS); do echo "== $$t"; OCERZ_NO_ARM_EXEC=1 $$t || exit 1; done

guest:
	$(MAKE) -C tests/guest

check: ocerz unit guest
	bash tests/run_guest_tests.sh --no-jit
	bash tests/run_guest_tests.sh
	bash tests/run_diff_test.sh
	bash tests/run_diff32.sh .
	bash tests/run_dynamic_tests.sh

diff32:
	bash tests/run_diff32.sh .

i386diff:
	bash tools/i386diff.sh .

clean:
	rm -f $(OBJS) $(DEPS) ocerz
	rm -rf tests/unit/bin
	$(MAKE) -C tests/guest clean

-include $(DEPS)

.PHONY: unit guest check clean i386diff diff32
