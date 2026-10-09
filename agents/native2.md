# native2 porting notes

Modules owned by agent `native2`: objcguard.s, sysbridge.c, objcclass.c,
bridge.c, objcbridge.c. Notes on what the Rust port does differently than a
naive reading of the C, and the evidence collected for each switch-over.

## objcguard.s -> rust/src/ported/objcguard.rs

- Ported as `core::arch::global_asm!` with the `.s` verbatim: same
  `_ocerz_objc_guarded` in `__TEXT,__text`, same `_ocerz_objc_guard_pad`
  pointer in `__DATA,__const`, same CFI including
  `.cfi_personality 155, _ocerz_objc_guard_personality`. The personality and
  `_ocerz_objc_guard_landed` still live in `src/objcbridge.c` (referenced as
  `U` from the Rust lib until that module is ported).
- Verified identical unwind info: `dwarfdump --eh-frame` on the C and Rust
  binaries gives the same FDE instruction stream, and the CIE personality GOT
  slot rebases to `_ocerz_objc_guard_personality` in both
  (`dyld_info -fixups`).
- `tests/unit/bin/test_objcbridge` with `OCERZ_NO_ARM_EXEC=1`: 117606 checks
  passed.
- Gotcha: `tests/run_diff32.sh` links `ls src/*.o` directly, so a stale
  `src/objcguard.o` (or any ported module's `.o`) produces a duplicate-symbol
  link failure that the gate then counts as "ok" because the log has no FAIL
  lines. Always `rm -f src/<ported>.o src/<ported>.d` after a port lands and
  read the diff32/i386 logs before trusting a gate run.

## sysbridge.c -> rust/src/ported/sysbridge/

Layout: `mod.rs` carries the C file's opening prose as `//!` and one private
submodule per C section, each keeping its `---- section ----` prose as `//!`:
`common` (shared helpers), `files` (open family), `fcntl` (fcntl/ioctl/sem/shm/
ulimit), `int128` (128-bit arith + float fixes), `fenv`, `mem` (mmap + mach/vm),
`jmp` (setjmp/longjmp), `proc` (fork/exec/spawn/system/popen), `threads`
(pthread keys/create/exit + dispatch_main), `misc` (glob, stacks, pagesize,
sysctl, sandbox).

- All 88 `ocerz_sys_*` exports verified present in `libocerz_rs.a` and the
  linked `ocerz` (`nm` count 88, and `dyld_info -exports` shows all 88
  exported for the bridge's dlsym lookup).
- Host variadic `__asm__` renames became private externs with `#[link_name]`:
  `open$NOCANCEL`, `openat$NOCANCEL`, `fcntl$NOCANCEL` (all confirmed `U` in
  the archive), plus `environ`, `strsep`, `fwide`, `ulimit`, `sandbox_check`,
  `mach_vm_region`, `mach_vm_remap`, `open/openat_dprotected_np`,
  `guarded_open_np`, `guarded_open_dprotected_np` — none of these are bound by
  the libc crate (or bindgen).
- `__builtin_frame_address(0)` is a `#[inline(never)]` fn reading x29 with a
  `nomem, nostack, preserves_flags` asm move.
- `sb_fix_signed`/`sb_fix_unsigned` keep C's explicit NaN/signbit/range
  branches; the 0x1p127/0x1p128 limits are `f64::from_bits` constants since
  Rust has no hex-float literal.
- `GLOB_ALTDIRFUNC` is not in the libc crate; defined as `0x0040` (SDK value).
- The `SbKey` tables are `#[repr(C)]` structs of `AtomicI32`/`AtomicU64`
  (seq_cst, matching C's default atomics); `g_sb_key_next` is a `static mut`
  behind a real `pthread_mutex_t` initialized with
  `PTHREAD_MUTEX_INITIALIZER`. `sb_thread_main` is a private `extern "C"` fn;
  malloc/free are used where C uses them; no Drop types on fork/exec/longjmp
  paths.
- `iocl`'s IOC_IN/IOC_OUT are private in the libc crate; redefined as
  `0x80000000`/`0x40000000` (`c_ulong`). `libc::stderr` does not exist —
  `crate::log::stderr()` (extern `__stderrp`) is used instead.
- `ocerz_guest_vm_*` take the task port as `u64`, not `u32` — pass `sb_arg`
  raw.
- Verification: `make -j12 ocerz` clean; test_bridge 1245 checks, 0 failed;
  fast gate pass (diff32 log inspected: 40044 passed, 107410 blocks, no link
  errors); full gate pass except the known `datomic_counter-no-jit` flake
  (30s timeout under load, passes standalone in 28.8s).
- Gotcha: a mutable C file-scope static must be `static mut` in Rust, and
  locks must be passed `&raw mut`. An immutable `static` of a C struct type
  (e.g. `libc::pthread_mutex_t = PTHREAD_MUTEX_INITIALIZER`) is placed in
  `__TEXT,__const`, so the first `pthread_mutex_lock` write faults (SIGBUS in
  `_pthread_mutex_lock_init_slow`). Check with `nm -m ocerz | grep <module>` —
  every writable object must be in `__DATA` (`__data`/`__bss`/`__common`),
  nothing mutable in `__TEXT,__const`. Statics of atomic types are fine as
  `static` because the compiler gives them interior-mutable placement.
  `G_SB_KEY_LOCK`, `G_SB_SYSTEM_LOCK`, `G_SB_POPEN_LOCK` hit exactly this;
  a guest calling `pthread_key_create` or `system()` crashed pre-fix.
- Perf vs the pure-C reference `~/AArchX-c/ocerz` (which needs `make apis`
  run there first — `runtime/apis` is resolved relative to the executable,
  so a copied binary needs a `runtime/` beside it or a symlink). Guests
  built with `clang -arch x86_64 -O2`, run as `./ocerz -native <bin>` 5
  times each, `/usr/bin/time -p` wall seconds:

  | guest | work | C reference min/median | Rust tip min/median |
  |-------|------|----------------------|---------------------|
  | jmp   | 2M setjmp/longjmp | 0.09 / 0.10 | 0.09 / 0.09 |
  | div128 | 2M unsigned __int128 div+mod | 0.06 / 0.06 | 0.06 / 0.06 |
  | mmap  | 50k mmap+munmap | 0.16 / 0.17 | 0.16 / 0.17 |

  Parity within timer noise, as expected — sysbridge is not a hot loop.

## objcclass.c -> rust/src/ported/objcclass/

Layout: `mod.rs` carries the C file's opening prose as `//!`; `common.rs` has
the mem.h inlines (copied from sysbridge's private copies), the oc_* word
readers, the OC_* flag constants, the lazy `OCERZ_OBJCLOG` check (an
`AtomicU64` instead of C's racy `static int`) and the `oc_stop!` macro_rules!
replacement for the variadic `_Noreturn` function (fputs prefix + variadic
fprintf + newline + fflush + `exit(OCERZ_BRIDGE_UNIMPL_EXIT)`; the format is
`concat!($fmt, "\n\0")` — a `&str` pointer is NOT NUL-terminated and fprintf
overreads it into a SIGSEGV, which the unit gate caught). `read.rs` holds the
guest-metadata readers (its `//!` is the "class stays where the guest put it"
+ "methods" prose) and `define.rs` the host-runtime definer (OcSym table,
maps, dead IMPs, protocol/class/category definition, ensure/prepare/swift,
define_image, run_loads; its `//!` is the remaining prose blocks).

- All 15 `ocerz_objc_*` readers and 11 `ocerz_objcbridge_*` definer exports
  verified `T` in libocerz_rs.a and exported in `ocerz`.
- OcSym is `#[repr(C)] { name: *const c_char, addr: AtomicPtr<c_void> }` +
  `unsafe impl Sync`, names via `concat!($name, "\0")`, SeqCst load/store
  through `ocerz_bridge_host_symbol(OCERZ_OBJC_LIBOBJC, ...)`, and each runtime
  call transmutes the address to an `unsafe extern "C" fn` of the exact
  signature (bool where C passes _Bool).
- `g_oc_lock` is `static mut pthread_mutex_t = PTHREAD_MUTEX_INITIALIZER`
  (`&raw mut`); the five OcMaps, `g_oc_loads`/`_n`/`_cap`, `g_oc_defining` are
  `static mut`; `g_oc_dead` is `AtomicPtr<OcDead>` SeqCst. `nm -m ocerz` shows
  every writable object in `__DATA,__bss`/`__data` — nothing in `__TEXT,__const`.
- `oc_dead_imp` is a private `extern "C" fn` (handed to the runtime as an IMP);
  `oc_order_cmp` is `extern "C"` for qsort (libc crate wants `Some(cmp)`).
  OcDead's flexible `why[]` is `[c_char; 0]` with `malloc(size_of + len + 1)`.
- `ocerz_abi_round_swap`/`_of_mxcsr` are `static inline` in abi.h — private
  copies (`mrs/msr fpcr`, `nomem, nostack, preserves_flags`). `mach_vm_read_
  overwrite` and the mach-o loader structs are private decls/repr(C) structs
  (libc lacks them).
- longjmp finding: +load runs through `ocerz_vm_call` -> `vm_call_core`, whose
  `sigsetjmp`/`siglongjmp` recovery point lives *inside* the C callee
  (src/vm.c:3582), so a guest fault never unwinds a Rust frame — Rust frames
  sit strictly above the jump target. Drop-free observed anyway.
- Gotcha (new): every `&str`/`String` pointer handed to C must be a `c"..."`
  literal or explicitly `\0`-terminated as `*const c_char`; `"...".as_ptr()`
  on a Rust string has no NUL and fprintf reads past it (upstream fix
  f9753d8 hit this in the shared log macros too — a stray `:/:` line broke
  the compat framework check on the tip, fixed by the rebase).
- Verification: `OCERZ_NO_ARM_EXEC=1 test_objcbridge` 123753 checks 0 failed
  (this exercises define_image incl. the lock); test_bridge 1245/0,
  test_callback 132015/0, test_attach 270/0; fast gate pass; full gate pass
  including native framework tests (all jit/interpreter/slow-bridge suites).
- Perf: class definition runs at image load. Guests (clang -arch x86_64):
  `many` = 2000 classes (~2-3 methods + a property each, 200 protocols, 200
  categories) touching a handful at exit; `frameworks.x86_64` = the native
  framework suite's own binary. `./ocerz -native <bin>` x5, wall seconds:

  | guest | C reference min/median | Rust tip min/median |
  |-------|----------------------|---------------------|
  | many (2000 classes) | 0.02 / 0.02 | 0.02 / 0.02 |
  | frameworks.x86_64 | 0.05 / 0.05 | 0.05 / 0.06 |

  Parity — definition work is dominated by the host runtime, not the reader.
