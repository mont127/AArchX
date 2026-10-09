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
