# jit_flags: Rust port notes

`src/jit_flags.c` (2778 lines) is replaced by `rust/src/ported/jit_flags.rs`.
It ports the shared condition-code emitters, compare/test fusion, flag
materialization and deferred-flags paths, conditional branches, if-conversion,
and XLIVE analysis.

## Shape

- C exports retain their `jit_internal.h` ABI. Shared state is defined with
  bindgen types and C initializers; private state and each `getenv` cache keep
  their C scope and per-callsite semantics.
- Header inline helpers use `crate::jit_internal`; external pieces use
  `crate::ffi`.
- `src/jit_flags_shim.c` preserves the `sigsetjmp` recovery boundary. Rust
  decode callbacks use plain stack state and raw pointers because C may
  `siglongjmp` over their frames; volatile progress fields are read and
  written with volatile operations.

## Audit corrections

The function-by-function comparison against `src/jit_flags.c` found and fixed:

- `xlive_decode_entry_d`, C line 428: restored the missing XLIVE diagnostic
  after the def-use loop and before memo storage.
- `emit_cc_predicate_ex`, C lines 1044–1056: moved the ADD-specialized branch
  after shift handling and before SSE handling, matching C's branch order.
- `emit_ifconv_diamond`, C lines 2404–2418: use `emit_incdec_jcc_arm` for the
  exit edge and preserve C's latch reload, flag deferral and stop-target RIP
  sequence.
- `emit_jcc`, C lines 2441–2445: use `g_no_chain` for `self_loop`, and set
  `g_cc_want_cbz` to `two_way` only (not `two_way | self_loop`).
- Flip lock-acquisition callsites, C lines 2654, 2730 and 2759: corrected
  diagnostic site arguments to those exact C line values.

No other deviations were found in the full function-by-function audit.

## Verification

- `make -j12 ocerz`: pass; the Rust port links with the C shim.
- Native fixtures `app_bundle`, `block_foundation`, `objc_classes`,
  `objc_view_render`, `sys_strings` and `sys_strings_inplace`: all pass in the
  native runner's modes.
- Requested unit binaries with `OCERZ_NO_ARM_EXEC=1`: all eight pass.
- `tools/jit_emit_audit.sh ~/AArchX-jitc`: MATCH, 215,295 blocks and
  145,523,525 arm64 words.
- `bash tools/rust_gate.sh --fast`: PASS, no new failures.
- `bash tools/rust_gate.sh`: PASS, no new failures vs baseline. Known baseline
  failures remain: three unit failures, nine dynamic failures and native
  `sys_proc`.

## Performance

Paired alternating measurements on this VM; the key results are recorded
inline below.

### Initial branch-wide comparison against `~/AArchX-jitc` at `16a7c2d`

This comparison predates the stack-scratch initialization fix and includes
the other Rust ports on the branch.

| Measurement | C reference | Candidate | Change |
| --- | ---: | ---: | ---: |
| i386 `--jit-required` diff32 wall time (median, 8 runs) | 21.337 s | 20.882 s | -2.1% |
| Time in `translate` (median, 8 runs) | 630.902 ms | 628.877 ms | -0.3% |
| `tcache_work` (median, 6 batches × 30 processes, tcache off) | 33.416 ms | 42.789 ms | +28.1% |
| `avx_fp` (same) | 32.817 ms | 42.021 ms | +28.1% |
| `low_hoist` (same) | 32.938 ms | 42.001 ms | +27.5% |

### Isolated C/Rust `jit_flags` comparison before the fix

Both binaries were built from this tree with the other Rust ports held
constant. A linked C `src/jit_flags.o` with the Rust module and shim moved
aside; B linked the Rust `jit_flags.rs` and C shim. Dynamic fixtures use six
alternating batches of 30 fresh processes with `OCERZ_TCACHE=off`; diff32 uses
four alternating pairs.

| Measurement | A: C `jit_flags` | B: Rust `jit_flags` | Change |
| --- | ---: | ---: | ---: |
| i386 `--jit-required` diff32 wall time (median, 4 pairs) | 21.295 s | 21.269 s | -0.1% |
| Time in `translate` (median, 4 pairs) | 612.732 ms | 656.811 ms | +7.2% |
| `tcache_work` (median, 6 batches × 30 processes, tcache off) | 33.420 ms | 42.816 ms | +28.1% |
| `avx_fp` (same) | 30.259 ms | 38.625 ms | +27.7% |
| `low_hoist` (same) | 29.883 ms | 37.981 ms | +27.1% |

### Isolated C/Rust `jit_flags` comparison after the fix

The C binary A was left unchanged; B was rebuilt with the scratch
initialization changes. The same six alternating batches of 30 processes and
four alternating diff32 pairs were used.

| Measurement | A: C `jit_flags` | B: Rust `jit_flags` | Change |
| --- | ---: | ---: | ---: |
| i386 `--jit-required` diff32 wall time (median, 4 pairs) | 21.543510 s | 21.216011 s | -1.52% |
| Time in `translate` (median, 4 pairs) | 631.470502 ms | 595.459109 ms | -5.70% |
| `tcache_work` (median, 6 batches × 30 processes, tcache off) | 30.732377 ms | 30.692419 ms | -0.13% |
| `avx_fp` (same) | 30.614764 ms | 30.385530 ms | -0.75% |
| `low_hoist` (same) | 29.899772 ms | 29.968046 ms | +0.23% |

The fix uses `MaybeUninit` for the per-call XLIVE dependency and decoded
instruction arrays, the single-instruction scan state, the two decoded logic
targets, the `IfConvDiamond` holder, and the 128-word compare/test scratch
buffer. Only successfully written prefixes are read; the C matcher still
initializes the complete `IfConvDiamond`, and `A64Buf` fields remain
initialized. After the fix, dynamic-fixture differences are within 0.75% of
the C build; diff32 `translate` time is 5.70% lower. No result is more than
about 3% slower, so the conditional profiler comparison was not run.
