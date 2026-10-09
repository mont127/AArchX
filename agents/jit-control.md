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
- Fixed-size static, struct-field, and local arrays use unchecked indexing to
  retain C's unchecked semantics on hot paths. The index expressions and their
  casts/masks are preserved; `ps_shapes` is sized from `OCERZ_OP_COUNT`.

## Verification

On the latest unchecked-index candidate, `make -j12 ocerz` succeeded and the
emission audit against `~/AArchX-jitc` returned:

```text
MATCH: 215295 blocks, 145523525 arm64 words (offset + low, seed=1; relocation payloads masked)
```

All eight requested unit binaries passed with `OCERZ_NO_ARM_EXEC=1`. The fast
gate passed with no new failures: 3 known unit failures, 134/134 guest tests in
both modes, 100/100 differential cases, and 107,410 translated diff32 blocks.

The latest full gate returned `GATE: FAIL (new failures vs baseline)` because
`dthread_signal-jit` failed once (`out='FAIL'`, exit 1). The task owner
identified this as a known flaky case; it had previously passed isolated
reruns on both the Rust candidate and C reference. It is not currently in the
gate's expected failure list, so the raw gate result remains a failure. The
dynamic phase had 279 passes and 8 failures total (7 baseline failures plus
this case). Native tests had 86 passes and the known `sys_proc`
host-compatibility failure.
Guest-library, native framework, and native format tests passed; native C++
and Swift tests were skipped because their guest fixtures were not built.
The earlier `datomic_counter-no-jit` timeout (exit 124) at `9595b42` was also
accepted as a known tip flake; it did not recur in this full gate.

Full-gate phase logs are preserved at
`/Users/devin/notes/gate-full/full-final-mut-access/`; fast-gate phase logs are
at `/Users/devin/notes/gate-full/fast-final-mut-access/`.

## Performance

Paired alternating runs used `~/AArchX-jitc` at `16a7c2d` as the C reference
and the latest unchecked-index candidate. The i386 harness uses the default
offset corpus and `--jit-required`; the translation counter was enabled by a
temporary constructor, without changing either production tree. There were
five alternating pairs. Each dynamic result is the median per-process time
from six alternating pairs of 30 fresh processes per tree and fixture, with
`OCERZ_TCACHE=off`.

| Measurement | C reference | Rust candidate | Change |
| --- | ---: | ---: | ---: |
| i386 `--jit-required` harness wall | 22.642 s | 23.564 s | +4.1% |
| Time inside `translate` | 857.519 ms | 882.472 ms | +2.9% |
| `tcache_work` | 38.712 ms | 38.558 ms | -0.4% |
| `avx_fp` | 38.486 ms | 38.221 ms | -0.7% |
| `low_hoist` | 38.182 ms | 37.419 ms | -2.0% |

Temporary probe, runner, and raw samples are in
`~/notes/jit-control-bench/`; the final raw samples are in
`results.json` and the previous samples were snapshotted as
`results-before-final-mut-access.json`. None are part of the repository.
