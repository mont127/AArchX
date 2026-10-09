# Runtime ports

## a64emit

Ported the AArch64 emitter exports to `rust/src/ported/a64emit.rs`, retaining the C ABI, buffer overflow/sink behavior, and patch semantics. Both C and Rust index the five-entry opcode tables in `a64_frint_s` and `a64_v_frint` without bounds checks; the benchmark constrains `mode` to 0–4, since out-of-range inputs invoke C undefined behavior.

Run `bash tools/bench/a64emit_bench.sh` to compare against `~/AArchX-c`. Both runs covered all 256 prototypes and reported `HASH MATCH` (`a92b607b1b78480d`).

| Run | Metric | C ns/call | Rust ns/call |
|---|---|---:|---:|
| 1 | `a64_mov_imm64` | 7.263 | 7.152 |
| 1 | four `a64_try_*_imm` | 3.976 | 3.974 |
| 1 | random mix | 8.742 | 9.239 |
| 2 | `a64_mov_imm64` | 6.852 | 7.164 |
| 2 | four `a64_try_*_imm` | 3.992 | 3.791 |
| 2 | random mix | 9.147 | 8.709 |

The audit follow-up retained unchecked table access. The repeat benchmark covered all 256 prototypes and reported `HASH MATCH` (`a92b607b1b78480d`): C/Rust ns per call were 6.678/6.653 for `a64_mov_imm64`, 3.768/3.545 for four `a64_try_*_imm`, and 9.090/8.272 for the random mix.

## stack

Ported initial guest stack construction to `rust/src/ported/stack.rs`, preserving the C layout, allocation failures, log format, and guest-memory stores. The stack imports `ocerz_g2h` and `ocerz_st` from `crate::inline`; the unused sysbridge re-export was removed, while sysbridge children continue importing their local `common` module. No stack-layout deviations. Stack setup runs once per process and is not a hot path, so no benchmark was needed.

Verification: `make -j12 ocerz` passed with a single `T _ocerz_setup_stack` and no `src/stack.o`; `OCERZ_NO_ARM_EXEC=1 tests/unit/bin/test_loader` passed (54 checks, 0 failures). The `-v` args guest log matched C (`argc=4 envc=39 unixthread`, 16-byte-aligned RSP, `stack_hi-rsp=0x900`). `bash tools/rust_gate.sh --fast` passed; `diff32` ran 40,044 sequences with 0 failures and translated 107,410 JIT blocks.

## loader

Ported `ocerz_load_image` to `rust/src/ported/loader.rs`, preserving the C loader's fat-slice selection, Mach-O validation, entry-point discovery, segment mapping/protection, cleanup, and log output. Reused the shared Mach-O records and added private, size-asserted fat/entry-point mirrors. This is a once-per-exec path, so no benchmark was needed.

Verification: `make -j12 ocerz` passed with one `T _ocerz_load_image` and no `src/loader.o`; `OCERZ_NO_ARM_EXEC=1 tests/unit/bin/test_loader` passed (54 checks, 0 failures). For `tests/guest/bin/args`, C and Rust emitted identical segment and loaded-image lines. Malformed-input comparisons also matched stderr and exit codes: 736-byte truncated x86_64 guest (65), `/etc/hosts` (64), and arm64e-only fat wrapper (64). The full gate passed with no new failures; `diff32` ran 40,044 sequences with 0 failures and translated 107,410 JIT blocks, and native framework tests passed. After the static-width and unchecked-index audit fixes, the fast gate also passed; `diff32` again ran 40,044 sequences with 0 failures and translated 107,410 blocks. `test_a64emit` passed all encodings, and the audit benchmark reported `HASH MATCH`.

## cache

Ported `src/cache.c` to `rust/src/ported/cache/`, split into `map`, `index`,
`resolve`, and `weak`. The port retains cache mapping and slide rebasing, lazy
fault handling, cache-region/watch state, image-index publication,
export-trie and re-export resolution, breadth-first resolution, memo tables,
and weak-export filtering. It uses `ffi::OcerzCache`, C allocation and pthread
mutex APIs, and the original fixed-size stack work arrays. Signal-handler
paths use raw pointer access and atomics without allocation or
bounds-checked indexing; `cmap_find` and `ocerz_cache_region` preserve the
wrapping range test and linear scan.

Static-state audit (C declaration → Rust representation):

| State | C declaration | Rust representation |
|---|---|---|
| lazy map | `g_lazy[16]`: `u64,u64,u32,const u8*,u64,int,u8*,int,u64`; `g_n_lazy: int` | `LazyRegion[16]` with matching field widths/pointers; `c_int` count |
| lazy lock/retry/options | volatile `int` lock; `__thread uintptr_t`; function-local `int` options initialized to `-1` | `AtomicI32` acquire/release spin lock; `#[thread_local] usize`; `c_int` file statics initialized to `-1` |
| cache map/watch | `g_cmap[128]` with `u64,u64,u8*`; `g_n_cmap: int`; bounds `~0ull,0`; volatile `int` | `[CMap;128]` with `AtomicPtr<u8>` watch; `c_int`; `u64::MAX,0`; `AtomicI32` |
| paths and mapping constants | `g_cache_dirs[]`, `CACHE_STEM`, `CACHE_MAX_SUBCACHES`, `LAZY_MAX`, `CMAP_MAX`, watch bits | typed NUL-terminated `*const c_char` strings; matching `c_int` capacities and `u8` watch bits |
| subcache/index | `g_named_cache: const OcerzCache*`; static `uint8_t hdr[0x400]`; `g_cix: CacheIndex*` | matching cache pointer; `[u8;0x400]`; `AtomicPtr<CacheIndex>` with acquire load and AcqRel/Acquire CAS publication |
| resolve memo | `g_rmemo[1<<16]` `{char*,u64,int}` and `g_fmemo[1<<15]` `{u64,char*,u64,int}` | matching `ResolveMemo` / `FromMemo` arrays with C-compatible fields |
| mutexes | three `pthread_mutex_t` objects initialized with `PTHREAD_MUTEX_INITIALIZER` | `libc::pthread_mutex_t` initializers and pthread lock/unlock |
| weak filter | weak pointer/count, bloom pointer, build flags, `uint32_t lookups`, mutex | pointer/`u32`/`c_int` state, `AtomicPtr<u8>` bloom with Release/Acquire, pthread mutex |

The SDK audit set `MH_WEAK_DEFINES` to `0x8000` and
`DYLIB_USE_COMMAND_SIZE` to 28 bytes (the SDK's seven-`uint32_t` struct).
These values corrected the initial C/Rust differences in weak-image filtering
and upward-dependency skipping. Other unsigned cache arithmetic uses explicit
wrapping operations. The cache-local `mach_vm_remap` declaration and
`VM_INHERIT_DEFAULT` use `c_int`, matching the existing mem module; the
memvm-owned `rust/src/ported/mem.rs` remains unchanged.

Run `bash tools/bench/cache_bench.sh` to compare the C and Rust trees. It
enumerates and deterministically samples 20,000 export names from the first 50
cache images, adds 2,000 misses, exercises all five resolver APIs plus image,
alias, address-name, and region queries, and times 20 cache-mode starts per
tree. Three alternating runs reported `HASH MATCH`
(`ddcfe80c6be96dc3`); C/Rust timings:

| Run | Cold resolve ns/symbol | Warm resolve ns/symbol | Region ns/call |
|---|---:|---:|---:|
| 1 | 67104.4 / 66192.2 | 12514.3 / 11043.0 | 7.23 / 5.92 |
| 2 | 62891.2 / 64675.6 | 10571.9 / 10587.9 | 5.95 / 6.76 |
| 3 | 63668.8 / 64037.1 | 10590.8 / 10663.4 | 6.08 / 5.98 |

20-start startup time was 25.49 ms C / 23.97 ms Rust. Median resolver and
region differences were below 3%.

Verification: `rm -f src/cache.o src/cache.d && make -j12 ocerz` passed;
`nm -gU ocerz` found 20 cache exports and no stale `src/cache.o/.d`.
`tests/unit/bin/test_cache` passed 15 checks. Dynamic `/bin/echo` and
`tests/guest/benchbin/xbench_dyn depchain 1` produced identical output and
exit codes between C and Rust with lazy remapping, `OCERZ_NO_LAZY_REMAP=1`,
and `OCERZ_EAGER_SLIDE=1`; `OCERZ_LAZYCHECK=1` produced 53 `ok` lines per
guest/tree and no failures.

Both `bash tools/rust_gate.sh --fast` and the full gate passed with no new
failures. The post-fix fast gate had 134/0 no-JIT guests, 134/0 JIT guests,
100/0 differential tests, and a real diff32 run of 40,044 passed / 0 failed
with 107,409 translated blocks. The full gate's diff32 run had 40,044 passed /
0 failed and 107,410 translated blocks. Its dynamic phase had 280 passed and
7 failures, all present in the expected-failure baseline; two previously
expected `dtest_jcc_gap_low` failures no longer reproduced. `dyn.new` was
empty. The native framework and native format phases passed; no i386 phase
log was produced.

## Mach VM extern follow-up

The pre-edit release build reported clashing declarations for
`mach_vm_remap` and `mach_vm_region`. Bindgen exposes only the
`ocerz_sys_mach_vm_remap` / `ocerz_sys_mach_vm_region` shims, and libc has no
raw declarations for either function, so the declarations follow the active
SDK prototypes in `mach/mach_vm.h`. The SDK defines `vm_inherit_t` as unsigned
int and `vm_region_info_t` as `int *`; `boolean_t` and `vm_prot_t` are `int`.

| Symbol | Declaration sites and prior varying argument | Aligned signature |
|---|---|---|
| `mach_vm_remap` | `cache/map.rs` (`c_int`), `mem.rs` (`c_int`), `sysbridge/mem.rs` (`libc::vm_inherit_t`), `syscall/bsd.rs` (`i32`), `syscall/mach.rs` (`c_int`) | The task/address/size/mask/flags/copy/protection/return types otherwise match; `inheritance` is `libc::vm_inherit_t` (`u32`) at all five sites. |
| `mach_vm_region` | `mem.rs` (`*mut c_void`), `sysbridge/mem.rs` (`*mut c_void`), `syscall/mach.rs` (`*mut c_void`), `vm/mod.rs` (`vm_region_info_t`) | The task/address/size/flavor/info-count/object-name/return types otherwise match; `info` is `*mut c_int` / `vm_region_info_t` at all four sites. |

The initially reported mismatches were `cache/map.rs` versus
`sysbridge/mem.rs` for `mach_vm_remap`, and `mem.rs` versus `vm/mod.rs` for
`mach_vm_region`; source audit found the additional duplicate declarations
listed above before the final build. Argument and constant types were updated
without changing their numeric values.

The subsequent rebase to `098bb5e` also exposed a `sys_icache_invalidate`
clash: `jit_control.rs` declared `*const c_void`, while
`jit.rs`, `jit_cache.rs`, `jit_flags.rs`, and `jit_tcache.rs` declared
`*mut c_void`. The active SDK's `libkern/OSCacheControl.h` prototype is
`void sys_icache_invalidate(void *start, size_t len)`, so `jit_control.rs`
was aligned to `*mut c_void` with `size_t`. No bindgen or libc declaration
exists for this symbol. `clock_gettime_nsec_np(clockid_t) -> uint64_t` was
also checked against the SDK; it had no clash and needed no edit.

After these alignments, `cargo build --release` reported zero
`clashing_extern_declarations`. Before the rebase, the cleanup preserved the
baseline executable text hash
`a5732e0c7f5fc3d0a0e40ce14b62de9f981b78dd`. The rebase brought in commit
`098bb5e`, which ports `jit_control.c` to Rust; therefore its executable
hash is not directly comparable to that earlier C/JIT-control baseline. A
clean build at `098bb5e` had hash
`1686578039c9c58358c255fdde46608fa33d270c`, versus
`cef5eb8c759db8c72d3147dfdffa8f3c4045c77b` on the aligned branch. Their
`__TEXT,__text` sizes are both `0x2b464c` and both disassemble to 702,803
instructions. The complete same-tip `otool -tV` comparison found only
address-formation relocation differences: 304 `adrp` page immediates and
4,567 `add` literal-pool offsets, all targeting the same literal labels
shown in the disassembly; opcodes, registers, and branch displacements are
otherwise identical. The initial and final build logs are in
`~/clash_before.log`, `~/clash_after.log`, and `~/clash_final.log`; text
comparisons are in `~/text_before.s`, `~/text_tip_baseline.s`, and
`~/text_current_tip.s`.

The fast gate passed after the original cleanup (`100` differential passes,
`0` failures; `GATE: PASS`).

## tcache

Ported `src/tcache.c` to `rust/src/ported/tcache/`, split into `store`, `io`,
and `writer`. The port retains the original index/slot and data-file layouts,
40-bit data offsets, record framing and checksum, raw LZ4 compression,
fingerprint inputs and C `qsort`/`strcmp` ordering, directory claim/prune
protocol, shared mmap publication order, and fork-child reset. It uses C
allocators and pthread mutex/condition/thread APIs; no Rust collection,
mutex, or condition variable is used.

Static-state audit (C declaration → Rust representation):

| State | C declaration | Rust representation |
|---|---|---|
| main lock and mode | `pthread_mutex_t` initializer; `int g_mode = -1` | `libc::pthread_mutex_t` initializer; `AtomicI32(-1)` (same signed 32-bit storage) |
| fingerprint/path/open state | `uint64_t g_fp`; `char g_dir[1024]`; `int g_opened`; header/slot/file pointers; mask and file count | `u64`; `[c_char; 1024]`; `c_int`; matching raw pointers and integer widths |
| limits/full state | `uint64_t` capacity, floor, room, data number/offset; `int g_full = 0` | `u64`; `AtomicI32(0)` (same signed 32-bit storage) |
| descriptors/logging | `int g_dfd = -1`, `g_log = -1`, `g_ufd = -1` | `c_int` with the same `-1` initializers |
| buffers | `uint8_t*` buffers and `size_t g_buf_n` | `*mut u8` and `usize`, null/zero initialized |
| writer state | pthread lock/conditions with static initializers; queue/spares of four; `int` counters; `pthread_t` | libc pthread statics; four `QueueItem`s and spare pointers; `c_int` counters; `pthread_t` |
| mapped index | `uint64_t` keys/locations, acquire loads, AcqRel/Acquire key CAS, release location store | `AtomicU64::from_ptr` with the same orderings |

The C fingerprint skips the UUID input when the main executable lacks
`LC_UUID`; that branch was reviewed in the C source, but not executed because
dyld aborts before `main` for no-UUID executables on this host. The benchmark
therefore links each driver normally, patches both UUIDs to
`00112233-4455-6677-8899-aabbccddeeff`, then ad-hoc signs them. It prints and
checks both UUIDs and both cache fingerprints.

Run `PERF_RUNS=5 tools/bench/tcache_bench.sh` to exercise 50,000 deterministic
records in both C→Rust and Rust→C directions and compare fresh deterministic
writes. Both drivers reported the same UUID and fingerprint
`tc-1f7a1d02e4d5c59e`; both directions returned hash
`8a8582ad476c0930`. Fresh index and `d-1.td` contents were byte-identical;
no PID/timestamp fields needed masking.

The final five alternating timing runs (C / Rust median, ns/call) split put
into a 2,000-call no-handoff sample (88-byte records; 176,088 bytes total),
the 50,000-call put loop, and that loop plus the final flush:

| Measurement | C | Rust | Rust delta |
|---|---:|---:|---:|
| Find hit | 103.25 | 206.27 | noisy; run order strongly affected samples |
| Find miss | 5.09 | 4.39 | -13.8% |
| Put, no handoff | 12.50 | 16.00 | +3.5 ns; ranges overlap |
| Put loop, excluding final flush | 442.18 | 476.66 | +7.8% |
| Put loop plus final flush | 474.96 | 519.44 | +9.4% |

The no-handoff sample stayed below the 256 KiB buffer threshold; its observed
ranges were 10.5–33.5 ns (C) and 9.5–34.5 ns (Rust), so the 3.5 ns median
difference is within the run-to-run variation. The total-minus-loop flush
time was 32.78 ns/record (C) and 42.78 ns/record (Rust). The larger delta
therefore appears in buffer handoff/backpressure and queued writer work, not
the steady-state copy into an unfilled buffer; the exact contribution of
handoff versus background compression/I/O is not isolated by this benchmark.

I compared the writer paths: both use libc `memcpy` for record and stored
payload copies; the Rust queue shift uses overlapping `ptr::copy`, equivalent
to C `memmove`. Both use the same four-entry queue, condition wait/signal/
broadcast pattern, detached writer, 256 KiB input/compression buffers, 512 KiB
output buffer, raw-LZ4 scratch sizing, and `pwrite` chunking. Neither path
calls `fsync`. No behavioral or buffer-size divergence was found to fix. The
initial out-of-line `stored_sum` call in Rust's find path was inlined to match
the C compiler; no Rust implementation change was needed in this isolation
pass. Find-hit timings in this run were highly order-sensitive and should not
be treated as a stable comparison.

I reran five paired find-hit samples with the driver order alternating
C→Rust, Rust→C, C→Rust, Rust→C, C→Rust. The per-run ns/call values were:

| Pair | First | C | Rust |
|---|---|---:|---:|
| 1 | C | 182.65 | 180.84 |
| 2 | Rust | 197.70 | 164.45 |
| 3 | C | 123.40 | 114.53 |
| 4 | Rust | 189.43 | 111.49 |
| 5 | C | 120.48 | 128.60 |

The follow-up medians were 182.65 ns/call C and 128.60 ns/call Rust. Together
with the earlier 91.91 / 94.49 ns medians, these order-sensitive samples do
not support a stable 2× Rust find-hit slowdown, so no further find-path change
was made.

With `OCERZ_TCACHE_TRACE=1`, cold dynamic `xbench_dyn depchain 1` runs on both
trees saved 3,167 records; warm runs logged 3,142 hits/loads and identical
stdout. Verify mode reported `verify_ok=3150`, `verify_variant=10`,
`verify_bad=0`; roundtrip reported `ok=3179 bad=0`. Twenty alternating warm
starts averaged 17.10 ms C / 16.54 ms Rust.

`make -j12 ocerz` passed after removing `src/tcache.o` and `src/tcache.d`;
`nm -gU ocerz` showed one definition of each of the five exports.
`tests/unit/bin/test_cache` passed 15 checks. Both the fast and full gates
passed with no new failures. The full-gate diff32 log recorded 40,044 passed /
0 failed and 107,410 translated blocks; no i386 phase log was produced.
The native framework phase passed. Dynamic tests reported 280 passed and
7 expected failures; `dyn.new` was empty.

### Put-path disassembly and profiler follow-up

I compared `otool -tV` output from the C and Rust benchmark drivers for
`ocerz_tcache_put`, handoff, the writer loop, and stored writes. Both put
paths perform the same null/size/alignment validation, lock with pthread
mutexes, and call libc `memcpy` to copy records into the 256 KiB buffer. The
handoff paths use the same detached `pthread_create`, condition wait/signal,
and queue pattern. The writer paths both use raw-LZ4 compression with the
same scratch and 512 KiB output sizing, copy stored payloads with `memcpy`,
zero padding with `bzero`, and write with the same `pwrite` flow. Rust's
`store_buffer` body is inlined into its writer entry; its queue shift calls
`memmove`, while the C compiler emits `___memmove_chk`. The Rust path adds no
panic/bounds-check branch or stronger atomic operation in these paths; the
mapped-index atomics are not on the put-copy path. No implementation
difference explaining a put slowdown was found.

`sample` profiled alternating 500,000-record puts into fresh directories.
One initial C capture ended before the workload and had no samples; the
second C capture and both Rust captures showed the same split. On the C
translator thread, 140 of 171 samples were in
`ocerz_tcache_put -> hand_off_locked -> pthread_cond_wait`; on the Rust
translator thread, 141–142 of 166–173 samples were in the corresponding
handoff wait. The C writer's top path was `writer_main -> store_buffer ->
compression_encode_buffer` (152 compressor samples), with eight `pwrite`
samples. The Rust writer's top path was `writer_main_entry ->
compression_encode_buffer` (146–148 compressor samples), with seven to
eight `pwrite` samples. Both profiles therefore show the translator waiting
for the writer and the writer spending its time in the same compression/I/O
work, rather than a Rust-only put-copy cost. Evidence is in
`~/tcache_profile_c2.sample`, `~/tcache_profile_rust1.sample`,
`~/tcache_profile_rust2.sample`, `~/tcache_asm_c.s`, and
`~/tcache_asm_rust.s`.

I reran five alternating benchmark pairs. The no-handoff sample used the
existing 2,000-call short-record loop; the 50,000-record columns include
handoff/backpressure, and the total column includes final flush:

| Pair | Order | Fast put C / Rust | Put loop C / Rust | Loop + flush C / Rust |
|---|---|---:|---:|---:|
| 1 | C then Rust | 10.00 / 13.00 | 488.14 / 438.58 | 521.38 / 478.26 |
| 2 | Rust then C | 15.50 / 9.50 | 467.98 / 459.88 | 506.74 / 493.38 |
| 3 | C then Rust | 32.50 / 44.00 | 518.28 / 525.96 | 557.38 / 572.76 |
| 4 | Rust then C | 28.50 / 23.00 | 544.60 / 526.74 | 581.44 / 571.96 |
| 5 | C then Rust | 32.50 / 32.00 | 533.04 / 530.36 | 570.66 / 566.32 |
| Median | — | 28.50 / 23.00 | 518.28 / 525.96 | 557.38 / 566.32 |

Values are ns/call or ns/record as applicable. The loop medians differ by
1.5% and loop-plus-flush medians by 1.6%; the no-handoff median is lower for
Rust, with substantial run-to-run variation in both trees. The paired
total-minus-loop estimate of flush/queued work is 37.62 ns/record C and
39.68 Rust. This is not a precise writer-only split, but it does not support
the earlier 8.7% put delta as a stable regression. No tcache implementation
change was made.

The repeat cross-tree format run again reported `HASH MATCH` in both
directions (`8a8582ad476c0930`); both fingerprints were
`tc-1f7a1d02e4d5c59e`, and the fresh index and `d-1.td` files were
byte-identical. Timing and format output is in `~/tcache_followup_5runs.log`.

## main

`src/main.c` is implemented in `rust/src/ported/main.rs`; the generated
ported-module list excludes `src/main.o`. The Rust entry point preserves the
two-argument C ABI and is weakly linked so strong `main` definitions in C unit
and benchmark drivers continue to win. The `linkage` crate feature is enabled
in its own build-only change. The module carries the C startup rationale and
preserves option parsing, environment handling, dyld re-exec, bundle and Wine
resolution, mock-keychain arguments, command-line summary, and dynamic versus
static guest startup.

| Static or global | C declaration / initializer | Rust declaration / initializer |
|---|---|---|
| mock keychain argv | `static char *out[514]` (zeroed) | `static mut [*mut c_char; 514]` (null pointers) |
| bundle executable | function-local `static char bundle_exe[PATH_MAX]` (zeroed) | `static mut [c_char; PATH_MAX]` (zeroed) |
| plist buffer | function-local `static char buf[1 << 20]` (zeroed) | `static mut [c_char; 1 << 20]` (zeroed) |
| VM | function-local `static OcerzVM vm` (zeroed) | `static mut OcerzVM` initialized with `zeroed()` |
| command summary | extern `char[256]`, defined in `globals.c` | shared `globals.rs` `c_char[256]` definition |
| environment vector | extern `char **environ` | matching extern `char **` declaration |

`objc_images` remains a NUL-terminated C string literal at its use site; the
Rust version and project strings match `version.h` (`AArchX 0.6`). The parity
driver is `bash tools/bench/main_parity.sh`; it compares stdout, stderr,
normalized binary paths, exit codes, and redirected stderr, then times twenty
`/bin/echo hi` starts per tree.

The parity run had exact matches for 17 cases: no args, version, unknown
option, missing/non-Mach-O/truncated files, verbose/trace/no-JIT static guests,
cache-mode echo, invalid `OCERZ_MODE`, `--`, redirected stderr, `OCERZ_EXE_ENV`,
`OCERZ_NOJIT_EXE`, `OCERZ_STRACE_EXE`, and a constructed bundle. The
`-native /bin/echo hi` case was run on both trees but is a known cross-tree
divergence: the C reference's older dyld exits 71 after reporting ten
unresolved libSystem bridges, while the Rust tree runs echo successfully
(exit 0, `hi`). This is outside `main.c`; the parity script reports it
explicitly instead of treating it as a match.

Twenty `/bin/echo hi` starts averaged 24.136 ms C / 24.022 ms Rust with
`OCERZ_TCACHE=off`.

After removing `src/main.o` and `src/main.d`, `make -j12 ocerz` succeeded;
both files remained absent, and `nm -gU ocerz` showed the Rust entry point as
`T _main`. The fast and full gates both reported
`GATE: PASS (no new failures vs baseline)`. The fast gate had 134/0 guests in
each mode and 100/0 differential tests. Full-gate diff32 covered 40,044
sequences and 1,060,272 guest steps per side, translating 107,410 blocks.
The full dynamic phase had 280 passes and seven expected failures; `dyn.new`
was empty (two failures listed in the expected baseline did not reproduce).
The native framework phase passed, and no i386 phase log was produced. All
unit bins linked and ran with the three existing unit failures unchanged.
The a64emit, cache, and tcache cross-tree bench drivers also linked and ran:
their hashes matched (`a92b607b1b78480d`, `ddcfe80c6be96dc3`, and
`8a8582ad476c0930`, respectively).

## Remaining C

None of the six requested modules remain in C: `a64emit.c`, `stack.c`,
`loader.c`, `cache.c`, `tcache.c`, and `main.c` are ported. `jit_core_shim.c`
remains as the separate sigsetjmp/decode-loop shim documented in the status
table; it is outside this six-module port.

## SDK constant audit

`tools/audit/sdk_consts.sh` extracts every Rust `const` declaration under
`rust/src/ported/**`, probes the active macOS SDK for arm64 and x86_64, and
evaluates copied Rust declarations in a scratch Cargo project. Arm64 values
are printed by a native program; x86_64 values are read from emitted assembly
because Rosetta is unavailable on this VM. The script records type-width and
signedness differences separately from numeric mismatches.

The audit artifacts are under `~/constaudit/`: `consts.tsv`, `probe.h`,
`probe_status.tsv`, `sdk_values.tsv`, `rust_eval.tsv`, `audit.tsv`, and
`unmatched.tsv`. The probe includes the requested Mach, Mach-O, POSIX,
networking, process, and compression headers, plus the additional headers
needed by extracted names. The final run found 1,101 declarations across 795
names; 119 SDK names compiled for both architectures and two additional
thread-state names compiled only for arm64. Of the evaluated declarations,
181 match both SDK values. Four pointer-valued expressions are marked MANUAL
by the integer-only evaluator (`NULL`, `__DARWIN_NULL`, `RTLD_DEFAULT`, and
`MAP_FAILED`); `ARM_THREAD_STATE64` and `ARM_THREAD_STATE64_COUNT` are also
MANUAL in the two-architecture table because they are arm64-only. They are
host thread-state values, not x86-64 guest constants. No arm64/x86_64
value-divergent constants were found. The 17 SDK-looking names that do not
compile on either architecture are individually hand-checked in
`unmatched.tsv`; they are project-local aliases/values, compatibility
fallbacks, or unused local declarations, not SDK value mismatches.

The audit found and corrected these numeric mismatches:

| Rust declaration | Old → SDK value | C reference site | Guest-visible effect |
|---|---:|---|---|
| `dyld/bind.rs:682` `N_WEAK_REF` | 128 → 64 | `src/dyld.c:1953` | The wrong bit misidentifies Mach-O weak imports during binding. |
| `syscall/bsd.rs:17` `VM_INHERIT_SHARE` | 1 → 0 | `src/syscall.c:5277` | A SysV attachment copied at fork would hide the child's write from its parent. |
| `syscall/mach.rs:19` `VM_INHERIT_DEFAULT` | 2 → 1 | `src/syscall.c:6759`, `:6927` | Mach alias and MIG-reply remaps receive the wrong inheritance semantics. |
| `vm/mod.rs:178` `THREAD_BASIC_INFO_COUNT` | 11 → 10 | `src/vm.c:630`, `src/jit.c:1340` | `thread_info` is called with the wrong structure word count. |
| `vm/mod.rs:185` `MACH_PORT_RECEIVE_STATUS` | 1 → 2 | `src/vm.c:1571`, `:3329` | Port attribute queries use the wrong flavor and return incorrect status. |
| `vm/mod.rs:188` `MACH_PORT_TYPE_PORT_SET` | 8 → 524288 | `src/vm.c:1558` | `mach_port_names` entries fail the port-set bit test. |

The VM inheritance definitions now live once in `rust/src/inline.rs`, sourced
from `libc::VM_INHERIT_SHARE` and `libc::VM_INHERIT_COPY`; the BSD syscall,
Mach syscall, cache-map, and memory-map code all use those shared definitions.
`N_WEAK_REF` is narrower than the SDK's C `int`, but its value 64 is
representable and remains the same flag bit passed in the Mach-O `n_desc`.

The fork regression is `tests/dynamic/sysv_shm_fork.c`, registered as
`dsysv_shm_fork` with the single expected output line in its `.out` file. It
writes through the inherited `shmat` pointer in the child without reattaching.
Before the inheritance fix, both Rust JIT and no-JIT runs exited 1 and printed
`bad`; the C-reference run exited 0 and printed `ok`. The existing
`tests/unit/test_syscall.c::test_mach_vm_remap_mig_relocation` covers a MIG
remap in-process, but no existing fixture exercises either Mach default
inheritance path after a fork; no separate test was added for those paths.
After the fix, the Rust JIT and no-JIT runs both exited 0 and printed `ok`.
