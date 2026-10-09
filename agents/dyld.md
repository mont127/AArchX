# dyld and dyldapi Rust ports

`src/dyldapi.c` is replaced at the C ABI by `rust/src/ported/dyldapi/`. The exported functions retain their C names and signatures, including `g_main_path`. At the dyldapi landing, `src/dyld.c` remained C; its Rust port is documented below.

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

## dyldapi tip breakages

The full gate on detached tip commit `ff3e398` also times out in `datomic_counter-no-jit` (exit 124, expected output `OK`). The current and tip dynamic failure lists are identical. The native `sys_proc` arm64 host fixture also fails on the tip; neither failure is attributed to this port.

## dyld Rust port

`src/dyld.c` is replaced at the C ABI by `rust/src/ported/dyld/`. All 38 text exports found in the pristine C object are present in `ocerz`; `ocerz_main_mh` is exported as a data symbol. `g_main_path` remains owned by dyldapi.

### Layout

- `mod.rs` holds shared image state, file and slice selection, and image queries.
- `map.rs` maps segments and applies protections.
- `exports.rs` handles ULEB/SLEB decoding, export tries, symbol lookup, and generation.
- `bind.rs` resolves imports and applies chained/classic fixups and legacy relocations.
- `frame.rs` builds the guest stack frame and native exit stub.
- `eager.rs` indexes dependencies and computes eager loads.
- `tlv.rs` implements cache and native TLV state and address lookup.
- `init.rs` handles initializer marks, closures, and load phases.
- `load.rs` resolves paths and rpaths, loads disk dependencies, and canonicalizes paths.
- `dlopen.rs` implements cache-mode dlopen/dlsym/dlclose/dlerror and piggyback loading.
- `native.rs` implements native dyld APIs, image queries, unwind sections, and per-thread error state.
- `run.rs` implements `ocerz_dyld_run`, inserted libraries, and native-image minimum-OS handling.

### Deviations and invariants

The libc Darwin `pthread_mutex_t` fields are private in the pinned libc version. The recursive load mutex is therefore initialized from its 64-byte C ABI representation, with `_PTHREAD_RECURSIVE_MUTEX_SIG_init` (`0x32aaaba2`) in the signature word; compile-time size assertions and the dlopen smoke cover the layout and use. `ndl_err!` formats into the native thread's error buffer through `snprintf`, preserving the C format strings without Rust C variadics.

TLV descriptors retain the C packed layout and first-touch behavior; the full native gate passes `tlv_main`, `tlv_bss`, `tlv_layout`, `tlv_threads`, `tlv_thread_churn`, and `tlv_dylib`. Guest-memory, VM, and callback paths use plain data and libc allocation; the dyld modules contain no `Vec`, `Box`, or `String`. `ocerz_main_mh` belongs to dyld, while `g_main_path` remains defined by dyldapi.

### Verification

- `make -j12 ocerz` and all unit-binary builds succeeded.
- Export parity: 38/38 C dyld text exports found in `ocerz`; `_ocerz_main_mh` is present.
- The six cache/native smoke cases (`ls`, `sw_vers`, and `plutil -help`) matched the pristine-C tree byte-for-byte in stdout/stderr and exit status. Native `plutil -help` exits 71 in both trees.
- The dynamic `dlopen_cf` smoke returned `OK` in both trees. The `native_cxx.cpp` TLS smoke returned 0 with identical output in both cache-mode trees, including `native_cxx threads tls futures ok`.
- `dlopen_image_list` produced the same two baseline failures in both trees; the full-gate failure files also match the detached-tip baseline.
- Fast gate: guest no-JIT 134/0, guest JIT 134/0, diff 100/0, and diff32 40,044/0. The diff32 log records real differentials translating 107,479 and 107,410 blocks.
- Full gate: dynamic 280/7 and native 86/1; `dyn.fail`, `run_native_tests.fail`, and `unit.fail` are byte-identical to the detached-tip logs. The only native failure is the host `sys_proc` fixture. The full gate reports no new failures.
- Gate logs: `/tmp/rust_gate/`; detached-tip reference: `/Users/devin/rust_gate_tip_dyld_9595b42/` and `/Users/devin/tip_gate_dyld_9595b42.txt`.
- Left in C: nothing.

### Startup benchmarks

`DYLD_BENCH_N=15 bash tools/bench/dyld_startup.sh` was run twice after the full gate completed; the gate and benchmark did not overlap.

Run 1 (best / median milliseconds):

| mode | case | AArchX best / median | AArchX-c best / median |
|---|---|---:|---:|
| cache | `/bin/echo hi` | 33.9 / 36.8 | 29.6 / 33.5 |
| cache | `/bin/ls /` | 36.0 / 40.5 | 35.8 / 39.8 |
| cache | `/usr/bin/sort /etc/hosts` | 33.2 / 37.1 | 33.6 / 38.8 |
| cache | `/usr/bin/sw_vers` | 358.0 / 364.9 | 350.6 / 359.9 |
| cache | `/usr/bin/plutil -help` | 311.9 / 320.4 | 311.4 / 320.4 |
| native | `/bin/echo hi` | 7.9 / 8.9 | 7.5 / 8.8 |
| native | `/bin/ls /` | 9.3 / 11.4 | 9.4 / 10.6 |
| native | `/usr/bin/sort /etc/hosts` | 9.8 / 12.0 | 9.5 / 10.9 |
| native | `/usr/bin/sw_vers` | 15.7 / 18.0 | 16.1 / 18.0 |
| native | `/usr/bin/plutil -help` | 89.4 / 92.4 (rc=71) | 91.2 / 96.7 (rc=71) |

Run 2 (best / median milliseconds):

| mode | case | AArchX best / median | AArchX-c best / median |
|---|---|---:|---:|
| cache | `/bin/echo hi` | 29.0 / 31.6 | 30.0 / 32.4 |
| cache | `/bin/ls /` | 37.2 / 40.4 | 36.6 / 39.8 |
| cache | `/usr/bin/sort /etc/hosts` | 35.0 / 37.7 | 33.0 / 36.5 |
| cache | `/usr/bin/sw_vers` | 343.2 / 353.0 | 340.7 / 353.0 |
| cache | `/usr/bin/plutil -help` | 304.4 / 312.6 | 294.6 / 306.8 |
| native | `/bin/echo hi` | 6.7 / 7.8 | 7.7 / 8.6 |
| native | `/bin/ls /` | 9.4 / 11.0 | 8.8 / 10.9 |
| native | `/usr/bin/sort /etc/hosts` | 10.0 / 11.5 | 9.6 / 10.7 |
| native | `/usr/bin/sw_vers` | 15.8 / 16.7 | 15.3 / 16.7 |
| native | `/usr/bin/plutil -help` | 86.5 / 92.6 (rc=71) | 88.9 / 92.5 (rc=71) |

The two native `sort` medians were each more than 5% slower in the first two samples. An additional 35 alternating paired runs measured medians of 10.97 ms for AArchX and 11.35 ms for AArchX-c (paired median difference −0.56 ms, −4.9% relative to C); the apparent regression did not reproduce.
