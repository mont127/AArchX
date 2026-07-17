# tests/guest/Makefile
#
# Cross-compiles the x86_64 guest test programs into static LC_UNIXTHREAD
# Mach-O binaries that run BOTH natively under Rosetta on this dev machine
# (used only to generate the deterministic golden outputs) and under Ocerz
# (the real target). No Rosetta dependency is baked into the artifacts: they
# are ordinary static x86_64 executables.
#
# Each binary is linked from crt0.s (the _start stub), libmini.c (the six
# freestanding mem/str primitives clang may call) and the single test .c.
# The flag set is fixed by the project contract:
#   -arch x86_64        target ISA
#   -nostdlib -static   no libc, no dyld -> LC_UNIXTHREAD static image
#   -fno-stack-protector  no __stack_chk_* references (none would resolve)
#   -O2                 real optimization, so jump tables / SSE / rep string
#                       ops actually get emitted
#   -Wl,-e,_start       entry point is our stub, not libc start
# -fno-builtin is intentionally NOT passed: we WANT clang free to lower
# struct copies/zeroing to memcpy/memset (libmini.c satisfies them) and to
# lower __builtin_sqrt to inline sqrtsd. The build failing to link would
# reveal any unexpected libcall.
#
# Targets:
#   all    build every test into bin/
#   clean  remove bin/
#
# The .out goldens are checked in and produced out-of-band by running the
# native binaries (see ../run_guest_tests.sh and the generator notes); this
# Makefile does not run anything, so `make` works on machines without Rosetta.

CC := clang
ARCH := -arch x86_64
CFLAGS := $(ARCH) -nostdlib -static -fno-stack-protector -O2 -Wl,-e,_start
COMMON := crt0.s libmini.c

TESTS := hello exit42 args alu branches strings fib sse mmap_test fileio memstress flags longblock signal_test signal_jump0 rip_test stack_test popmem_test flags_live flags_carry fault_regs fault_resume callret_fault imul_flags

# bench is a THROUGHPUT benchmark, not a correctness test. It is built into
# benchbin/, NOT bin/, and that separation is load-bearing: run_diff_test.sh and
# run_guest_tests.sh both GLOB bin/*, so a bench binary in bin/ silently becomes
# a 19th differential case that (with the runner's fixed ARGS="one two three")
# selects the 200M-iteration 'br' workload and adds ~50s of interpreter time to
# every `make check`. It has no golden and is not a correctness test; keep it out
# of bin/. Built by `make bench`; run via tests/run_bench_diff.sh for the
# interp-vs-jit check on the benchmark itself.
#
# fibn is the same story and lives in the same place. It is the two-point
# regression variant of bench's `call` workload (fib(n) with n from argv), used
# to cancel the ~0.015s process-startup term that makes call's 0.039s wall clock
# unusable as a ratio. Same rule: benchbin/, never bin/.
BENCH := benchbin/bench benchbin/fibn
BINS := $(addprefix bin/,$(TESTS))

all: $(BINS)

bench: $(BENCH)

bin/%: %.c $(COMMON) gsys.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(COMMON) $<

benchbin/bench: bench.c $(COMMON) gsys.h
	@mkdir -p benchbin
	$(CC) $(CFLAGS) -o $@ $(COMMON) bench.c

benchbin/fibn: fibn.c $(COMMON) gsys.h
	@mkdir -p benchbin
	$(CC) $(CFLAGS) -o $@ $(COMMON) fibn.c

clean:
	rm -rf bin benchbin

.PHONY: all bench clean
