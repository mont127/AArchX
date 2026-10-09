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
- `jit_perfstat_one` keeps C's two separate sequentially consistent atomic
  operations for `ps_ops`: a fetch-add followed by a fresh load.

## Verification

On the final source after the perfstat atomic-order fix, `make -j12 ocerz`
succeeded and the emission audit against `~/AArchX-jitc` returned:

```text
MATCH: 215295 blocks, 145523525 arm64 words (offset + low, seed=1; relocation payloads masked)
```

- Full x64/all-corpus audit at candidate tip `f6e579e611beb793a12de134f7ec011268fef158`: `MATCH: 427961 blocks, 199368518 arm64 words (corpus=all; relocation payloads masked)`.

All eight requested unit binaries passed with `OCERZ_NO_ARM_EXEC=1`. After the
perfstat atomic-order fix, the build, audit, all eight binaries, and fast gate
were rerun successfully. The fast gate passed with no new failures: 3 known
unit failures, 134/134 guest tests in both modes, 100/100 differential cases,
and 107,410 translated diff32 blocks.

The post-rebase full gate passed with no new failures. Dynamic tests reported
280 passes and 7 known failures; native tests reported 86 passes and 1 known
`sys_proc` host-compatibility failure. Guest-library, native framework, and
native format tests passed; native C++ and Swift tests were skipped because
their guest fixtures were not built. The rebase added the
`dtest_jcc_gap_low-{jit,no-jit}` cases to the expected dynamic failures. This
full gate preceded the narrow `OCERZ_PERFSTAT` atomic-order correction;
afterward, the native-framework phase was rerun independently and passed.

A pre-rebase full-gate run briefly reported `dthread_signal-jit` as a new
failure. It had previously passed isolated reruns on both the Rust candidate
and C reference, and did not recur in the post-rebase full gate. The earlier
`datomic_counter-no-jit` timeout (exit 124) at `9595b42` was also accepted as a
known tip flake and did not recur.

Full-gate phase logs are preserved at
`/Users/devin/notes/gate-full/full-post-final-tip/`; fast-gate phase logs for
the final source are at `/Users/devin/notes/gate-full/fast-perfstat-fix/`.
The post-fix native-framework log is
`/Users/devin/notes/gate-full/native-framework-perfstat-fix.log`.

## Performance

Paired alternating runs used `~/AArchX-jitc` at `16a7c2d` as the C reference
and the latest rebased candidate. The i386 harness uses the default offset
corpus and `--jit-required`; the translation counter was enabled by a temporary
constructor, without changing either production tree. There were five
alternating pairs. Each dynamic result is the median per-process time from six
alternating pairs of 30 fresh processes per tree and fixture, with
`OCERZ_TCACHE=off`. `OCERZ_PERFSTAT` was unset during measurement, so the later
perfstat-only atomic-order correction does not affect these timings.

| Measurement | C reference | Rust candidate | Change |
| --- | ---: | ---: | ---: |
| i386 `--jit-required` harness wall | 22.640 s | 23.096 s | +2.0% |
| Time inside `translate` | 855.155 ms | 816.239 ms | -4.6% |
| `tcache_work` | 39.443 ms | 39.477 ms | +0.1% |
| `avx_fp` | 39.158 ms | 38.524 ms | -1.6% |
| `low_hoist` | 36.916 ms | 36.806 ms | -0.3% |

These timings compare the rebased tree against the C reference; the rebase also
landed other Rust ports, including the decoder and JIT flags. The -4.6%
translation-time change is an end-to-end result and is not isolated to
`jit_control.c`.

Temporary probe, runner, and raw samples are in
`~/notes/jit-control-bench/`; the final raw samples are in
`results.json` and the pre-rebase samples were snapshotted as
`results-before-final-tip.json`. None are part of the repository.
