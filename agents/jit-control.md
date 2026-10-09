# `jit_control.c` Rust port

`src/jit_control.c` is replaced by `rust/src/ported/jit_control.rs`, preserving
the exported C ABI and emitted code. The port uses `crate::jit_internal` for
shared header helpers rather than keeping private copies. Its `ocerz_ld` helper
is the `mem.h` static inline added to `rust/src/jit_internal.rs`; it is needed
by this module and is committed separately.

## Caller-address shim

`src/jit_control_shim.c` contains only `ocerz_jit_exec_one` and
`ocerz_jit_exec_one_at`. Both definitions are explicitly `noinline` and pass
`__builtin_return_address(0)` to `jit_exec_one_parked`. Fault and park paths
identify their caller through that address, which Rust cannot capture at the
equivalent call site.

## Notes and gotchas

- C `ENV_ON` uses become per-callsite cached `libc::getenv` checks. Keeping a
  separate cache at each site preserves the C macro's lazy, one-time lookup
  semantics.
- Every C-bound string is a `\0`-terminated byte literal, including format
  strings and environment-variable names.
- The C-only `jit_trace_one` and `jit_perfstat_one` helpers used
  `noinline`, `cold`, and `preserve_most`. Rust keeps `#[inline(never)]` and
  `#[cold]`; `preserve_most` has no equivalent used by this Rust port.
- The C shim must remain non-inlined: moving either caller-address capture
  into Rust or allowing the wrapper to inline would lose the caller address
  used by fault and park handling.
- The shared `ocerz_ld` uses acquire atomic loads for naturally aligned 1/2/4/8
  byte accesses and a byte-copy plus acquire fence for other cases.

## Verification

At candidate commit `9595b42`, `make -j12 ocerz` succeeded and the emission
audit against `~/AArchX-jitc` returned:

```text
MATCH: 215295 blocks, 145523525 arm64 words (offset + low, seed=1; relocation payloads masked)
```

The eight requested unit binaries passed with `OCERZ_NO_ARM_EXEC=1`. The fast
gate passed with no new failures: 3 known unit failures, 134/134 guest tests in
both modes, 100/100 differential cases, and 107,410 translated diff32 blocks.

The full gate at `9595b42` is accepted as passing for this port. The raw runner
reported `GATE: FAIL` only because `datomic_counter-no-jit` timed out (exit 124,
expected `OK`) in the interpreter-only run. This is a known tip flake, not a
port regression; the same timeout is recorded in `agents/status.md:48`,
`agents/dyld.md`, and `agents/native2.md`. The dynamic phase reported 279
passed/8 failed, with that timeout as its only new failure. Native tests
reported 86 passed/1 known failure; guest-library tests passed, and native
framework and format tests passed. Native C++ and Swift phases were skipped.

Full-gate output and phase logs are preserved at
`~/notes/gate-full/full-9595b42/gate.stdout.log` and
`~/notes/gate-full/full-9595b42/logs/`. Fast-gate phase logs are at
`~/notes/gate-full/fast-9595b42/`.

## Performance

Paired alternating runs used `~/AArchX-jitc` at `16a7c2d` as the C reference
and the candidate at `9595b42`. The i386 harness uses the default offset corpus
and `--jit-required`; the translation counter was enabled by a temporary
constructor, without changing either production tree. There were five pairs.
Each dynamic result is the median per-process time from six alternating pairs
of 30 fresh processes per tree and fixture, with `OCERZ_TCACHE=off`.

| Measurement | C reference | Rust candidate | Change |
| --- | ---: | ---: | ---: |
| i386 `--jit-required` harness wall | 24.052 s | 24.791 s | +3.1% |
| Time inside `translate` | 958.100 ms | 987.461 ms | +3.1% |
| `tcache_work` | 40.501 ms | 40.412 ms | -0.2% |
| `avx_fp` | 38.459 ms | 38.871 ms | +1.1% |
| `low_hoist` | 38.083 ms | 37.788 ms | -0.8% |

Temporary probe, runner, and raw samples are in
`~/notes/jit-control-bench/`; none are part of the repository.
