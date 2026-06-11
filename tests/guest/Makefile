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

TESTS := hello exit42 args alu branches strings fib sse mmap_test fileio memstress flags longblock signal_test signal_jump0
BINS := $(addprefix bin/,$(TESTS))

all: $(BINS)

bin/%: %.c $(COMMON) gsys.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(COMMON) $<

clean:
	rm -rf bin

.PHONY: all clean
