# jit_integer: Rust port notes

`src/jit_integer.c` (2678 lines) is replaced by `rust/src/ported/jit_integer.rs`:
pinned GPR reads/writes, the pin prologue, scalar ALU (reg/imm/mem, narrow
8/16-bit, ADC/SBB), INC/DEC, MOV+logic and ADD+INC pair fusion, immediate,
CL-count and double shifts, rotates, MUL/IMUL, DIV/IDIV with the out-of-line
slow path, NOT/NEG, CBW/CWD family, MOVSXD, LEA, CMOVcc, SETcc (including the
SETcc+MOVZX sink), BSWAP, BSF/BSR/TZCNT/LZCNT/POPCNT, BT/BTS/BTR/BTC and
64-/32-bit PUSH/POP/LEAVE including memory operands.

## Shape

- All 32 exported functions keep their `jit_internal.h` prototypes as
  `#[unsafe(no_mangle)] pub unsafe extern "C" fn`; C `static` helpers are
  private Rust fns. Check order and every special case follow the C source
  one-to-one, because the order picks the encoding.
- The 17 shared variables this piece owns (`g_cc_direct`, `g_cur_insn_start`,
  `g_defer`, `g_div_prev_skipped`, `g_m32low`, `g_mov_sink_at`, `g_mov_skip`,
  `g_n_pinned`, `g_no_addincfuse`, `g_no_lazyflags`, `g_nzcv_from` (=-1),
  `g_nzcv_kind`, `g_nzcv_want`, `g_oolslow_pre`, `g_pin`, `g_pin_class`,
  `g_pin_hold`) are `#[unsafe(no_mangle)] pub static mut` with the bindgen
  types and C initializers; the private `g_no_inline_imul` is a private static.
- Header `static inline` helpers come from `crate::jit_internal`; calls into
  other pieces (memory EA/guards, flags predicates, slow calls, oolslow,
  `emit_materialize`, `stack_identity`, ...) go through `crate::ffi`.
- `ENV_ON(name)` sites use a local `env_on!` macro (one `static mut` cache per
  call site + `libc::getenv`), matching the C per-site caching. Other one-shot
  `getenv` statics are function-local `static mut`s as in C.
- No C shim: nothing here takes addresses used by emitted code, uses
  `__builtin_return_address`, special calling conventions or runs in signal
  context.

## Gotchas

- `g_mov_skip`/`g_mov_sink_at`/`g_push_fix` are indexed through raw pointers
  (`(&raw mut X).cast::<T>().add(i)`) to keep C's unchecked indexing.
- `emit_div` rewinds `b->p` by one word and re-emits a CMN encoding; this is
  `(*b).p = (*b).p.sub(1)` followed by `a64_emit32`.
- `jit_internal` predicates return `c_int`, not `bool`; compare with `!= 0`.
- Immediate negation of displacements uses `wrapping_neg()` then truncation to
  `u32`, matching C's `(uint32_t)-disp`.

## Verification

- `tools/jit_emit_audit.sh ~/AArchX-jitc` (reference at 16a7c2d): MATCH,
  215,295 blocks, 145,523,525 arm64 words, offset + low layouts, seed 1.
- Unit bins test_a64emit, test_jit32, test_jit_exit, test_jit_order_transition,
  test_jit_psc_invalidate, test_chain_concurrency, test_sse, test_interp: pass.
- `tools/rust_gate.sh --fast`: PASS.
- `tools/rust_gate.sh` (full): only new failure is run_native_framework_tests,
  where the JIT compat case's stderr starts with a stray `:/:\capacity overflow`
  prefix. It reproduces identically with this file removed (C
  `src/jit_integer.o` linked) on the same tip, so it is not from this port.

## Performance (vs ~/AArchX-jitc at 16a7c2d, same VM)

Dynamic fixtures, `OCERZ_TCACHE=off`, medians of six alternating-order
batches of 30 fresh processes (wall time per process):

| Fixture | C | Rust | Change |
| --- | ---: | ---: | ---: |
| `tcache_work` | 40.668 ms | 39.366 ms | -3.2% |
| `avx_fp` | 37.857 ms | 37.769 ms | -0.2% |
| `low_hoist` | 36.212 ms | 36.439 ms | +0.6% |

i386 differential harness (`tests/diff32.c --jit-required`, offset corpus,
built against each tree's core objects), eight alternating-order runs:
C 24.373 s, Rust 24.253 s median (-0.5%; run-to-run spread is about 3 s, so
this is no measurable change). A first four-run pass showed +5.0% inside the
same spread; the longer pass does not reproduce it. Translation-time work in
this piece is a small share of `translate`, so no regression is expected.

After rebasing onto f9753d8 (the log-format NUL fix) the native framework
failure is gone; the full gate's only new failure was `datomic_counter-no-jit`
hitting the 30 s dynamic timeout. That case runs in the interpreter (no
translation, so none of this module runs) and takes 30-31 s in isolation on
this VM (27-29 s for the 16a7c2d reference), so it is a borderline timeout,
not a JIT difference. The audit still reports MATCH after the rebase.
