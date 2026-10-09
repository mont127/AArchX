# mem.c -> rust/src/ported/mem.rs (owner: memvm)

Full port of src/mem.c (2342 lines) as a single module. All 51 exported
symbols are `#[unsafe(no_mangle)]` with the exact mem.h signatures; `nm -g` on
`ocerz` exports every one, and every global symbol in the reference
`src/mem.o` (~/AArchX-c) resolves.

## Notes / gotchas

- The mach APIs are not in `libc`, so the module declares
  `mach_vm_allocate/deallocate/region/region_recurse/remap` and
  `mach_task_self_` itself (the `mach_task_self()` C macro reads
  `mach_task_self_`). The two vm_region info structs are private `repr(C)`
  copies whose sizes and field offsets are pinned by `const` asserts against
  values measured with a C probe on this SDK (submap_short = 48 B,
  basic = 36 B, protection/max_protection at 0/4).
- `region_n` is an `AtomicI32`, `g_armed_live` an `AtomicI64`; slot and
  bitmap word accesses go through `AtomicU32`/`AtomicU8::from_ptr` with the
  same orderings as the C `__atomic_*` calls. Plain C accesses stayed plain.
  `__atomic_thread_fence` -> `fence(Ordering::Release)` (the only fences in
  mem.c are Release).
- `ocerz_pin_map` is stored via `AtomicPtr::from_ptr` Release, matching the C
  publish store; C readers load it non-atomically exactly as before.
- `g_init_released` (C `volatile int`) uses `read_volatile`/`write_volatile`.
- Function-local `static`s (`lg`, `once`, `init`, `probe`, `lowlog`, `v`,
  `dis`, `off`, `rlog`, `rn`, the pin `map`, the hole-fill `lock`) are
  module-level `static mut`/atomics with the same laziness.
- `ocerz_current_guest_rip/rsp` (vm.c, not in a header) and
  `ocerz_jit_lock_held_self` are declared in private `unsafe extern "C"`
  blocks, as in mem.c.
- The mem.h `static inline` helpers mem.c uses (`ocerz_g2h`,
  `ocerz_pinned_page`) are private `#[inline(always)]` copies in the module,
  byte-for-byte faithful (wrapping_sub where the C relies on unsigned
  wraparound). Not added to inline.rs.
- All address math is u64 with `wrapping_*` where C relies on wraparound;
  `for_unpinned` keeps the exact pin-skip logic; hot loops use raw-pointer
  indexing.
- `atexit(armstat_dump)` -> `libc::atexit` with an `extern "C" fn`.
- `ocerz_fatal!`/`ocerz_log!` mirror OCERZ_FATAL/OCERZ_LOG (which do not
  exit); plain `fprintf(stderr, ...)` stays `libc::fprintf` with identical
  format strings.

No semantic deviations intended.

## Perf (see ~/memvm-perf-baseline.md for the pre-port numbers)

| measurement | C (~/AArchX-c) | Rust port |
|---|---|---|
| guest suite, jit (`tests/run_guest_tests.sh`) | 1.76-1.78 s | 1.95 s |
| guest suite, no-jit | 4.75-4.96 s | 5.72 s |
| dynamic suite | 131.7-136.4 s | 145.4 s |
| microbench: 200x map_anywhere(1 GB)+unmap, best of 5 | 0.0925 s | 0.0953 s |
| microbench: 1e6 ocerz_addr_prot, best of 5 | 0.0019 s | 0.0019 s |

Bench program: ~/memvm-bench/membench.c (binaries membench-c / membench-rust,
linked like the unit tests: CORE_OBJS + libocerz_rs.a for the rust tree,
CORE_OBJS incl. src/mem.o for the C tree).

## vm.c (4376 lines) -> rust/src/ported/vm/{mod,run,sig,prof}.rs

Split: `mod.rs` holds the file-scope state, the private Mach/ucontext
declarations, the small helpers (str_into/hex_into, g2h/h2g/ld/st, leaf_site),
the cpu registry, suspend/resume, the riphist/recov rings, the reporting
functions (bt/exc/arg/sel/ctx traps), watch/peek/dump accessors,
mirror_host_signal and the atfork hooks. `run.rs` holds the setjmp trampoline,
vm_init/install_handlers/peek_dump, the guest-call core (CallCtx +
vm_call_body), run_cpu (RunCtx + run_cpu_body + run_fatal), thread
attach/detach/guest_stack, the unstick watchdog, request_exit, vm_fatal_where
and vm_run. `sig.rs` holds ripdump/threaddump/portdump handlers, the async
signal handler, the kick handler and the ~1200-line crash_handler.
`prof.rs` holds the guest profiler tables, sampling thread and final report.

- `sigsetjmp` (two sites) is called through a module-private `global_asm!`
  trampoline `_ocerz_vm_setjmp_run` (15 instructions, same shape as the lead
  design note): pre-setjmp state lives in a `#[repr(C)]` ctx struct on the
  outer frame (`CallCtx`/`RunCtx`), the code after setjmp is an
  `unsafe extern "C" fn` body that the trampoline calls with the setjmp rc.
  Teardown ordering matches C. NOTE: the asm block must carry
  `.section __TEXT,__text` — without it the symbol linked into
  `__DATA_CONST,__const` and every setjmp site SIGBUS'd (found via
  test_callback, bisected with a standalone driver).
- `ocerz_jit_decode_recover` is `__thread` in jit_flags.c — it must be
  declared `#[thread_local]` in the module extern block, not a plain
  `static mut`. Declared plainly it reads a different address and the
  crash handler siglongjmps into garbage: every fault/signal/jit guest test
  died with host SIGILL (rc=132). `ocerz_jit_exec_state` (jit_control.c) is
  also `#[thread_local]` extern. Do not trust the bindgen `static mut` decls
  for these two.
- ucontext/mcontext: private `repr(C)` types in mod.rs, layout const-asserted
  against SDK-measured values (ucontext_t 56B mcsize@40 mcontext@48;
  mcontext64 816B es@0 ss@16 ns@288; thread_state 272B sp@248 pc@256
  cpsr@264; esr@8; fpsr@512; sigjmp_buf 196B; siginfo_t 104B via libc).
- All C `__thread`s are `#[thread_local] static mut` (g_cur_cpu,
  g_pending_async_mask [volatile => AtomicU32 SeqCst/Relaxed], g_sig_recover,
  t_jit_escape_r, g_recov_ring, g_riphist/_n, g_attached, fn-local
  depth/last_alias_page/retry_addr/retry_n, CRASH_DEPTH).
- `ocerz_leaf_site` (leaf.h static inline) and the mem.h helpers are private
  `#[inline(always)]` copies; leaf_near_lo/hi read via Acquire atomics.
- Handlers keep the exact C sigaction flags and async-signal-safe bodies
  (write(2)+str_into/hex_into, no alloc).
- Mach calls used by vm.c (thread_get/set_state, mach_vm_read_overwrite,
  mach_vm_region, mach_port_names/get_attributes/get_set_status/peek,
  dladdr, malloc_zone_malloc, _dyld_get_image_header/slide, sysctlbyname,
  clock_gettime_nsec_np — the last two not bound by libc 0.2.190) are
  declared in the module extern block with repr(C) info structs
  (vm_region_basic_info_data_64, mach_port_status, mach_port_info_entry
  trailer, thread_state_64/basic_info) and const size asserts.
- Every C-bound string is a `c"..."` literal (fprintf formats, getenv names,
  str_into/arg strings); no `"...".as_ptr()` anywhere.
- `mrs tpidrro_el0` -> `core::arch::asm!("mrs {0}, tpidrro_el0")`.
- `__typeof__` swaps -> `ptr::swap`; `__builtin_expect` -> plain branch.
- `ocerz_jit_time_xlat`/`xlat_ns`/`retire_ns` are plain externs (not TLS).

Verification: all 47 `nm -g ~/AArchX-c/src/vm.o` globals resolve in `ocerz`;
direct units test_attach/test_callback(132015)/test_syscall/test_mem/
test_shared_map/test_interp{,32}/test_jit{,32}/test_jit_exit/
test_jit_order_transition/test_jit_psc_invalidate/test_wow64/test_loader/
test_bridge/test_objcbridge/test_chain_concurrency/test_blocks all pass.
Known non-regression still observed: dtest_jcc_gap_low -jit/-no-jit prints
`faults 59` vs golden 60, identical on the pristine C tree (5/5 runs).

vm post-landing fixes (found by the full gate's native fault-report tests):
- `mach_task_self_` is a global `mach_port_t`, not a function — declaring it
  `fn` made every mach call jump into data (nested fault mid-report).
- `ocerz_jit_decode_recover` is `__thread`; the extern must be
  `#[thread_local]` or the crash handler siglongjmps to garbage (host SIGILL).
- Bounds checks must not exist in the crash handler: `ji.host_holds[i]` and
  `ss.x[21+i]` with `n_pinned` up to 16 panicked mid-report and swallowed the
  `SIGNH deliver` line. Use `get_unchecked` wherever C indexes raw arrays in
  signal/fault paths.
- The `global_asm` setjmp trampoline needs `.section __TEXT,__text` first or
  it lands in `__DATA_CONST` and every setjmp site SIGBUSes.
- Static audit: `~/memvm/vm-static-audit.md`. One width fix (`long peek` ->
  c_long, non-atomic); every other static matches C exactly.

Perf (alternating trees, quiet machine, vs ~/AArchX-c):
guest no-jit x3: 6.03 6.67 6.29 (Rust) vs 6.60 6.51 6.17 (C)
guest jit    x3: 1.99 2.38 2.42 (Rust) vs 2.05 2.61 2.14 (C)
dynamic      x2: 145.05 136.90  (Rust) vs 143.23 145.00 (C)
Parity within noise on all three suites.
