# jit_memory.c -> rust/src/ported/jit_memory.rs

`src/jit_memory.c` (2184 lines) is ported as a single file,
`rust/src/ported/jit_memory.rs` (~4100 lines), function-by-function in the C
order, against `crate::jit_internal` for every `jit_internal.h` static-inline
helper and `crate::ffi` for all bindgen types, constants, `a64_*` emitters and
cross-module calls. The C file's opening prose block is carried over verbatim
as `//!` doc comments.

## Exports and state

34 C-ABI exports (every non-static function): `ocerz_jgb_trap`,
`g_cur_insns_fwd`, `emit_v_ld_at_`, `stack_plain_ok`, `vec_tso_relaxed`,
`insn_may_write_gpr`, `ea_cache_usable`, `emit_cmpxchg8b`,
`emit_commpage_guard`, `emit_gpr_ld_at`, `emit_gpr_lds_at`, `emit_gpr_st_at`,
`emit_guard_arms`, `emit_guest_load_ordered`, `emit_guest_load_ordered`,
`emit_low_hoist_bail`, `emit_low_hoist_check`, `emit_mem_ea`,
`emit_mem_ea_plain_ex`, `emit_mem_load_plain`, `emit_mov_mem`, `emit_movx`,
`emit_ordered_slow_arms`, `emit_plain_mem_fast`, `emit_reload_mem_base`,
`emit_rmw_mem`, `emit_v_st_at`, `emit_xchg_reg32`, `low_guard_fast_ok`,
`lowstack_delta_ok`, `lowstack_disp_ok`, `lowstack_disturbs`,
`select_low_hoist`, `stack_identity`.

Shared state (`#[unsafe(no_mangle)] pub static mut`, exact bindgen types and C
initializers): `g_mem_hoist_greg`, `g_low_hoist_greg`,
`g_mem_hoist_aux_index`, `g_mem_hoist_greg2`, `g_mem_hoist_greg3`,
`g_mem_hoist_scale`, `g_mem_hoist_base`, `g_mem_hoist_aux`,
`g_mem_hoist_greg_map`, `g_undo_want_slot`, `g_undo_*`, `g_ea_cache`
(`JitState_g_ea_cache`), `g_lowhoist_marks` (`MarkSet`), `g_fpbmap[256]` +
`g_n_fpbmap`, `g_al_marks`/`g_cp_marks[262144]`, `g_no_ldapr`, plus the
`-1`-initialized hoisting registers.

Private file-scope state (`static mut`): `g_const_ea*`, `g_ea_const`,
`g_ea_w32`, `g_oslow[OSLOW_MAX]` + `g_n_oslow`, `g_garm[GUARD_ARMS_MAX]` +
`g_n_garm`, `g_ea_lowhoisted_reg`, `g_low_hoist_*` fields. `g_garm`'s
anonymous C struct became a private `#[repr(C)] struct Garm`. `g_oslow`,
`g_fpbmap`, `g_garm` are filled by this module but read by core `jit.c`/`jit_*`
through the C ABI — ownership per jit-split.md is export-for-use, not private.

## No C shim

Nothing in the file needed `src/jit_memory_shim.c`: no setjmp/longjmp, no
thread-local storage, no special calling conventions, no compound literal
that couldn't be expressed field-by-field, no `static` globals shared across
TUs. `goto fold_disp` and `goto generic` restructured to flags/guards
(`fold_now`, `aux_disp_ok == 0`) preserving emission order.

## Deviations (mechanical only)

- `emit_mem_ea32`'s unused `insn` parameter -> `let _ = insn;` (C relied on
  `-Wno-unused-parameter`).
- C dead assignments kept as written; `let _ = have;` in `emit_mem_ea32`
  silences `unused_assignments`, `shut[]` kept immutable. No statements were
  dropped.
- `ENV_ON(name)` -> local `env_on!` macro_rules: each expansion site gets its
  own `static mut ON_: c_int = -1` lazy cache + `libc::getenv`, evaluated at
  the same point in C short-circuit order; `libc::atoi` where C used atoi.
  (ENV_ON is deliberately not in `crate::jit_internal`.)
- ACC_AT/ACC_REGOFF statement macros -> local `macro_rules` with identical
  bodies.
- `malloc` in `emit_ordered_slow_arms` -> `libc::malloc` (freed by C).
- `ocerz_jgb_trap` -> `libc::fprintf(crate::log::stderr(), ...)` with the
  identical format string, then `libc::abort()`.

## Gotchas for future ports

- `ENV_ON` has a per-call-site static: replicate the lazy `getenv` cache per
  expansion, never share one, and never evaluate it where C short-circuit
  skipped it.
- Strings passed to C must be `c"..."` literals or explicitly
  `'\0'`-terminated `*const c_char` (`concat!($name, "\0").as_ptr() as *const
  c_char`) — never `"...".as_ptr()`.
- The emission-audit reference worktree must be at `16a7c2d` or later (the
  audit hook landed there); `cac4b33` is too old.
- `g_oslow`/`g_fpbmap`/`g_garm`/`g_ea_cache` are owned here but consumed by
  core `jit.c` — keep them `#[no_mangle]` (or private only when the C was
  private) with exact bindgen layouts.
- Calls into not-yet-ported pieces (`emit_const_lit`, `emit_slowcall`,
  `emit_materialize`, `emit_cc_predicate_ex`, `oolslow_add`, `a64_*` etc.)
  go through `crate::ffi`, not direct Rust calls — modules switch over one at
  a time at the same symbol names.
- `X86Operand` `kind/reg/size/base/index/scale/high8` are `u8`; `op` on
  `X86Insn` is `u16`; `OCERZ_OP_*` constants are `c_uint` — compare via
  `as c_uint` in match patterns (they're const paths, so they work as
  patterns).
- `offsetof(OcerzCPU, x)` -> `core::mem::offset_of!(OcerzCPU, x)`; bindgen
  skips casted `#define`s — reproduce them from `jit_internal.rs` consts or
  ffi.

## Verification results

- `make -j8 ocerz`: links, `src/jit_memory.o` dropped from the link line,
  zero warnings from the module.
- `nm -g` symbol audit vs `~/AArchX-jitc/src/jit_memory.o`: every T/D/S/B
  symbol defined by `libocerz_rs.a`, no differences.
- `tools/jit_emit_audit.sh ~/AArchX-jitc` (reference @16a7c2d):
  `MATCH: 215295 blocks, 145523525 arm64 words` (offset + low, seed=1).
- Unit bins (`OCERZ_NO_ARM_EXEC=1`): test_a64emit, test_jit32, test_jit_exit,
  test_jit_order_transition, test_jit_psc_invalidate, test_chain_concurrency,
  test_sse, test_interp — all pass.
- `bash tools/rust_gate.sh` (full): PASS, no new failures vs baseline —
  build ok; unit ok (3 known `test_apidb` fixture failures); guests 134/134
  interp + 134/134 JIT; diff 100/0; diff32 real differential; dynamic 280
  pass / 7 known failures; native 86/1 known; guest libs, cxx, frameworks,
  format, swift all pass.

## Perf

Paired alternating runs of `tests/diff32 --jit-required` (offset corpus,
OCERZ_TCACHE=off, OCERZ_JITMEASURE=1), 4 per build, candidate at the
f9753d8-era tip vs `~/AArchX-jitc` @16a7c2d:

| Measurement | Candidate | Reference | Change |
| --- | ---: | ---: | ---: |
| i386 harness wall (median) | 20.585 s | 20.545 s | +0.2% |
| Time inside translate (median, 98304 blocks) | 500 ms | 499.5 ms | +0.1% |

Spreads overlap run-to-run noise (wall ±0.22 s, xlat ±21 ms): no material
translator-speed regression. Raw numbers: `~/jit_memory_perf.txt`.
