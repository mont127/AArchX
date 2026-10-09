# dyldapi Rust port

`src/dyldapi.c` is replaced at the C ABI by `rust/src/ported/dyldapi/`. The exported functions retain their C names and signatures, including `g_main_path`. `src/dyld.c` remains C.

## Layout

- `hostmem.rs` contains guest/host address conversion and unaligned guest-memory helpers.
- `macho.rs` contains the Mach-O layouts, constants, and size assertions used by the shim.
- `closure.rs` handles cache closure membership, cache paths, PC ranges, images, and symbols.
- `memfn.rs` indexes function starts and answers the JIT's in-place routine queries.
- `objc.rs` contains Objective-C callbacks, optimization tables, selector hashing, and image-load handling.
- `dispatch.rs` contains setup, lazy loading, API returns, and vtable dispatch.

Guest-memory and VM-facing paths use plain data rather than Rust drop types. Mach-O parsing and lookup paths preserve the C bounds, ordering, and unchecked hot-path access patterns. `ocerz_vdylib_dispatch` is declared locally because the generated FFI bindings do not expose it.

The selector hash tail uses the byte-16 shift from the C implementation (8 bits). A selector verification run after that correction completed without a mismatch.

## Shared logger correction

The port's verbose paths exposed that `rust/src/log.rs` passed `concat!(...).as_ptr()` directly to C `fprintf` without a terminating NUL. The macros now append `"\0"` before passing the format string. This was a minimal shared-file fix: before it, verbose cache logs could be corrupted, `sys_strings_inplace` counted only eight log lines, and `./ocerz -v -cache /usr/bin/sw_vers` crashed in `vfprintf`; afterward the same `sys_strings` fixture counted nine distinct entries and the explicit-cache `sw_vers` run exited 0.

## Verification

- Build and unit binaries succeeded; `nm ocerz | grep -c ' T _ocerz_dyldapi'` returned 12.
- All six translated/native smoke comparisons (`ls`, `sw_vers`, and `plutil -help`) matched the pristine-C reference; `OCERZ_SELVERIFY=1 ./ocerz /usr/bin/sw_vers` exited 0.
- Fast-gate phases passed: guest no-JIT 134/0, guest JIT 134/0, diff 100/0, and diff32 40,044/0. The diff32 log reports real differentials translating 107,480 and 107,410 blocks. There is no separate i386 log; `diff32.log` is the i386-counterpart phase.
- The full gate completed with dynamic 279/8 and native 86/1. `sys_strings_inplace` now passes at 9; the native `sys_proc` host fixture still fails its own checks. The dynamic failure lists at current and tip are identical; `datomic_counter-no-jit` times out at exit 124 in both. The gate flags that baseline timeout because it is not yet in `agents/expected/`. Full logs: `/tmp/rust_gate/`; detached-tip logs: `/tmp/rust_gate_tip/`.

## Startup benchmark

`DYLD_BENCH_N=15 bash tools/bench/dyld_startup.sh` compares this tree with `/Users/devin/AArchX-c`, with translation caching disabled. First run (best / median milliseconds):

| mode | case | AArchX best/median ms | AArchX-c best/median ms |
|---|---|---:|---:|
| cache | `/bin/echo hi` | 30.6 / 33.6 | 33.2 / 36.7 |
| cache | `/bin/ls /` | 38.8 / 42.6 | 37.8 / 44.0 |
| cache | `/usr/bin/sort /etc/hosts` | 34.4 / 39.2 | 35.8 / 40.3 |
| cache | `/usr/bin/sw_vers` | 341.6 / 356.3 | 341.3 / 370.6 |
| cache | `/usr/bin/plutil -help` | 307.7 / 335.8 | 309.7 / 329.5 |
| native | `/bin/echo hi` | 7.9 / 9.1 | 8.3 / 10.7 |
| native | `/bin/ls /` | 8.7 / 10.8 | 9.3 / 11.3 |
| native | `/usr/bin/sort /etc/hosts` | 12.8 / 14.7 | 11.4 / 13.4 |
| native | `/usr/bin/sw_vers` | 17.3 / 19.2 | 17.3 / 21.3 |
| native | `/usr/bin/plutil -help` | 92.2 / 103.2 (rc=71) | 98.7 / 106.9 (rc=71) |

A second 15-run sample changed direction on the apparent regressions: cache medians (AArchX / C) were 35.9 / 32.0 for echo, 42.0 / 39.0 for ls, 38.7 / 36.7 for sort, 367.9 / 347.0 for `sw_vers`, and 331.3 / 325.5 for `plutil`; native medians were 8.5 / 9.7, 11.8 / 10.0, 12.0 / 13.8, 20.0 / 19.2, and 99.1 / 102.4, respectively. A 15-run comparison of the unmodified tip against C also showed >3% differences in both directions. The outliers did not repeat consistently, so no performance change was made.

## Tip breakages

The full gate on detached tip commit `ff3e398` also times out in `datomic_counter-no-jit` (exit 124, expected output `OK`). The current and tip dynamic failure lists are identical. The native `sys_proc` arm64 host fixture also fails on the tip; neither failure is attributed to this port.
