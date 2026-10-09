# jit_fp.c -> rust/src/ported/jit_fp/

Ported the whole file (3203 lines) to Rust. The Makefile PORTED wildcard drops
`src/jit_fp.o` once `ported/jit_fp/mod.rs` exists. Emitted arm64 is
byte-identical to C (audit below).

## Layout

- `mod.rs` — module doc, the block-level shared state (`g_cur_blk`,
  `g_cur_insn_idx`, `g_cur_insns(_n)`, `g_keep(_n)`), `c_int` aliases for the
  bindgen constants the file uses (JT*/VX*/A64 conds/RK_*/K2_*/X87R_*/XF_*/
  XK_*/XS_*/TCR_BLK/sizes), `a64_inv`, and the `env_on`/`getenv_on` lazy-getenv
  helpers (jit_internal.rs intentionally does not cover ENV_ON).
- `nan.rs` — `emit_nan_cold_scalar/packed`, `emit_nan_fix_scalar2/packed2`,
  `emit_nan_ool_arms` and `g_nanool`, `g_n_nanool`, `g_scpend`,
  `g_scalar_merge_next`, `g_pk_consts_needed`, `g_fcmp_self_vreg`,
  `g_fcmp_self_idx`.
- `fpb.rs` — `mov_sink_gap_ok`/`mov_sink_scan`, fpb classification,
  `fpb_scan_v1`/`fpb_scan_v2`, `fpb_v1`, `unsafe_nocheckbr`,
  `fpb_emit_check`/`fpb_emit_regs_check`/`fpb_emit_store_check` and all the
  `g_fpb*` state (including the file-private mrd/mwr/marith/mmem/mstore/stdbl
  tables as private `static mut`).
- `l0.rs` — `l0_enabled`, `lanerec_note`, `l0_defer`, `l0_alloc2`, mov128 pair,
  `vex_cmps_blendv_pair`, `l0_fixed_setup`/`l0_fixed_restore`, `yc_setup`,
  `cmps_blendv_fusable`, `vex_lane_aware`, `l0_pre_insn`, `l0_aware_op` and all
  `g_l0*`/`g_yc*`/`g_lanerec`/`g_lane_used`/`g_undo_vreg` state.
- `x87.rs` — `x87_run_flags`, `emit_x87`, `emit_x87_arms` plus every private
  x87 helper and the `g_x87*` state; the C anonymous-struct `g_x87_site` and
  `g_x87_frag` tables became private `X87Site`/`X87Frag` Rust structs.

All header inlines go through `crate::jit_internal`; all cross-piece calls go
through `crate::ffi`. No shim — nothing had to stay in C.

## Conventions

`#[unsafe(no_mangle)] pub unsafe extern "C"` for the exported symbols,
`#[unsafe(no_mangle)] pub static mut` for owned shared state with the exact C
initializers, private `static mut` for file-scope state, one private
`static mut` per ENV_ON site. Static-mut access via `&raw mut`/`&raw const`
raw pointers and small unchecked-index helpers; no bounds checks, allocation,
locks or Drop. `int`/`long`/`?:` ordering/short-circuit side effects preserved.
The `OCERZ_FPB_DBGPRINT` fprintf goes through `libc::fprintf` on
`crate::log::stderr()`.

## Deviations

- The two anonymous-struct state tables (`g_x87_site`, `g_x87_frag`) are named
  private structs — same fields, same order, same element size.
- `l0_alloc2`'s `g_l0_next++ % g_l0_nlanes` uses wrapping i32 remainder (C's
  signed `%` semantics on nonnegatives).
- The `OCERZ_FPB_DBGRIP` debug block keeps its lazy-init semantics; `getenv`d
  values are cached in per-site `static mut`s.

## Verification

- `make -j12 ocerz` links with zero undefined/dup symbols; `src/jit_fp.o` is
  absent from the link line; `make apis` ok.
- `tools/jit_emit_audit.sh ~/AArchX-jitc` (reference at 16a7c2d):
  `MATCH: 215295 blocks, 145523525 arm64 words (offset + low, seed=1)`.
- Units with `OCERZ_NO_ARM_EXEC=1`: test_a64emit, test_jit32, test_jit_exit,
  test_jit_order_transition, test_jit_psc_invalidate, test_chain_concurrency,
  test_sse, test_interp — all pass.
- `tools/rust_gate.sh --fast`: PASS, no new failures.
- `tools/rust_gate.sh` full: PASS. A flaky `compat.jit.err` interleave
  (`:/:\capacity overflow` prepended before the identity-arena log) failed
  the native-frameworks phase on two earlier gate runs; it reproduces on a
  jit_fp-less build of the same tip and on ~/AArchX-jitc, so it is a
  pre-existing tip issue, not this port (also recorded under
  "Tip breakages" in status.md).

## Perf

vs a C-jit_fp build of the same tip (9595b42, temporary worktree, removed
after), plus ~/AArchX-jitc (16a7c2d). diff32 runs are the `--jit-required`
harness with `ocerz_jit_time_xlat` enabled via an uncommitted ctor; dynamic
fixtures are `OCERZ_TCACHE=off`, batches of 30 fresh processes, two rounds.

| measurement | Rust jit_fp | C jit_fp (same tip) | ~/AArchX-jitc |
| --- | ---: | ---: | ---: |
| diff32 wall median (s) | 21.1 | 21.0 | 21.1 |
| `ocerz_jit_xlat_ns` median (ms) | 596 | 600 | 631 |
| tcache_work (ms/proc) | ~258 | ~258 | ~263 |
| avx_fp (ms/proc) | ~11.0 | ~10.9 | ~11.7 |
| low_hoist (ms/proc) | ~8.0 | ~7.8 | ~7.7 |

No material translator-speed regression.
