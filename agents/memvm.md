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
