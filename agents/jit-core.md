# jit (core): Rust port notes

`src/jit.c` (2844 lines) is replaced by `rust/src/ported/jit.rs` plus a 77-line C
shim, `src/jit_core_shim.c`. The Rust module covers translation orchestration
(`translate`), pin-class / call-region / frequency pin selection, memory-base and
low-stack hoist selection, stack-pair recognition, inline-call recognition and
callee splicing, return-seam flag liveness, the backward `fl_need` pass, the
per-instruction emission dispatch with all fusion paths, FP-batch / lane-0 /
x87 arms, side exits and stubs, relocation and fixup patching, the cfg-gated
audit hook, `tc_bind`/tcache, code-index insertion, chain installation,
statistics, `jit_interp_block` and `ps_report`.

## Shape

- Exports keep their C prototypes: `translate`, `jit_interp_block`,
  `is_terminator`, `m32_inline_ok`, `g_pin_class_fwd`, `rsp_ptr3`, `ps_report`.
  Owned shared state (`g_flaglive_log`, `g_x87spec_marks`, `g_xlat_ftop`,
  `ocerz_jit_time_xlat`, `ocerz_jit_xlat_ns`, `ps_hits`, `ps_misses`,
  `ps_retsite`, `ps_steps`, `ps_t0`, `t_xlat_overflow`) is
  `#[unsafe(no_mangle)] pub static mut` with the bindgen types and C
  initializers. File-scope C statics are private statics; function-local
  statics stay function-local.
- Calls into the other pieces (ported or not) go through `crate::ffi` in this
  switch-over; `crate::jit_internal` supplies the header inlines. `ENV_ON` sites
  use a per-site `env_on!` cache.
- C indexes its fixed arrays unchecked; the port does the same through
  `ga!(array, i)` (raw element pointer + offset), and the large stack arrays of
  `translate` (`exit_sites`, `epi_sites`, `fl_need`, ...) are `MaybeUninit` so
  no zeroing is added to the hot path.
- C `continue` inside the emission loop is `break 'body` out of a labeled block
  whose tail increments `i`.

## The decode shim

`translate`'s decode loop runs under `sigsetjmp` with
`ocerz_jit_decode_recover` set, so a fault reading guest code `siglongjmp`s back
into it. Rust cannot host a returns-twice call, so the loop (with its call
splicing trigger and superblock extension/JCC flip) is `jit_core_decode` in
`src/jit_core_shim.c`, unchanged from C. It calls back into Rust through three
plain-data exports: `jit_core_decode_reset` (clears `g_ic_kind`,
`g_ic_pushelide`, `g_promo_reg`), `jit_core_ic_kind_set` and
`jit_core_splice_callee` (the Rust `splice_callee`). No Rust frame with a live
`Drop` value sits between the `sigsetjmp` and the decoder; `splice_callee` itself
decodes and may be unwound by the longjmp, and holds only integers and raw
pointers.

## Gotchas

- Unit bins must run with `OCERZ_NO_ARM_EXEC=1` (the gate and Makefile do);
  without it `test_jit32` and `test_jit_order_transition` fail on the C reference
  too.
- Register / condition constants are `u32` in bindgen; the module re-declares
  the ones it uses as `c_int` consts so call sites match the C prototypes.
- `tc_roundtrip` returns a pointer (`!tc_roundtrip(...)` in C is `.is_null()`).
- Every Rust static (exported, file-scope and function-local) was checked
  against its C declaration for width, signedness and initializer: `int` ->
  `c_int`, `unsigned` -> `c_uint`, `unsigned long long` / `uint64_t` -> `u64`,
  `_Atomic unsigned long long` -> `AtomicU64`, `__thread` -> `#[thread_local]`,
  `-1` env caches stay `-1`, zero-initialized arrays and `MarkSet` are zero.
  Release builds have `overflow-checks = false`, so plain integer arithmetic
  wraps as C's unsigned arithmetic does.

## Verification (rebased onto b849b33)

- Line-by-line re-audit of jit.rs against jit.c (all of it, incl. ps_report):
  branch and check order, which g_* toggle / env cache each branch reads, log
  strings and their conditions, finalize order (audit hook, tc_bind,
  code_index_append_locked, EMITCHECK, lanerec, tc_put/tc_verify, chain
  install, stats). No semantic difference. It found 16 bounds-checked reads of
  the shared per-insn arrays (g_mov_skip, g_fpb_*, g_jcc_edge, g_call_edge);
  they now go through the unchecked `ga!` accessor like the C.
- `tools/jit_emit_audit.sh ~/AArchX-jitc ~/AArchX` (reference 16a7c2d):
  MATCH, 215,295 blocks, 145,523,525 arm64 words, offset + low, seed 1. The
  runner's existing Rust-core build path was used unchanged. The corpus is
  i386 only; the x64 corpus (`--corpus all`) had not landed, so the full
  gate's native phases are the x86-64 evidence.
- Unit bins test_a64emit, test_jit32, test_jit_exit, test_jit_order_transition,
  test_jit_psc_invalidate, test_chain_concurrency, test_sse, test_interp: pass.
- `tools/rust_gate.sh` (full) on 6d3fdcf and again on b849b33: PASS, no new
  failures. dyn 278 pass / 9 known; run_native_tests 86 pass / 1 known;
  guest library, framework and format suites pass.
- An earlier full gate (on 5212e30) hit `dthread_signal-jit` ("SIGUSR1 not
  delivered within 2s" in one of 120 rounds). It is a flake, not from the
  core: 90 isolated runs gave 7 failures with the Rust core and 4 with the C
  core built from the same tip (a second 60/60 batch was 3 vs 3), and it
  passed in both later full gates.
- `datomic_counter-no-jit` (known interpreter-mode slowdown, the lead is
  bisecting it) passed in the full gate and the dynamic rerun. It is not worse
  with the Rust core: five alternating runs, median 30.25 s with the Rust core
  vs 30.14 s with the C core on the same tip (spread 28.2-31.8 s).

## Performance (vs ~/AArchX-jitc at 16a7c2d, same VM)

i386 differential harness (`tests/diff32.c --jit-required`, offset corpus)
built against each tree's core objects with a constructor that enables
`ocerz_jit_time_xlat`, eight alternating-order runs, medians:

| Measurement | C (16a7c2d) | Rust core | Change |
| --- | ---: | ---: | ---: |
| Time inside `translate` | 764.9 ms | 689.8 ms | -9.8% |
| Whole harness | 22.52 s | 22.23 s | -1.3% |

A separate four-run harness comparison without the counter was noisy
(20.5-24.3 s spread, median +5.6%); the eight-run timed numbers above are
the reliable ones.

Dynamic fixtures, `OCERZ_TCACHE=off`, medians of six alternating-order batches
of 30 fresh processes:

| Fixture | C (16a7c2d) | Rust core | Change |
| --- | ---: | ---: | ---: |
| `tcache_work` | 40.10 ms | 39.78 ms | -0.8% |
| `avx_fp` | 36.74 ms | 36.71 ms | -0.1% |
| `low_hoist` | 39.24 ms | 38.95 ms | -0.7% |

The candidate includes every piece ported since 16a7c2d (integer, fp, memory,
tcache, ...), so these compare whole translators, not the core alone.

### Direct calls into ported pieces (after the Rust decoder landed, ae45b15+)

Switching the core's calls into jit_integer (31 functions), jit_memory (18)
and jit_flags (16) from the `crate::ffi` extern declarations to explicit
`use crate::ported::<piece>::{...}` imports gives a byte-identical `__text`
section (`otool -t` hash equal; only debug info and the UUID differ). The ffi
externs name the same symbols the Rust pieces define, and fat LTO over the
single crate already resolves and inlines them, so a direct-call commit buys
nothing and was not landed. The emission audit stayed MATCH with it. Same-tip
timing, eight alternating runs, inside `translate`: ffi calls 657.2 ms,
direct imports 663.6 ms (+1.0%, noise; 566-711 ms spread), C reference
701.1 ms (Rust core -6.3%). Whole harness: 21.80 s / 21.79 s / 21.67 s.
