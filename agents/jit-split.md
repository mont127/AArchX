# JIT split and Rust-porting contracts

This is a mechanical split of the 23,531-line `src/jit.c` at
`97a92471fedc13f63075bf6ff641bcb739852c58`, not a Rust port or an algorithm change.
There are nine C translation units, including the retained core. The original
opening prose is distributed by topic, and existing inline explanations have
been moved to the corresponding opening blocks. The Makefile wildcard discovers
the new files without a build-system edit.

## Emission oracle for Rust ports

Run on arm64 macOS with Clang, Python 3.9+ and the repository's Cargo/toolchain
on PATH. The reference is the C JIT split at `cac4b33` (its unrelated
flags/globals modules already use the Rust scaffold):

```sh
git worktree add --detach ../AArchX-jit-reference cac4b33
tools/jit_emit_audit.sh ../AArchX-jit-reference
tools/jit_emit_audit.sh ../AArchX-jit-reference /path/to/candidate
```

The omitted candidate defaults to the checkout containing the tool, not the
current directory. Both trees are built normally, then separate instrumented
executables are linked in a temporary directory. Sources, normal JIT objects
and normal Rust archives are not replaced. Linking uses Makefile's filtered
`CORE_OBJS`, avoiding stale objects for already-ported C modules. Both binaries
use the **reference's** `tests/diff32.c`, seed 1, 20,000 random cases, all hand
cases, and both offset/low-shadow layouts with `--jit-required`. Ambient
`OCERZ_*` tuning variables are removed from corpus processes;
`OCERZ_TCACHE=roundtrip` and `OCERZ_NO_ARM_EXEC=1` are set explicitly. Do not
run this alongside another gate or mutate either tree during the comparison.

Stdout starts with one `MATCH` or `MISMATCH` summary, followed on mismatch by
the first three differing blocks: ordinal/layout, guest RIP, x86 disassembly,
relocation descriptors and both complete raw little-endian arm64 byte streams.
Build/run progress goes to stderr. Exit status is 0 for MATCH, 1 for MISMATCH,
2 for a failed/incomplete oracle (build failure, absent hook, malformed stream,
failed corpus, timeout, etc.). A failed guest run never produces MATCH. Build
logs, corpus logs, raw audit streams and instrumented binaries are retained
on failure at the printed artifact path. To retain successful runs too, set
`OCERZ_JIT_AUDIT_DIR=/path/to/artifact-parent`; each invocation gets a unique
subdirectory. Normal successful runs remove their temporary artifacts.

The comparison masks only form-1 two-word address literals and form-0
MOVZ/MOVK imm16 fields. MOVZ/MOVK shape/register consistency, bounds and
non-overlapping relocation ranges are validated before masking. All opcode,
register and non-relocated bits remain significant. Relocation offset, kind,
form **and semantic argument** must match, as must record order, block count,
guest RIP and x86 instruction boundaries. Disassembly formatting is diagnostic
only. Missing/empty/truncated streams cannot pass. The versioned `AXJITA01`
format is documented in `tools/jit_emit_audit/writer.c` and keeps raw bytes
unmodified for diagnostics. The legacy hashes below use the earlier normalized
format, not these richer raw stream files.

### Hook contract: preserve this in the Rust core

`translate` calls the C ABI symbol `ocerz_jit_emit_audit` after finalizing every
arm64 word and relocation, immediately before `int tc_save = 0` / `tc_bind`,
while the existing translation lock is held. Its bindgen-visible signature is:

```c
void ocerz_jit_emit_audit(uint64_t rip, const uint32_t *code, uint32_t nwords,
                          const X86Insn *insns, uint32_t ninsns,
                          const TcReloc *rel, uint32_t nrel);
```

Arguments are `rip`, `entry`, `blk->code_words`, `blk->insns`, `n`, `g_tc_rel`,
and `g_tc_nrel`, in that order. The sink copies synchronously and retains no
caller pointers. It lives only in `tools/jit_emit_audit/writer.c`; do not
provide a competing implementation in the Rust staticlib. Keep `TcReloc`
semantics and numbering intact in the tcache port.

C uses `#ifdef OCERZ_JIT_EMIT_AUDIT`; the runner defines it only for its
temporary core object. The old `cac4b33` core has no hook, so the runner inserts
the same call into a temporary source copy at the unique pre-tcache marker.
When `jit` itself is ported, preserve this call behind
`#[cfg(ocerz_jit_emit_audit)]` using `ffi::ocerz_jit_emit_audit` and the same raw
pointer/integer arguments. The runner builds a separate Rust archive with
`cargo rustc --release --lib --target-dir ... -- --cfg ocerz_jit_emit_audit`.
It does not enable this cfg in normal builds. The runtime environment variable
`OCERZ_JIT_EMIT_AUDIT` names the output stream; unset/empty disables recording
even in instrumented executables. Normal C/Rust builds contain **no audit
call or branch**, no environment lookup, and no linked writer.

Tool checks: `python3 -B -m unittest discover -s tools/jit_emit_audit` and
`bash -n tools/jit_emit_audit.sh`. Integration against `cac4b33` reproduces
**215,295 blocks / 145,523,525 arm64 words**. This is the original i386 corpus,
not exhaustive x86-64 instruction coverage; ports must still run the normal
guest/differential/full gates.

The integration self-check changed padding NOPs to YIELD in a disposable C
candidate. Both architectural differentials still passed, but the oracle
reported **14,601 differing blocks**, exited 1, and printed the requested RIP,
disassembly and byte-stream diagnostics. Re-encoding the matching run in the
legacy format also reproduced both SHA-256 hashes recorded below exactly.

## Ownership map

Line counts include opening prose and declarations. The exact exported symbols
and owned state appear below each piece's porting advice; header declarations
are the authoritative C signatures. Calls here list direct out-of-line calls to other split pieces, not
ordinary dependencies such as `a64emit`, `decode`, `mem`, `interp` or `vm`.
Per-piece caller tables cover every exported function and shared-state symbol.
They include address-taking and references through header-inline helper chains;
public functions can also have callers outside these nine JIT pieces. `core`
means `jit.c`; the other labels correspond to `jit_<label>.c`.

| Piece | Lines | Responsibility | Calls other pieces | State (shared / private definitions) |
| --- | ---: | --- | --- | ---: |
| `src/jit.c` | 2848 | Translation orchestration, decoding a block, pin/hoist selection, emission dispatch, block fallback and perf reporting | cache, control, flags, fp, integer, memory, simd, tcache | 11 / 18 |
| `src/jit_cache.c` | 3185 | Block/hash/arena lifecycle, invalidation indices, retire/flush barriers, signal-safe lookups, chaining and stop-site patching | control, core, flags, tcache | 27 / 51 |
| `src/jit_control.c` | 2070 | Calls/returns, indirect dispatch, RAS, bridge/leaf transitions, interpreter slow calls and tracing helpers | cache, flags, integer, memory, simd | 33 / 7 |
| `src/jit_flags.c` | 2778 | Flag/GPR/XMM liveness, deferred NZCV recipes, cmp/test/Jcc fusion, superblocks, if-conversion and branch-flip feedback | cache, core, integer, memory, simd, tcache | 31 / 6 |
| `src/jit_integer.c` | 2678 | Pinned GPR reads/writes, scalar arithmetic, shifts, divides, bit operations and push/pop | control, flags, fp, memory, simd | 17 / 1 |
| `src/jit_memory.c` | 2184 | Effective addresses, low-shadow addressing, ordered/alignment guards, commpage guards, RMW and stack/address guards | control, core, flags | 31 / 11 |
| `src/jit_simd.c` | 3121 | MMX, legacy SSE, AVX/VEX, CRC and packed-integer/float instruction selection | control, fp, memory | 14 / 1 |
| `src/jit_fp.c` | 3203 | Scalar/vector FP batching, lane residency, NaN fixups, deferred checks, x87 TOP/tag/exception runs and recovery | control, flags, memory, simd | 67 / 20 |
| `src/jit_tcache.c` | 1063 | Translation-cache relocation, serialization, validation, load/store and roundtrip/verification | cache, control, memory | 18 / 18 |

## Shared header and non-negotiable contracts

The 2,192-line `include/ocerz/jit_internal.h` defines `OcerzJit`, `JitBlock`, edge/profiling,
fault/lane-recovery, cache-index, translation-cache and scratch-state layouts,
register constants, macros and cross-piece prototypes/externs. It contains 129
small `static inline` helpers, including the transitive tiny helpers they call.
They need private inline Rust equivalents; bindgen deliberately does not bind
static inline definitions. Do not create extra instances of their shared state.

- All 698 original function bodies retain the same C tokens in normal builds
  (the later audit call is preprocessed out). Only placement and
  linkage change. Functions whose addresses are used by emitted code/tcache
  remain real definitions, not header-local inline copies.
- Each shared mutable variable is defined in exactly one C piece. Newly exported
  zero-initialized byte arrays have explicit `{0}` initializers, preserving static
  zero initialization while avoiding Darwin common-symbol over-alignment.
  Existing explicit initializers and function-local statics are unchanged.
- The original nested tag-only `struct JitPromo` declaration is placed before
  `JitBlock`; it was not a data member. Shared anonymous scratch structs receive
  `JitState_*` tags so extern declarations and Rust bindings denote the same type.
  No fields, orders, sizes or offsets are changed.
- Use bindgen's layouts, including atomics, packed/aligned records and thread
  locals. Keep `offsetof` expressions and arm64 register assignments exact.
  Pointer-sized fields, code-word offsets versus byte offsets, and signed
  sentinels are particularly easy to mistranslate.
- Bindgen's plain extern-static declaration is not sufficient for C TLS. Use
  the scaffold's `#[thread_local]` extern pattern for `ocerz_jit_decode_recover`
  and any exported thread-local state; preserve private TLS as TLS too.
- The cross-piece state remains the existing single translator state, protected
  by the existing locking/lifecycle discipline. This split adds no locks,
  allocations, bounds checks or new state transitions.
- Preserve signal and longjmp behavior. A Rust frame that can be bypassed must
  contain only plain data: no `Vec`, `String`, lock guard or other Drop value.
  Neither Rust panics nor unwinding may cross the C/generated-code boundary.
- Port each `jit_<area>` as that module name; leave core `jit` until its callers
  and callees have faithful C ABI surfaces. Do not port the header's inline
  helpers as exported duplicates. Public `ocerz_jit_*` declarations remain in
  the existing public header.

## jit.c

This is the hottest *orchestrator*: `translate` selects all the emitters and is the only place that sets up and tears down a block's scratch state. Keep its call ordering, fallback paths, barriers and cleanup exactly as written. `sigsetjmp` protects decoding; restore the previous `ocerz_jit_decode_recover` on every path. Never hold a Rust Drop value in a frame that can be jumped over. The public `ocerz_jit_step` wrapper is in cache, alongside thread entry/exit and cache generations. Most emitters need this shared state, so port the leaf emitters first and core last. The stack-pair and low-hoist recognizers stay here because they inspect the block-level instruction stream.

Existing external entry points:

`g_pin_class_fwd`.

New cross-piece function exports:

`is_terminator`, `jit_interp_block`, `m32_inline_ok`, `ps_report`, `rsp_ptr3`, `translate`.

Shared state definitions owned here:

`g_flaglive_log`, `g_x87spec_marks`, `g_xlat_ftop`, `ocerz_jit_time_xlat`, `ocerz_jit_xlat_ns`,
`ps_hits`, `ps_misses`, `ps_retsite`, `ps_steps`, `ps_t0`, `t_xlat_overflow`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| None; local or external-to-JIT consumers | `ocerz_jit_xlat_ns` |
| cache | `g_flaglive_log`, `g_xlat_ftop`, `jit_interp_block`, `ps_hits`, `ps_misses`, `ps_report`, `ps_steps`, `ps_t0`, `t_xlat_overflow`, `translate` |
| cache, control, flags, fp, integer, memory, simd | `rsp_ptr3` |
| cache, flags | `ocerz_jit_time_xlat` |
| control | `ps_retsite` |
| flags | `g_x87spec_marks`, `is_terminator`, `m32_inline_ok` |
| memory | `g_pin_class_fwd` |

Private file-scope state owned here:

`g_align_any`, `g_align_blk`, `g_cur_fpb`, `g_flag_producer_operands_intact`, `g_ic_expect`,
`g_ic_kind`, `g_ic_pair_rj`, `g_ic_pushelide`, `g_keep_cap`, `g_lowstack_check`, `g_lowstack_from`,
`g_promo_mate`, `g_promo_push_of`, `g_promo_reg`, `g_promo_seq`, `g_rsp_lag`, `g_xlat_n`,
`ps_shape_name`.

Helpers from this subsystem now inline in the shared header:

`g_xlat_mode32_fwd`, `ps_retsite_counter`, `rsp_is_ptr`, `steplog`.

## jit_cache.c

This is the most synchronization-sensitive piece. Preserve atomic memory orders, thread-local storage, `jit_lock` ownership/depth, pending frees and the distinction between retiring a block and flushing an arena. The code-index lookup and fault repair paths run in signal context: no allocation, locking, formatting, unwinding or Rust panic may be added there. Preserve `pthread_jit_write_protect_np`, instruction-cache synchronization, branch range checks and the stop/undo protocol. `js_note_fail` has its own decode recovery setjmp. Pointers handed to generated code (RAS cells, buckets, guards, counters) must remain stable until the existing reclamation protocol permits freeing them.

Existing external entry points:

`ocerz_jit_blocks`, `ocerz_jit_code_range`, `ocerz_jit_create`, `ocerz_jit_destroy`,
`ocerz_jit_fault_info`, `ocerz_jit_fault_pair`, `ocerz_jit_fault_recover_flags`,
`ocerz_jit_fault_recover_regs`, `ocerz_jit_fault_recover_xmm`, `ocerz_jit_fault_rip`,
`ocerz_jit_forget`, `ocerz_jit_guest_gprs_at`, `ocerz_jit_hotpatch_align`,
`ocerz_jit_invalidate_all`, `ocerz_jit_invalidate_range`, `ocerz_jit_lock_depth_ptr`,
`ocerz_jit_lock_held_self`, `ocerz_jit_lock_owner_cpu`, `ocerz_jit_note_align_fault`,
`ocerz_jit_note_commpage_fault`, `ocerz_jit_owner_pid`, `ocerz_jit_pc_in_arena`,
`ocerz_jit_postfork`, `ocerz_jit_postfork_child`, `ocerz_jit_prefork`, `ocerz_jit_prof_stats`,
`ocerz_jit_request_stop`, `ocerz_jit_require_ordered`, `ocerz_jit_step`, `ocerz_jit_thread_mark`,
`ocerz_jit_thread_restore`.

New cross-piece function exports:

`blk_chain_install`, `branch_word_target`, `build_fault_flag_recipes`, `cache_insert`,
`cache_lookup`, `chain_edge_now`, `churn_blacklisted`, `churn_note_refusal`,
`code_index_append_locked`, `compact_block`, `gran_block`, `jit_exec_one_parked`, `jl_acquire`,
`js_note_fail`, `pending_add`, `pending_add_ras`, `psc_alloc`, `psc_retire_cols`, `ras_slot_alloc`,
`veneer_pool_check`.

Shared state definitions owned here:

`g_cap_ras_cells`, `g_jl_log`, `g_jl_owner`, `g_jl_phase`, `g_jl_since`, `g_n_ras_cells`,
`g_ras_cells`, `g_ras_slot_n`, `g_ras_slots`, `jit_lock`, `jl_held`, `js_decoded_insns`,
`js_fail_alloc`, `js_fail_decode0`, `js_fail_overflow`, `js_xlat_fail`, `js_xlat_ok`,
`ocerz_jit_retire_count`, `ocerz_jit_retire_ns`, `ocerz_leaf_near_hi`, `ocerz_leaf_near_lo`,
`ocerz_perfstat`, `ps_align_patches`, `ps_chain_far`, `ps_chain_ok`, `ps_chain_veneer`,
`ps_ras_noslot`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| None; local or external-to-JIT consumers | `ocerz_jit_blocks`, `ocerz_jit_code_range`, `ocerz_jit_create`, `ocerz_jit_destroy`, `ocerz_jit_fault_info`, `ocerz_jit_fault_pair`, `ocerz_jit_fault_recover_flags`, `ocerz_jit_fault_recover_regs`, `ocerz_jit_fault_recover_xmm`, `ocerz_jit_fault_rip`, `ocerz_jit_forget`, `ocerz_jit_guest_gprs_at`, `ocerz_jit_hotpatch_align`, `ocerz_jit_invalidate_all`, `ocerz_jit_invalidate_range`, `ocerz_jit_lock_depth_ptr`, `ocerz_jit_lock_held_self`, `ocerz_jit_lock_owner_cpu`, `ocerz_jit_note_align_fault`, `ocerz_jit_note_commpage_fault`, `ocerz_jit_owner_pid`, `ocerz_jit_pc_in_arena`, `ocerz_jit_postfork`, `ocerz_jit_postfork_child`, `ocerz_jit_prefork`, `ocerz_jit_prof_stats`, `ocerz_jit_request_stop`, `ocerz_jit_require_ordered`, `ocerz_jit_step`, `ocerz_jit_thread_mark`, `ocerz_jit_thread_restore`, `ocerz_leaf_near_hi`, `ocerz_leaf_near_lo` |
| control | `jit_exec_one_parked` |
| control, core | `ocerz_perfstat` |
| control, core, flags, tcache | `cache_lookup` |
| control, core, tcache | `pending_add_ras`, `veneer_pool_check` |
| control, flags, tcache | `ocerz_jit_retire_count` |
| control, tcache | `psc_alloc`, `ras_slot_alloc` |
| core | `build_fault_flag_recipes`, `churn_blacklisted`, `churn_note_refusal`, `compact_block`, `js_decoded_insns`, `js_fail_alloc`, `js_fail_decode0`, `js_fail_overflow`, `js_note_fail`, `js_xlat_fail`, `js_xlat_ok`, `ps_align_patches`, `ps_chain_far`, `ps_chain_ok`, `ps_chain_veneer`, `ps_ras_noslot` |
| core, flags | `g_ras_slot_n` |
| core, flags, tcache | `g_n_ras_cells`, `g_ras_cells` |
| core, tcache | `blk_chain_install`, `cache_insert`, `code_index_append_locked`, `g_cap_ras_cells` |
| flags | `branch_word_target`, `chain_edge_now`, `g_jl_log`, `g_jl_owner`, `g_jl_phase`, `g_jl_since`, `gran_block`, `jit_lock`, `jl_acquire`, `jl_held`, `ocerz_jit_retire_ns`, `pending_add`, `psc_retire_cols` |
| flags, tcache | `g_ras_slots` |

Private file-scope state owned here:

`g_cap_psc_tables`, `g_chain_batching`, `g_chain_npatched`, `g_chain_patched`, `g_churn`,
`g_churn_suppress`, `g_flush_fail`, `g_flush_gen`, `g_flush_mark`, `g_flush_n`, `g_flush_req`,
`g_flush_retry_ns`, `g_flush_want`, `g_gblk`, `g_gblk_off`, `g_gran`, `g_gran4`,
`g_gran_degenerate`, `g_inv_caller`, `g_invsrc`, `g_jit_thr`, `g_jit_thr_key`, `g_jit_thr_once`,
`g_jl_acq`, `g_jl_owner_cpu`, `g_jl_rip`, `g_jl_waits`, `g_jl_xlat_null`, `g_n_psc_tables`,
`g_no_fault_recipes`, `g_pending`, `g_ps_atexit_jit`, `g_psc_cap`, `g_psc_pool`, `g_psc_tables`,
`g_psc_used`, `g_retire_sweep`, `g_trip_count`, `g_trip_jit`, `g_trip_tab`, `g_xlp`, `g_xlp_log`,
`js_ftab`, `js_ftab_full`, `js_ftab_used`, `js_hits`, `js_misses`, `js_steps`, `js_t0`, `js_xlat`,
`t_jit_thr`.

Helpers from this subsystem now inline in the shared header:

`blk_insn_full`, `blk_mode32`, `blk_rip`, `hash_key`, `jit_key`, `jit_key_mode32`, `jit_key_rip`,
`jl_release`, `psc_col`, `ras_cell_register`.

## jit_control.c

This contains both emitters and routines called directly *by emitted code*. Keep the exported C entry points and `noinline` boundaries of `ocerz_jit_exec_one*`: `__builtin_return_address(0)` identifies the fault/parking caller. The `preserve_most` trace/perf helpers remain private beside their only caller, so bindgen does not expose an unsupported ABI. Preserve the return-stack layout and every register-preservation rule at interpreter/native/leaf crossings. Slow calls can escape through the VM's longjmp; no Rust destructors may be live across them. A Rust port may need tiny C shims for return-address capture and these special calling conventions.

Existing external entry points:

`ocerz_jit_exec_one`, `ocerz_ras_push`.

New cross-piece function exports:

`emit_body_chain_tail`, `emit_bridge_fastcall`, `emit_call_ret`, `emit_chain_tail`,
`emit_const_lit`, `emit_dispatch_stub`, `emit_indirect_call`, `emit_indirect_jmp`, `emit_jmp`,
`emit_leaf_call_ret`, `emit_oolslow_arms`, `emit_push_pinned`, `emit_slowcall`, `fps_watch`,
`host_ras_enabled`, `jit_exec_one`, `leaf_layout_ok`, `low_stack_fast`, `ocerz_jit_exec_one_at`,
`ocerz_jit_exec_run_at`, `oolslow_add`, `stop_retarget`.

Shared state definitions owned here:

`g_body_entry`, `g_call_edge`, `g_callout_seq`, `g_chain_epi`, `g_chain_keeps_jgb`,
`g_chain_target`, `g_ea_plain`, `g_fps_frames`, `g_n_call_edges`, `g_n_oolslow`, `g_n_pe_real`,
`g_n_promo_real`, `g_n_push_fix`, `g_n_stop_extra`, `g_no_chain`, `g_no_compact`, `g_no_ras`,
`g_pe_insns`, `g_pe_real`, `g_promo_real`, `g_push_entry`, `g_push_fix`, `g_slow_run_last`,
`g_stop_extra`, `ocerz_jit_exec_state`, `ps_ops`, `ps_ras_miss`, `ps_ras_null`, `ps_ras_sentinel`,
`ps_ras_stale`, `ps_shape`, `ps_shapes`, `ps_slow_insns`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| cache | `jit_exec_one` |
| cache, core | `g_no_compact`, `g_pe_insns` |
| cache, core, flags, memory, tcache | `g_no_chain` |
| cache, core, tcache | `g_no_ras`, `low_stack_fast` |
| core | `emit_bridge_fastcall`, `emit_call_ret`, `emit_indirect_call`, `emit_indirect_jmp`, `emit_jmp`, `emit_leaf_call_ret`, `emit_oolslow_arms`, `fps_watch`, `g_body_entry`, `g_call_edge`, `g_chain_epi`, `g_chain_keeps_jgb`, `g_chain_target`, `g_fps_frames`, `g_n_call_edges`, `g_n_oolslow`, `g_n_pe_real`, `g_n_promo_real`, `g_n_stop_extra`, `g_pe_real`, `g_promo_real`, `g_stop_extra`, `host_ras_enabled`, `leaf_layout_ok`, `ocerz_jit_exec_state`, `ps_ops`, `ps_ras_miss`, `ps_ras_null`, `ps_ras_sentinel`, `ps_ras_stale`, `ps_shape`, `ps_shapes`, `ps_slow_insns`, `stop_retarget` |
| core, flags | `emit_body_chain_tail`, `emit_chain_tail` |
| core, flags, memory | `g_callout_seq` |
| core, fp, integer, memory, simd | `emit_slowcall` |
| core, integer | `emit_push_pinned`, `g_n_push_fix`, `g_push_entry`, `g_push_fix` |
| core, integer, memory | `g_ea_plain` |
| core, tcache | `emit_dispatch_stub`, `ocerz_jit_exec_one` |
| fp | `g_slow_run_last` |
| integer, memory, simd | `oolslow_add` |
| memory | `emit_const_lit` |
| tcache | `ocerz_jit_exec_one_at`, `ocerz_jit_exec_run_at`, `ocerz_ras_push` |

Private file-scope state owned here:

`g_dbg_ind_src`, `g_fps_start`, `g_ind_call_cont`, `g_ind_call_tocont`, `g_ind_m32`, `g_ind_treg`,
`g_oolslow`.

Helpers from this subsystem now inline in the shared header:

`emit_frame_sp_reset`, `emit_l0_flush_from`, `emit_l0_reload_from`, `emit_slowcall_keep_lanes`,
`emit_static_chain_tail`, `patch_any_branch`, `ras_body_only`, `ras_entry_for`, `stack_fast`.

## jit_flags.c

Very hot during translation and cross-block lookahead. Keep small state queries inline and preserve the NZCV/lazy-flags producer lifetime and exact transitions on slow exits. Multiple bounded lookahead/decode paths use `sigsetjmp`; reset/restore the previous recovery pointer, and do not introduce Drop-bearing frames. Preserve flip/probe state, dependencies, patch offsets and the saved flag recipe used by fault recovery. Do not turn signed conditions into unsigned comparisons or recompute flag masks at a different point in the instruction stream.

Existing external entry points: none.

New cross-piece function exports:

`can_fuse_cmp_test_jcc`, `canonical_body_successor`, `comis_fuse_producer`,
`decoded_call_region_entry`, `decoded_terminator`, `emit_arith_incdec_jcc`, `emit_cc_predicate_ex`,
`emit_cmp_test_jcc`, `emit_ifconv_diamond`, `emit_incdec_jcc`, `emit_jcc`,
`emit_logic_jmp_incdec_jcc`, `emit_materialize`, `emit_pf`, `flip_disabled`, `flip_retire_locked`,
`flip_side_hit`, `jcc_flip_wanted`, `nzcv_fuse_producer`, `side_fuse_ok`, `side_gap_fuse_ok`,
`superblock_back_enabled`, `superblock_enabled`, `value_cond_fuse_producer`, `xlive_decode_entry_d`.

Shared state definitions owned here:

`g_cc_cbz_nz`, `g_cc_cbz_reg`, `g_cc_cbz_sf`, `g_cc_want_cbz`, `g_flag_producer`, `g_flip`,
`g_flip_n_retire`, `g_jcc_edge`, `g_jcc_side_fall_need`, `g_jcc_side_mode`, `g_jcc_side_need`,
`g_loop_entry`, `g_n_jcc_edges`, `g_n_probes`, `g_n_side`, `g_no_jccfuse`, `g_no_jcclink`,
`g_no_regflags`, `g_no_xlive`, `g_self_rip`, `g_side`, `g_stop_patch`, `g_stop_target`, `g_tag_blk`,
`g_tag_idx`, `g_tc_ndlog`, `g_tc_rec`, `g_xlat_jit`, `g_xlat_mode32`, `g_xlive_log`,
`ocerz_jit_decode_recover`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| cache | `flip_retire_locked`, `flip_side_hit`, `g_flip_n_retire` |
| cache, control, core | `g_xlat_jit` |
| cache, control, core, tcache | `g_no_regflags` |
| cache, core | `g_no_jccfuse`, `g_no_jcclink`, `g_no_xlive` |
| cache, core, tcache | `ocerz_jit_decode_recover` |
| control, core | `g_jcc_edge`, `g_loop_entry`, `g_n_jcc_edges`, `g_self_rip`, `g_stop_patch`, `g_stop_target`, `g_tag_blk`, `g_tag_idx` |
| control, core, fp, integer, memory, simd | `g_xlat_mode32` |
| control, core, integer, memory | `emit_materialize` |
| core | `can_fuse_cmp_test_jcc`, `canonical_body_successor`, `decoded_call_region_entry`, `decoded_terminator`, `emit_arith_incdec_jcc`, `emit_cmp_test_jcc`, `emit_ifconv_diamond`, `emit_incdec_jcc`, `emit_jcc`, `emit_logic_jmp_incdec_jcc`, `flip_disabled`, `g_cc_cbz_nz`, `g_cc_cbz_reg`, `g_cc_cbz_sf`, `g_cc_want_cbz`, `g_flag_producer`, `g_flip`, `g_jcc_side_fall_need`, `g_jcc_side_mode`, `g_jcc_side_need`, `g_n_probes`, `g_n_side`, `g_side`, `g_xlive_log`, `jcc_flip_wanted`, `nzcv_fuse_producer`, `side_fuse_ok`, `side_gap_fuse_ok`, `superblock_back_enabled`, `superblock_enabled`, `value_cond_fuse_producer`, `xlive_decode_entry_d` |
| core, fp | `comis_fuse_producer` |
| core, fp, integer, memory | `emit_cc_predicate_ex` |
| core, tcache | `g_tc_ndlog`, `g_tc_rec` |
| integer | `emit_pf` |

Private file-scope state owned here:

`g_flip_atexit_jit`, `g_flip_n_hit`, `g_flip_ns_hit`, `g_flip_ns_retire`, `g_xlive_dbuf`,
`g_xlive_memo`.

Helpers from this subsystem now inline in the shared header:

`call_body_successor`, `can_fuse_incdec_jcc`, `emit_cc_predicate`, `emit_commit_flags`,
`emit_defer_flags`, `emit_prof_count`, `emit_side_tag`, `emit_zf_sf`, `flip_find`, `flip_state`,
`probe_wanted`, `side_stub_has_work`, `xlive_succ_live`, `xlive_succ_live_d`.

## jit_integer.c

A good early emitter port, but hot: use unchecked indexing only where the original C proved the same bounds, keep wrapping/truncation and immediate widths exact, and inline the pinned-GPR helpers from the header. The integer instruction selector must return the same success/fallback answer before or after touching the buffer. Bit operations are here; CRC remains with SIMD; push/pop emitters (including their memory forms) stay with the integer selector, while block-level stack-pair selection stays in core. Preserve scratch-register aliases and CPU-field offsets in emitted loads/stores.

Existing external entry points: none.

New cross-piece function exports:

`emit_adc_sbb`, `emit_add_inc_pair`, `emit_arith`, `emit_arith_mem`, `emit_arith_narrow`,
`emit_bitscan`, `emit_bswap`, `emit_bt`, `emit_cbw_cwd`, `emit_cmov`, `emit_cmp_test_narrow`,
`emit_div`, `emit_imul`, `emit_incdec`, `emit_lea`, `emit_leave`, `emit_mov_logic_pair`,
`emit_movsxd`, `emit_mul_wide`, `emit_not_neg`, `emit_pin_prologue`, `emit_push_pop`,
`emit_push_pop_mem`, `emit_rot`, `emit_setcc`, `emit_shift`, `emit_shift_cl`, `emit_shiftd`,
`fullpin_enabled`, `m32_lowreg_ok`, `rdx_prep_skippable`, `stack_inline_enabled`.

Shared state definitions owned here:

`g_cc_direct`, `g_cur_insn_start`, `g_defer`, `g_div_prev_skipped`, `g_m32low`, `g_mov_sink_at`,
`g_mov_skip`, `g_n_pinned`, `g_no_addincfuse`, `g_no_lazyflags`, `g_nzcv_from`, `g_nzcv_kind`,
`g_nzcv_want`, `g_oolslow_pre`, `g_pin`, `g_pin_class`, `g_pin_hold`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| cache | `g_no_addincfuse` |
| cache, control, core, tcache | `fullpin_enabled` |
| cache, core | `g_no_lazyflags` |
| cache, core, flags, fp, memory, simd | `g_defer` |
| control | `stack_inline_enabled` |
| control, core | `g_oolslow_pre` |
| control, core, flags | `g_m32low`, `g_n_pinned`, `g_pin_hold` |
| control, core, flags, fp, memory, simd | `g_pin`, `g_pin_class` |
| core | `emit_adc_sbb`, `emit_add_inc_pair`, `emit_arith_mem`, `emit_arith_narrow`, `emit_bitscan`, `emit_bswap`, `emit_bt`, `emit_cbw_cwd`, `emit_cmov`, `emit_cmp_test_narrow`, `emit_div`, `emit_imul`, `emit_incdec`, `emit_leave`, `emit_mov_logic_pair`, `emit_movsxd`, `emit_mul_wide`, `emit_not_neg`, `emit_pin_prologue`, `emit_push_pop`, `emit_push_pop_mem`, `emit_rot`, `emit_setcc`, `emit_shift`, `emit_shift_cl`, `emit_shiftd`, `g_cur_insn_start`, `g_div_prev_skipped`, `m32_lowreg_ok`, `rdx_prep_skippable` |
| core, flags | `emit_arith`, `emit_lea`, `g_cc_direct` |
| core, flags, memory | `g_nzcv_from` |
| core, fp | `g_mov_skip` |
| core, memory | `g_nzcv_want` |
| flags, memory | `g_nzcv_kind` |
| fp | `g_mov_sink_at` |

Private file-scope state owned here:

`g_no_inline_imul`.

Helpers from this subsystem now inline in the shared header:

`body_edge_pin_class`, `emit_fill_pinned`, `emit_fill_pinned_callersaved`, `emit_gpr_rd`,
`emit_gpr_wr`, `emit_pin_epilogue_restore`, `emit_spill_pinned`, `emit_spill_pinned_callersaved`,
`m32_stack_base_ok`, `m32_stack_ld`, `m32_stack_low`, `m32_stack_ok`, `m32_stack_st`, `pin_hreg`,
`pin_saved_count`, `pin_slot`.

## jit_memory.c

Very hot and correctness-sensitive under Wine/low-shadow layouts and multi-threaded guests. Preserve ordered-vs-plain memory selection, alignment granules (not merely natural alignment), segment bases, address-size wrapping, signed displacements and the guard-elision caches. Guard sites feed cache/fault recovery and tcache; their instruction forms, offset units and register assignments are contracts, not implementation details. Keep address arithmetic wrapping and avoid per-instruction allocation or bounds checking.

Existing external entry points:

`g_cur_insns_fwd`, `ocerz_jgb_trap`.

New cross-piece function exports:

`ea_cache_usable`, `emit_cmpxchg8b`, `emit_commpage_guard`, `emit_gpr_ld_at`, `emit_gpr_lds_at`,
`emit_gpr_st_at`, `emit_guard_arms`, `emit_guest_load_ordered`, `emit_guest_store_ordered`,
`emit_low_hoist_bail`, `emit_low_hoist_check`, `emit_mem_ea`, `emit_mem_ea_plain_ex`,
`emit_mem_load_plain`, `emit_mov_mem`, `emit_movx`, `emit_ordered_slow_arms`, `emit_plain_mem_fast`,
`emit_reload_mem_base`, `emit_rmw_mem`, `emit_v_ld_at_`, `emit_v_st_at`, `emit_xchg_reg32`,
`insn_may_write_gpr`, `low_guard_fast_ok`, `lowstack_delta_ok`, `lowstack_disp_ok`,
`lowstack_disturbs`, `select_low_hoist`, `stack_identity`, `stack_plain_ok`, `vec_tso_relaxed`.

Shared state definitions owned here:

`g_al_all`, `g_al_marks`, `g_al_n`, `g_align_guard`, `g_blk_ordered_loads`, `g_cp_guard`,
`g_cp_marks`, `g_ea_cache`, `g_ea_is_const`, `g_ea_lowhoisted`, `g_fpbmap`, `g_low_hoist_greg`,
`g_low_top`, `g_lowhoist_marks`, `g_lowstack`, `g_mem_hoist_aux_disp`, `g_mem_hoist_aux_index`,
`g_mem_hoist_aux_scale`, `g_mem_hoist_greg`, `g_mem_hoist_greg2`, `g_mem_hoist_greg3`, `g_n_fpbmap`,
`g_n_garm`, `g_n_low_hoist_bail`, `g_n_oslow`, `g_no_ldapr`, `g_no_oolslow`, `g_plain_mem`,
`g_undo_saved`, `g_undo_want_size`, `g_undo_want_slot`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| cache | `g_no_ldapr`, `g_no_oolslow` |
| cache, control, core, flags, fp, integer | `g_cp_guard` |
| cache, control, core, flags, fp, integer, simd, tcache | `g_plain_mem`, `stack_plain_ok` |
| cache, core | `g_al_all`, `g_al_marks`, `g_al_n`, `g_cp_marks` |
| control, core, flags | `emit_reload_mem_base` |
| control, core, flags, fp, integer, simd | `emit_guest_load_ordered` |
| control, core, flags, integer | `g_lowstack` |
| control, core, flags, integer, simd | `emit_commpage_guard` |
| control, core, flags, simd | `g_ea_cache` |
| control, core, fp, integer, simd | `emit_guest_store_ordered` |
| control, core, integer | `stack_identity` |
| control, core, integer, simd | `emit_plain_mem_fast` |
| control, flags, integer, simd | `emit_mem_ea` |
| control, integer | `low_guard_fast_ok` |
| core | `emit_cmpxchg8b`, `emit_guard_arms`, `emit_low_hoist_bail`, `emit_low_hoist_check`, `emit_mov_mem`, `emit_movx`, `emit_ordered_slow_arms`, `emit_rmw_mem`, `emit_xchg_reg32`, `g_align_guard`, `g_blk_ordered_loads`, `g_ea_is_const`, `g_ea_lowhoisted`, `g_fpbmap`, `g_low_hoist_greg`, `g_low_top`, `g_mem_hoist_aux_disp`, `g_mem_hoist_aux_index`, `g_mem_hoist_aux_scale`, `g_mem_hoist_greg`, `g_mem_hoist_greg2`, `g_mem_hoist_greg3`, `g_n_fpbmap`, `g_n_garm`, `g_n_low_hoist_bail`, `g_n_oslow`, `lowstack_delta_ok`, `lowstack_disturbs`, `select_low_hoist` |
| core, flags | `insn_may_write_gpr` |
| core, fp, simd | `emit_v_ld_at_`, `emit_v_st_at`, `g_undo_saved`, `g_undo_want_size`, `g_undo_want_slot` |
| core, tcache | `ocerz_jgb_trap` |
| flags | `ea_cache_usable`, `g_lowhoist_marks`, `lowstack_disp_ok` |
| flags, fp, integer, simd | `emit_gpr_ld_at` |
| flags, integer, simd | `emit_mem_load_plain` |
| fp, integer, simd | `emit_mem_ea_plain_ex` |
| fp, simd | `emit_gpr_st_at`, `vec_tso_relaxed` |
| integer | `emit_gpr_lds_at`, `g_cur_insns_fwd` |

Private file-scope state owned here:

`g_const_ea`, `g_const_ea_valid`, `g_ea_const`, `g_ea_lowhoisted_reg`, `g_ea_w32`, `g_garm`,
`g_low_hoist_bail`, `g_low_hoist_hi`, `g_low_hoist_lo`, `g_low_hoist_until`, `g_oslow`.

Helpers from this subsystem now inline in the shared header:

`a64_word_may_write_reg`, `al_mark`, `al_marked`, `cp_mark`, `cp_marked`, `ea_cache_has_base`,
`ea_cache_reset`, `ea_cache_set_full`, `ea_cache_step`, `ea_fold`, `emit_add_const`,
`emit_mem_ea_plain`, `emit_misaligned_pieces_st`, `emit_reload_jgb`, `emit_stack_delta`,
`emit_stack_delta_check`, `emit_stack_delta_into`, `emit_v_ld_at`, `hoist_signature`, `jgb_usable`,
`lowhoist_mark`, `lowstack_disp_ea`, `mark_add`, `mark_has`, `mem_fast_forms_ok`,
`mem_guard_needed`, `mem_native_store_ok`, `mem_plain_access_ok`, `patch_guard_skip`,
`stack_guard_needed`, `stack_plain_access_ok`, `stack_plain_now`, `undo_save_hook`.

## jit_simd.c

Instruction-selection-heavy and a useful independent port after header helpers are available. Preserve lane width, upper-YMM zeroing, NaN/sign behavior, legacy-vs-VEX distinctions and pinned-XMM interactions. It calls FP's lane/batch/NaN machinery rather than owning a second copy. Exact arm64 instruction words and emitter ordering matter: a numerically equivalent Rust rewrite is not the goal. Retain all unsupported-instruction fallbacks.

Existing external entry points: none.

New cross-piece function exports:

`emit_crc32`, `emit_mmx`, `emit_pmovmskb`, `emit_sse`, `emit_sse_mem_addr`, `emit_vex`,
`emit_ymmh_clear`, `sse_enabled`, `xmm_global_enabled`, `xmm_pinning_enabled`.

Shared state definitions owned here:

`g_blk_ymm_write`, `g_cmps_mask_idx`, `g_cur_need`, `g_fpb_fast`, `g_n_raslit`, `g_raslit`,
`g_sse_mem_disp`, `g_sse_mem_plain`, `g_sse_mem_plainacc`, `g_sse_mem_ra`, `g_vec_int_move`,
`g_xmm_pinned`, `g_ymmh_zero`, `g_zero_vreg`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| control, core | `g_ymmh_zero` |
| control, core, flags, fp | `g_xmm_pinned` |
| control, core, flags, memory | `g_n_raslit` |
| control, core, fp | `xmm_global_enabled` |
| control, core, memory | `g_raslit` |
| core | `emit_crc32`, `emit_mmx`, `emit_pmovmskb`, `emit_sse`, `emit_vex`, `g_blk_ymm_write`, `g_cmps_mask_idx`, `g_cur_need`, `g_fpb_fast`, `g_zero_vreg`, `xmm_pinning_enabled` |
| core, flags, fp | `sse_enabled` |
| core, fp, integer | `emit_sse_mem_addr`, `g_sse_mem_disp`, `g_sse_mem_plainacc`, `g_sse_mem_ra` |
| fp | `emit_ymmh_clear` |
| fp, integer | `g_sse_mem_plain` |
| memory | `g_vec_int_move` |

Private file-scope state owned here:

`g_vex_mem_skip`.

Helpers from this subsystem now inline in the shared header:

`emit_mem_load_any`, `emit_sse_mem_ld`, `emit_sse_mem_ld_gpr`, `emit_sse_mem_st`,
`emit_sse_mem_st_gpr`, `emit_xmm_pin_load_all`, `emit_xmm_pin_spill_all`, `xmm_is_pinned`,
`xmm_vreg`.

## jit_fp.c

The most stateful emitter port. FP batches and x87 intentionally share one piece: splitting them further duplicates the lane/recovery contracts and creates more hot cross-file calls. Preserve TOP/tag/exception state, NaN fixups, undo masks, dirty lanes, deferred exactness tests and the points that fall back to the interpreter. `g_l0*`, `g_yc*`, `g_fpb*` and x87 scratch arrays are single mutable instances, not per-port caches. Fault handlers consume emitted lane-recovery tables; do not reorder their entries or change CPU offsets. Although this piece has no direct setjmp site, its slow paths can escape through control/VM recovery.

Existing external entry points: none.

New cross-piece function exports:

`cmps_blendv_fusable`, `emit_mov128_pair`, `emit_nan_fix_packed2`, `emit_nan_fix_scalar2`,
`emit_nan_ool_arms`, `emit_x87`, `emit_x87_arms`, `fpb_emit_check`, `fpb_emit_regs_check`,
`fpb_emit_store_check`, `fpb_scan_v1`, `fpb_scan_v2`, `fpb_v1`, `l0_alloc2`, `l0_defer`,
`l0_enabled`, `l0_fixed_restore`, `l0_fixed_setup`, `l0_pre_insn`, `lanerec_note`,
`mov_sink_gap_ok`, `mov_sink_scan`, `unsafe_nocheckbr`, `vex_cmps_blendv_pair`, `vex_lane_aware`,
`x87_run_flags`, `yc_setup`.

Shared state definitions owned here:

`g_cur_blk`, `g_cur_insn_idx`, `g_cur_insns`, `g_cur_insns_n`, `g_fcmp_self_idx`,
`g_fcmp_self_vreg`, `g_fpb`, `g_fpb_det`, `g_fpb_exit_batch`, `g_fpb_exit_end`, `g_fpb_exit_mask`,
`g_fpb_member`, `g_fpb_of`, `g_fpb_open`, `g_fpb_sidechk`, `g_fpb_sites`, `g_fpb_stchk`,
`g_fpb_stlane`, `g_fpb_undo`, `g_fpb_undo_done`, `g_fpb_undo_from`, `g_fpb_undo_ld`,
`g_fpb_undo_ldst`, `g_fpb_undo_ldsz`, `g_fpb_undo_size`, `g_fpb_v1_active`, `g_keep`, `g_keep_n`,
`g_l0`, `g_l0_dbl`, `g_l0_dirty`, `g_l0_fixed`, `g_l0_fixed_dbl`, `g_l0_fixed_dirty`,
`g_l0_fixed_lane`, `g_l0_next`, `g_l0_nlanes`, `g_l0_owners`, `g_lane_used`, `g_lanerec`, `g_n_fpb`,
`g_n_fpb_sites`, `g_n_lanerec`, `g_n_nanool`, `g_n_undo_lanes`, `g_n_x87_frag`, `g_n_x87_run`,
`g_n_x87_site`, `g_nanool`, `g_pk_consts_needed`, `g_scalar_merge_next`, `g_scpend`, `g_undo_vreg`,
`g_x87_btop`, `g_x87_cur`, `g_x87_delta`, `g_x87_frag_open`, `g_x87_kcarry`, `g_x87_lane`,
`g_x87_lanes_on`, `g_x87_live`, `g_x87_lv`, `g_x87_nzcv`, `g_x87_spec`, `g_x87_spec_cut`, `g_yc`,
`g_yc_dirty`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| cache, control, core | `g_keep`, `g_keep_n` |
| cache, control, core, memory | `g_cur_blk` |
| cache, core, flags, integer, memory, simd | `g_cur_insns` |
| cache, core, integer, simd | `g_cur_insns_n` |
| control, core, flags | `fpb_emit_regs_check`, `g_fpb_exit_batch`, `g_fpb_exit_mask`, `g_l0_fixed`, `g_pk_consts_needed` |
| control, core, flags, integer, memory, simd | `g_cur_insn_idx` |
| control, core, flags, simd | `g_fpb_sites`, `g_l0`, `g_l0_dbl`, `g_l0_dirty`, `g_n_fpb_sites`, `g_yc`, `g_yc_dirty` |
| control, flags | `g_fpb_exit_end`, `l0_fixed_restore` |
| core | `emit_mov128_pair`, `emit_nan_ool_arms`, `emit_x87`, `emit_x87_arms`, `fpb_emit_check`, `fpb_emit_store_check`, `fpb_scan_v1`, `fpb_scan_v2`, `fpb_v1`, `g_fpb`, `g_fpb_member`, `g_fpb_sidechk`, `g_fpb_stchk`, `g_fpb_stlane`, `g_fpb_undo`, `g_fpb_undo_done`, `g_fpb_undo_from`, `g_fpb_undo_ld`, `g_fpb_undo_ldst`, `g_fpb_undo_ldsz`, `g_fpb_undo_size`, `g_fpb_v1_active`, `g_l0_fixed_dbl`, `g_l0_fixed_dirty`, `g_l0_fixed_lane`, `g_l0_next`, `g_l0_nlanes`, `g_lanerec`, `g_n_fpb`, `g_n_lanerec`, `g_n_undo_lanes`, `g_n_x87_frag`, `g_n_x87_run`, `g_n_x87_site`, `g_x87_btop`, `g_x87_cur`, `g_x87_delta`, `g_x87_frag_open`, `g_x87_kcarry`, `g_x87_lane`, `g_x87_lanes_on`, `g_x87_live`, `g_x87_lv`, `g_x87_nzcv`, `g_x87_spec`, `g_x87_spec_cut`, `l0_fixed_setup`, `l0_pre_insn`, `lanerec_note`, `mov_sink_scan`, `x87_run_flags`, `yc_setup` |
| core, flags | `g_lane_used` |
| core, flags, simd | `g_fpb_of`, `g_fpb_open`, `l0_enabled` |
| core, memory, simd | `g_undo_vreg` |
| core, simd | `emit_nan_fix_scalar2`, `g_fcmp_self_idx`, `g_l0_owners`, `g_n_nanool`, `g_scalar_merge_next`, `g_scpend` |
| flags, simd | `g_fpb_det` |
| integer | `mov_sink_gap_ok` |
| simd | `cmps_blendv_fusable`, `emit_nan_fix_packed2`, `g_fcmp_self_vreg`, `g_nanool`, `l0_alloc2`, `l0_defer`, `unsafe_nocheckbr`, `vex_cmps_blendv_pair`, `vex_lane_aware` |

Private file-scope state owned here:

`g_fpb_disabled`, `g_fpb_live`, `g_fpb_marith`, `g_fpb_mmem`, `g_fpb_mrd`, `g_fpb_mstore`,
`g_fpb_mwr`, `g_fpb_stdbl`, `g_x87_c1k`, `g_x87_fcmov_static`, `g_x87_fr0`, `g_x87_fr1`,
`g_x87_frag`, `g_x87_nzcv_live`, `g_x87_rc_near`, `g_x87_run`, `g_x87_site`, `g_x87_st_mark`,
`g_x87_tagk`, `g_x87_xokk`.

Helpers from this subsystem now inline in the shared header:

`emit_pk_consts_load`, `fpb_det_here`, `fpb_emit_exit_check`, `fpb_emit_undo_restore`,
`fpb_emit_undo_save`, `fpb_replay_prelude`, `fpb_scan`, `fpb_site_emit`, `fpb_undo_clear`,
`l0_defer_take`, `l0_fixed_backedge`, `l0_fixed_fallthrough`, `l0_fixed_map`, `l0_flush_all`,
`l0_flush_reg`, `l0_inval`, `l0_reset`, `l0_share`, `l0_src2`, `lane_reserve`, `scalar_cvt_follows`,
`scalar_pend_flush`, `x87_inline_ok`, `x87_reset`, `yc_flush_all`, `yc_flush_from`, `yc_reload_all`.

## jit_tcache.c

A smaller natural seam, deliberately not padded with unrelated emitters. Preserve all record widths, relocation kinds/forms, relative offsets, fingerprints, instruction-pool compaction and persisted-cache version checks. Bindgen types are mandatory for `TcRec`, `TcReloc`, `JitBlock` and associated layouts. Decode validation uses `sigsetjmp`; no unwinding or Drop-bearing frames. `tc_value` resolves addresses of helpers in other pieces, which must remain real exported symbols rather than per-TU inline copies. Use off/roundtrip/verify/read/write cache modes as separate oracles. Repacking a serialized record requires a separate cache-version change, not part of a faithful port.

Existing external entry points:

`ocerz_jit_tcache_final`.

New cross-piece function exports:

`jit_decode`, `tc_bind`, `tc_load`, `tc_noload_add`, `tc_put`, `tc_roundtrip`, `tc_summary`,
`tc_verify`.

Shared state definitions owned here:

`g_tc_bad`, `g_tc_dbar`, `g_tc_dbytes`, `g_tc_dlog`, `g_tc_entry`, `g_tc_key`, `g_tc_learned`,
`g_tc_lf`, `g_tc_log`, `g_tc_nbytes`, `g_tc_noload`, `g_tc_noload_cap`, `g_tc_noload_n`,
`g_tc_nrel`, `g_tc_on`, `g_tc_pool_off`, `g_tc_rel`, `ocerz_jitstat`.

| Other JIT pieces referencing the symbol | Exported symbols |
| --- | --- |
| None; local or external-to-JIT consumers | `ocerz_jit_tcache_final` |
| cache, core | `ocerz_jitstat` |
| cache, flags | `tc_noload_add` |
| control, core, flags, fp, memory | `g_tc_bad`, `g_tc_entry`, `g_tc_nrel`, `g_tc_on`, `g_tc_rel` |
| core | `g_tc_key`, `g_tc_lf`, `g_tc_log`, `g_tc_noload`, `g_tc_noload_cap`, `g_tc_noload_n`, `g_tc_pool_off`, `tc_bind`, `tc_load`, `tc_put`, `tc_roundtrip`, `tc_summary`, `tc_verify` |
| core, flags | `g_tc_dbar`, `g_tc_nbytes`, `jit_decode` |
| core, flags, memory | `g_tc_learned` |
| flags | `g_tc_dbytes`, `g_tc_dlog` |

Private file-scope state owned here:

`g_tc_mbytes`, `g_tc_mdep`, `g_tc_n_bad`, `g_tc_n_const`, `g_tc_n_full`, `g_tc_n_load`,
`g_tc_n_mism`, `g_tc_n_ok`, `g_tc_n_pcrel`, `g_tc_n_put`, `g_tc_n_rej`, `g_tc_n_stale`,
`g_tc_n_vbad`, `g_tc_n_vok`, `g_tc_n_vvar`, `g_tc_opos`, `g_tc_out`, `g_tc_val`.

Helpers from this subsystem now inline in the shared header:

`tc_imm64`, `tc_keepable`, `tc_key`, `tc_log_init`, `tc_noload_has`, `tc_note`, `tc_usable`.

## Verification and timing

The baseline and initial split verification used all `make check` phases from
`docs/testing.md`, run individually so existing failures did not prevent later
phases from running. The split was then rebased onto the Rust scaffold at
`379c1e7` and verified with its full `bash tools/rust_gate.sh`, followed by an
explicit i386 rerun after removing stale pre-port C objects. The only shared
integration edit is adding `jit_internal.h` to the scaffold's explicitly listed
`rust/wrapper.h`; no bindgen blocklist or build-rule changes are needed.
Baseline and split use
the same VM, SDK, environment and fixtures. `OCERZ_NO_ARM_EXEC=1` is used for unit
harnesses, as in the Makefile.

| Gate | Monolith baseline | Split |
| --- | --- | --- |
| C build and JIT warning-as-error syntax checks | builds | builds |
| Unit programs | 28/29; two `test_apidb` fixture failures | same |
| Guests, interpreted | 134/134 | 134/134 |
| Guests, JIT | 134/134 | 134/134 |
| x86-64 differential | 100/100 | 100/100 |
| i386 differential, offset and low-shadow, JIT required | 40,044/40,044 per layout; selftest passes | same |
| Dynamic | 280 passed, 7 failed | same seven failures |
| Native | 86 passed, 1 failed | same failure |
| Guest libraries | pass | pass |
| Native frameworks | pass | pass |
| Native format | pass | pass |
| Native C++ and native Swift | skipped: guest runtimes absent | same skips |

The unchanged failures are: `test_apidb` expects macOS 27.0 API files while this
VM's SDK generator produces 26.5 files; dynamic `ddlopen_image_list` (both
engines), `ddlopen_cryptex` (both), `dyldslots`, `dmetal_nocopy_low` (both); and
native `sys_proc`, whose arm64 host oracle fails its own spawn/exec checks.
No test expectations, SDK fixtures or unrelated modules were changed to hide
these failures. A `make check` invocation therefore stops at the pre-existing
API fixture failure; this is not an all-green-environment claim.

After rebasing an existing C checkout, remove stale `.o` files for ported
modules: the i386 runner globs all `src/*.o`, including obsolete C objects that
would collide with Rust exports. Its first post-rebase invocation hit this for
flags/globals. The gate's FAIL-line scan did not detect that linker exit; inspect
phase exit statuses and success summaries, not only the gate's final PASS line.
No changes to the shared gate or expected failure lists are part of this split.

Clang accepts the internal header under `-Wall -Wextra -Werror` (with the
project's unused-parameter exception). Bindgen 0.71.1 generates its declarations
with an allowlist for project headers; rustc compiles the generated bindings and
layout assertions. The scaffold's bindgen 0.72/nightly build also succeeds.
All 200 named record layouts shared with the monolith match
Clang's complete record-layout dump. The two private `preserve_most` diagnostic
helpers do not require a bindgen blocklist or an ABI override.

All object definitions were checked with `nm` for duplicate globals and new
JIT symbol collisions with the SDK's libSystem exports.

### Emitted-code comparison

In addition to both differentials, temporary instrumented builds of the
monolith and split record every freshly emitted block immediately before
`int tc_save = 0` in `translate`, using `OCERZ_TCACHE=roundtrip`. Each record is
`{u64 rip, u32 code_words, i32 nrel}`, followed by all arm64 words and each
relocation's `{u32 off, u8 kind, u8 form}`. Only process-dependent relocation
payloads are normalized: form-1 two-word pointer literals are zeroed; for form-0
four-instruction MOVZ/MOVK sites only the imm16 bits (`0x1fffe0`) are masked.
Opcode and destination-register bits, all other words, instruction counts and
relocation descriptions are compared exactly. Literal address bytes cannot be
identical across independently linked/ASLR-loaded binaries. This initial audit
used temporary source copies. The reusable oracle above adds a compile-time
disabled hook; no instrumentation is included in production binaries.

The recorded offset corpus has 107,635 blocks and 72,747,100 arm64 words; the
low-shadow corpus has 107,660 blocks and 72,776,425 words. Both files compare
byte-for-byte after that address-only normalization. SHA-256:

- Offset: `f9f85e9629c8de4e2031e6f741b69e5319c439308a2a6ec1e79fa3ff1a76ece6`
- Low shadow: `8259582646b20b7f4cdbd1c66da28fc2052cd4b0814c3a9b9d5c74a9a00c5d45`

### Wall-clock measurements

The full `bash tests/run_diff32.sh .` took 57.51 s wall clock for the monolith
and 53.92 s for the final split after rebasing onto the Rust scaffold. This
includes compiling the harness, selftest and both memory layouts. That final
script run also includes the scaffold's flags/globals ports; the C-only paired
runs below isolate this JIT split from those unrelated changes.
A separate alternating-order comparison of the final
compiled `--jit-required` harness (four runs each, same offset corpus) gave
19.741 s monolith versus 19.383 s split, a 1.8% reduction.

The final alternating-order run additionally enabled the existing
`ocerz_jit_time_xlat` counter in a temporary constructor, without changing either
production binary. Medians across four runs per build:

| Measurement | Monolith | Split | Change |
| --- | ---: | ---: | ---: |
| i386 whole harness | 19.954 s | 19.797 s | -0.8% |
| Time inside `translate` | 591.841 ms | 586.504 ms | -0.9% |

For dynamic fixtures, both executables ran the same x86-64 Mach-O fixtures with
`OCERZ_TCACHE=off`; no persisted translations were reused. These are medians of
six alternating-order batches, 30 fresh processes per batch (180 runs per
build/fixture). Batch wall time is divided by 30:

| Dynamic fixture | Monolith | Split | Change |
| --- | ---: | ---: | ---: |
| `tcache_work` | 30.128 ms | 30.434 ms | +1.0% |
| `avx_fp` | 32.776 ms | 31.361 ms | -4.3% |
| `low_hoist` | 40.432 ms | 39.166 ms | -3.1% |

The short-process numbers include loader/scheduler noise and are not instruction
throughput microbenchmarks. The 0.307 ms `tcache_work` difference is within the
observed batch spread; there is no material translator-speed regression in these
measurements. Timings compare the monolithic C binary against the split C binary
before the unrelated Rust scaffold's flags/globals switch-over.
