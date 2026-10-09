# syscall.c -> rust/src/ported/syscall/ (owner: devin-9aa5caef)

Full port of src/syscall.c (8906 lines) as a directory module. Every global
symbol of the reference `src/syscall.o` (~/AArchX-c) is exported by the Rust
module with the exact include/ocerz/syscall.h signature (43 functions plus
the `g_ocerz_deliver_src` global); every C `static` stays private. The C
file's opening prose is the `//!` doc of `mod.rs`.

## Layout

| file | C lines | contents |
|---|---|---|
| mod.rs | 1-592 | shared constants/types, sysret flag helpers, ret_ok/ret_err/mach_ret, ocerz_is_wqthread_exit |
| raw.rs | sys_raw.h | `ocerz_host_syscall`/`ocerz_host_mach_trap` (`svc #0x80`) |
| util.rs | mem.h/cpu.h inlines | private copies of g2h/h2g/ld/st/ld128/st128, guest-space tests, `env_set!` |
| mem.rs | 593-1028 | mmap/munmap/mprotect/madvise and the guest_* exports |
| sysctl.rs | 1029-1377 | sysctl/sysctlbyname/mac_syscall |
| workers.rs | 1378-1690 | guest worker threads, cpu numbering, fork child |
| spawn.rs | 1691-2357 | posix_spawn/execve/fork, launchd plist rewrite, mock keychain |
| hostwq.rs | 2358-2667, 3373-3894 | host workqueue bridge, kevent/workloop callbacks |
| machmsg.rs | 2668-3372 | mach_msg translation, OOL descriptors, timebase |
| threads.rs | 3895-4118 | bsdthread_create/register/terminate |
| signals.rs | 4119-4822, 4961-5141 | sigaction, delivery frames, sigreturn, native sigtramp, waiters |
| bsd.rs | 4823-4960, 5142-6477 | BSD helpers, bsd_table, dispatch_bsd/dispatch_bsd_at |
| mach.rs | 6478-8337 | trap names, alias registry, MIG relocation, guest_vm_*, dispatch_mach |
| ldt.rs | 8338-8552 | LDT table and exports, ocerz_ldt_call, dispatch_machdep |
| entry.rs | 8553-8906 | sysring, bigring, write/page traps, ocerz_handle_syscall |
| machabi.rs | - | SDK `mach/message.h` descriptor layouts that bindgen does not emit |

## Notes / gotchas

- `raw.rs` keeps sys_raw.h's register convention exactly: x0-x7 from `a[0..8]`,
  x16 the number (negated for Mach traps), `svc #0x80`, then `cset cs` for
  the carry; x0/x1 are inout, the carry is `lateout`. Rust `asm!` assumes
  flags and memory are clobbered, matching the C's `"cc", "memory"`.
- bindgen does not emit `mach_msg_descriptor_t` or the
  `mach_msg_*_descriptor_t` bitfield structs. machabi.rs has packed
  `repr(C)` copies, with sizes, offsets and bitfield bit positions
  measured by a C probe on SDK 26.5 (12/12/16/16/16 bytes, union 16).
- `bsd_table` is a 600-entry static with the C's designated-initializer
  mapping. 375 entries were diffed mechanically against the C (index, name,
  nargs, ptr_mask, dual_ret, interceptor). Names are `\0`-terminated byte
  strings typed `*const c_char`. The Mach trap name table (37) and the
  dispatch_mach/dispatch_machdep case sets were checked the same way.
- dispatch_mach mirrors the C switch case by case; every arm calls
  `mach_ret` with the full u64 host x0, as the C does. An earlier draft
  joined the arms at one common return that truncated x0 to 32 bits. Don't
  "simplify" it back.
- Diagnostics: every `OCERZ_*` env knob of the C exists with the same
  number of call sites (the audit counts string literals). Caches have the
  C's shape: function-local `static int x = -1` becomes a `static mut` or a
  relaxed atomic of the same width and sentinel, read once. Nothing on the
  per-syscall path calls getenv after the first call. Watch FDOPLOG_EXE: an
  empty `ocerz_cmdline_summary` leaves the C's cache at -1, which means
  logging ON.
- Guest memory is re-read wherever the C re-reads it (no caching of
  `ocerz_ld`/`ocerz_addr_readable` results across C statements), since
  another guest thread can change it in between.
- Statics: all 173 C file-scope/function-local/`__thread` statics were matched
  against their Rust counterparts for width, signedness, initializer,
  array length, atomicity and TLS placement. The 7 `__thread` variables are
  `#[thread_local]`.
- Hot path (ocerz_handle_syscall -> dispatch_bsd_at -> ret_*/sysring_*): raw
  pointer indexing (the sysring stays `at % 24` on a raw pointer), no bounds
  checks, no allocation.
- `mach_vm_region`/`mach_vm_region_recurse` are declared with exactly
  the signatures in ported/mem.rs. Rust warns on clashing extern
  declarations across modules, so keep the two in sync.
- Strings handed to C, the guest or C-read tables are `c"..."` literals or
  explicitly `\0`-terminated.

## Verification

Full `tools/rust_gate.sh` on b120ccd + the local port: GATE PASS, no new
failures. Units all rc=0 except test_apidb (2 macOS-27 fixtures);
test_syscall 367/367, test_wow64 and test_shared_map pass. Guest 134/134 in
both modes, diff 100/100, diff32 40,044/0, dynamic 280 passed + 7 listed in
agents/expected/dyn.fail, native 86 + sys_proc. Library/framework/format
suites pass; C++/Swift skipped (guests not built).

Perf: `tools/bench/syscall_bench.sh` runs an x86-64 guest loop of 500k calls
through ocerz's syscall boundary and subtracts a zero-iteration run. Times are
ns/call. Ran after the gate on an idle VM, two runs each, C = ~/AArchX-c:

| workload | C run1 | Rust run1 | C run2 | Rust run2 |
|---|---|---|---|---|
| getpid (BSD success) | 206.5 | 200.6 | 199.6 | 196.4 |
| badwrite (BSD error) | 230.1 | 226.0 | 224.8 | 219.9 |
| gtod (intercepted) | 215.1 | 213.3 | 206.2 | 204.4 |
| machself (Mach trap) | 210.4 | 206.7 | 204.6 | 202.0 |

The Rust boundary is 1-3% faster on every path in both runs. Run-to-run spread
is about 3%, so read this as "not slower" rather than a real speedup. Nothing
is left in C.
