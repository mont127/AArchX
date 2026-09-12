# Cross-compiles the x86_64 guest test programs into static LC_UNIXTHREAD
# Mach-O binaries that run BOTH natively under Rosetta on this dev machine
# (used to generate deterministic goldens and as the benchmark control) and
# under Ocerz (the real target). No Rosetta dependency is baked into the
# artifacts: they are ordinary static x86_64 executables. This Makefile does
# not run anything, so `make` works on machines without Rosetta; the .out
# goldens are checked in and produced out-of-band.
#
# Each binary is linked from crt0.s (the _start stub), libmini.c (the six
# freestanding mem/str primitives clang may call) and the single test .c. The
# flag set is fixed by the project contract:
#   -arch x86_64          target ISA
#   -nostdlib -static     no libc, no dyld -> LC_UNIXTHREAD static image
#   -fno-stack-protector  no __stack_chk_* references (none would resolve)
#   -O2                   real optimization, so jump tables / SSE / rep string
#                         ops actually get emitted
#   -Wl,-e,_start         entry point is our stub, not libc start
# -fno-builtin is intentionally NOT passed: we WANT clang free to lower struct
# copies and zeroing to memcpy/memset (libmini.c satisfies them) and to lower
# __builtin_sqrt to inline sqrtsd. The build failing to link would reveal any
# unexpected libcall.
#
# Targets: `all` builds every correctness test into bin/, `bench` builds bench,
# fibn and memn into benchbin/, `clean` removes both.
#
# What lives in which directory is load-bearing. run_diff_test.sh and
# run_guest_tests.sh both GLOB bin/*, so a bench binary placed there silently
# becomes an extra differential case that -- with the runner's fixed
# ARGS="one two three" -- selects the 200M-iteration 'br' workload and adds
# ~50 s of interpreter time to every `make check`. bench is a THROUGHPUT
# benchmark with no golden, so it belongs in benchbin/, never bin/; it is run
# via tests/run_bench_diff.sh for the interp-vs-jit check and
# tests/run_bench_compare.sh for Rosetta performance. fibn and memn are the
# same story: the two-point regression variants of bench's `call` and `mem`
# workloads, used to cancel process startup and fixed-work terms that make the
# short raw wall clocks unusable as throughput ratios. Same rule.
#
# lostwake.c is deliberately NOT in TESTS either. It is a manual reproducer for
# the entry-edge signal drain, timing-dependent by construction (a forked child
# must deliver SIGUSR1 while the parent spins), and its 300M-iteration spin
# takes minutes under the interpreter. Build and run it by hand:
#   clang $(CFLAGS) -o /tmp/lostwake crt0.s libmini.c lostwake.c
#   ../../ocerz /tmp/lostwake     # must print "start" then "woke rc=1"
CC := clang
ARCH := -arch x86_64
CFLAGS := $(ARCH) -nostdlib -static -fno-stack-protector -O2 -Wl,-e,_start
COMMON := crt0.s libmini.c

TESTS := hello exit42 args alu branches strings fib sse mmap_test fileio memstress flags flags_deferred longblock signal_test signal_jump0 rip_test stack_test popmem_test flags_live flags_carry fault_regs fault_resume fault_chain callret_fault imul_flags interrupt_test ras_stress chaincall link_spin fault_link fault_xlive fork_order fork_exec shared_order unaligned_order jit_ops jit_div jit_sse jit_cvt jit_sse2 nan_rules jit_mul jit_scan jit_rmw jit_bt jit_movq jit_narrow jit_misc2 jit_misc3 jit_align jit_align2 jit_nanabs jit_divtrap sigmask_test int3_test sse42_test rsp_ops fcntl_locks cvt_nan_batch fp_loop_nan fpb_defer_nan cvt_merge_nan machdep_rdx ret_flags mov_sreg far_sel_bits filemap_private bigenv stack_align jcc_chain_rec atomic_unaligned lddqu jcc_gap_fuse smc pop_rsp_mem avx_basic movnt_stores punpck_qdq movhlpd side_exit_l0 xorpd_movhpd xf_epilogue

BENCH := benchbin/bench benchbin/fibn benchbin/memn benchbin/xbench benchbin/xbench_dyn
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

benchbin/memn: memn.c $(COMMON) gsys.h
	@mkdir -p benchbin
	$(CC) $(CFLAGS) -o $@ $(COMMON) memn.c

benchbin/xbench: xbench.c $(COMMON) gsys.h
	@mkdir -p benchbin
	$(CC) $(CFLAGS) -o $@ $(COMMON) xbench.c

benchbin/xbench_dyn: xbench.c gsys.h
	@mkdir -p benchbin
	$(CC) $(ARCH) -O2 -fno-stack-protector -o $@ xbench.c

clean:
	rm -rf bin benchbin

.PHONY: all bench clean
