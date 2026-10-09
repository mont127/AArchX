# jit_simd: Rust port notes

`src/jit_simd.c` (3121 lines) is replaced by `rust/src/ported/jit_simd/mod.rs`:
MMX, legacy SSE (moves, shuffles, packed integer and floating-point
arithmetic, compares, conversions, pmovmskb), CRC32, AES/PCLMUL selection,
AVX/VEX (128- and 256-bit integer, FP and FMA forms) and the upper-YMM
zeroing rules, plus the SSE memory-operand front end `emit_sse_mem_addr`.

## Shape

- The 10 exported functions (`emit_crc32`, `emit_mmx`, `emit_pmovmskb`,
  `emit_sse`, `emit_sse_mem_addr`, `emit_vex`, `emit_ymmh_clear`,
  `sse_enabled`, `xmm_global_enabled`, `xmm_pinning_enabled`) keep their
  `jit_internal.h` prototypes as `#[unsafe(no_mangle)] pub unsafe extern "C" fn`;
  C `static` helpers are private Rust fns. Case order, grouped cases and
  fallbacks follow the C one-to-one, because the order picks the encoding.
- The 14 shared variables this piece owns (`g_blk_ymm_write`,
  `g_cmps_mask_idx` (=-1), `g_cur_need`, `g_fpb_fast`, `g_n_raslit`,
  `g_raslit`, `g_sse_mem_disp`, `g_sse_mem_plain`, `g_sse_mem_plainacc`,
  `g_sse_mem_ra` (=JTA), `g_vec_int_move`, `g_xmm_pinned`, `g_ymmh_zero`,
  `g_zero_vreg` (=-1)) are `#[unsafe(no_mangle)] pub static mut` with the
  bindgen types and C initializers; `g_vex_mem_skip` is a private static.
  All 24 global symbols of the reference `src/jit_simd.o` are defined once.
- Header `static inline` helpers come from `crate::jit_internal` (and
  `ocerz_cc_pack`/flag bits from `crate::inline`); the one `decode.h` inline it
  needs, `ocerz_insn_has_mmx`, is a private helper. Calls into the other JIT
  pieces (FP lane/batch/NaN machinery, memory EA and guards, control) go
  through `crate::ffi`.
- The `ENV_ON` site (`OCERZ_NO_COMIS_MEM_FUSE`) and the one-shot `getenv`
  caches (`OCERZ_NO_XMM_PIN`, `OCERZ_NO_XMM_GLOBAL`/`OCERZ_NO_FULLPIN`,
  `OCERZ_NO_INLINE_SSE`, `OCERZ_INEXACT_NAN` (two sites),
  `OCERZ_NO_JIT_MMX`, `OCERZ_NO_INLINE_VEX`, `OCERZ_NO_YMMH_FLAG`) are
  function-local `static mut` initialised to -1, filled on first use, as in C.
  Every string handed to C is a `c"..."` literal.
- No C shim: nothing here has its address taken by emitted code, uses
  `__builtin_return_address` or a special calling convention, or runs in
  signal context. No allocation, formatting or `Drop` values.

## Gotchas

- The translation is deliberately literal (C integer casts and the C
  statement structure are kept, with a module-level `#![allow]` for the
  resulting unused-assignment/unsafe lints) so it can be checked against
  `src/jit_simd.c` line by line; tidy it only with the emission audit in the
  loop.
- `jit_internal` predicates return `c_int`, not `bool`; compare with `!= 0`.

## Verification

- `tools/jit_emit_audit.sh ~/AArchX-jitc` (reference at 16a7c2d): MATCH,
  215,295 blocks, 145,523,525 arm64 words, offset + low layouts.
- Unit bins test_a64emit, test_jit32, test_jit_exit, test_jit_order_transition,
  test_jit_psc_invalidate, test_chain_concurrency, test_sse, test_interp: pass.
- `tools/rust_gate.sh --fast`: PASS.
- `tools/rust_gate.sh` (full) at `cd41ce454c4ae39f8fc008d5a74471c22761d716`:
  PASS, no new failures vs `agents/baseline.md` (3 expected unit records,
  7 expected dynamic failures, 1 expected native failure).
- `tools/rust_gate.sh` (full) at f9753d8: only new failure was
  `dthread_signal-jit` ("SIGUSR1 not delivered within 2s" in one round). It is
  a pre-existing flake: 30 alternating runs each gave 3/30 failures with this
  port and 1/30 with C `jit_simd.o` on the same tip, all with the same symptom.
  At a0f5f84 the native framework compat failure seen by the other JIT ports
  appeared too; it was the log-format NUL bug fixed in f6b9e36/f9753d8.

## Performance (same VM, alternating order)

Primary reference is the same tip (f9753d8) with C `jit_simd.o`; the split C
tree (16a7c2d) is shown for context, but the tip differs from it in other
already-ported modules.

| Measurement | Same-tip C | Rust | Change | vs 16a7c2d |
| --- | ---: | ---: | ---: | ---: |
| `ocerz_jit_time_xlat`, diff32 offset, median of 6 | 890.3 ms | 887.0 ms | -0.4% | +4.7% |
| diff32 offset `--jit-required` wall, median of 6 (xlat runs) | 23.86 s | 23.88 s | +0.1% | +3.5% |
| dynamic `tcache_work`, 6 x 30 processes | 41.07 ms | 40.53 ms | -1.3% | -0.8% |
| dynamic `avx_fp`, 6 x 30 processes | 41.28 ms | 41.25 ms | -0.1% | +0.1% |
| dynamic `low_hoist`, 6 x 30 processes | 41.08 ms | 40.82 ms | -0.6% | +1.8% |
| guest JIT suite, one run | 2.40 s | 2.29 s | -4.5% | +0.5% |

Run-to-run spread on diff32 is about 2-3 s, so none of these is a measurable
change against the same-tip C build. Not yet done: switching hot cross-piece
calls (FP batch/NaN helpers, memory EA) from `crate::ffi` to direct Rust calls
now that jit_fp and jit_memory are Rust too.
