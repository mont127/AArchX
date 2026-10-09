# Interpreter family (interp, interp_ext, interp_sse, x87, flags_live)

Owner: interp-agent. All five C units are Rust under `rust/src/ported/`:

| C file | Rust module | exports |
|---|---|---|
| flags_live.c | flags_live.rs | `ocerz_flags_defuse`, `ocerz_flags_defuse_nofault` |
| x87.c | x87.rs | `ocerz_x87_exec/push/fsw/reset/fxsave/fxrstor/to_f80/from_f80/f80_dbits` |
| interp_ext.c | interp_ext.rs | `ocerz_interp_ext` |
| interp_sse.c | interp_sse.rs | `ocerz_interp_sse` |
| interp.c | interp.rs | `ocerz_interp_step`, `ocerz_interp_exec`, `ocerz_unimpl`, `ocerz_cftrap(_on)`, `ocerz_atomic_*`, `ocerz_exc_read_cfstr` |

`rust/src/interp_common.rs` is the Rust form of `include/ocerz/interp_common.h`
(guest address translation, `ocerz_ea` with FS/GS and i386 wrap, GPR
read/write with high-byte and 32-bit zero-extension, operand and 128-bit
operand access, mode-aware push/pop). The header stays for the C callers
(jit, vm, abi...).

## Gotchas

- **Float ops are speculated.** LLVM treats float arithmetic as side-effect
  free, so a `match` over `a + b`, `a - b`, `a * b`, `a / b` inside an
  FPSR bracket was compiled as "compute all four, select one", raising OE/PE
  from operations the guest never ran (diff32 x87 caught 10 cases). The
  bracketed arithmetic is inline `fadd/fsub/fmul/fdiv`; every other bracketed
  operation is a single op per arm with `black_box` on its inputs/outputs.
  Any new FPSR-observing code must keep exactly one FP op per bracket.
- `#pragma STDC FENV_ACCESS ON` has no Rust equivalent: FPCR/FPSR are read
  and written with `mrs`/`msr` asm, and values that must be computed under
  the guest rounding mode pass through `black_box` so they are not
  constant-folded.
- SSE conversions that honour MXCSR rounding call `lrint/lrintf/llrint`
  (libc), truncating ones use `f32::trunc`/`f64::trunc` (libc crate has no
  `trunc`).
- CMPXCHG16B uses `caspal` inline asm (arm64 LSE) like the C's
  `__atomic_compare_exchange` on `__int128`; misaligned atomics use the same
  256 striped spin locks as the C.
- Stale `src/<module>.o` from before a port gets linked by
  tests/run_diff32.sh and duplicates symbols; the Makefile now deletes them
  (0674cf5), but delete by hand on older trees.
- Run unit bins as the gate does, with `OCERZ_NO_ARM_EXEC=1`. Without it
  test_jit32 churn-blacklists page 0x400000 after three retires and every
  later case "did not reach DONE"; the pristine C tree fails the same way.

## Known baseline failures on these VMs

- `dtest_jcc_gap_low-{jit,no-jit}`: golden says `faults 59`, this VM gives
  60 with the pristine C tree too (environmental).
- test_apidb's two macOS-27 fixture checks.

## Perf

Paired runs, alternating C (`~/AArchX-c`, 97a9247) and Rust on the same
idle VM, best of 3, `./ocerz -no-jit tests/guest/benchbin/<bin>`, with all
five units in Rust (0f1cab8):

| benchmark | C (s) | Rust (s) | Rust vs C |
|---|---|---|---|
| xbench -no-jit | 18.66 | 15.40 | -17.5% |
| fibn -no-jit | 4.13 | 3.80 | -7.9% |
| memn -no-jit | 9.33 | 8.14 | -12.8% |
| bench -no-jit | 4.02 | 3.72 | -7.4% |
| guest suite `--no-jit` wall (134 tests, best of 2) | 4.83 | 4.56 | -5.6% |

Before interp.c moved over (flags_live, x87, ext, sse in Rust, core in C),
xbench -no-jit was 18.83 C vs 17.78 Rust. Most of the remaining gain comes
from LTO inlining operand fetch/store and the flag helpers into the Rust
dispatch switch. JIT-mode times are unchanged, since the interpreter is off
the hot path there.
