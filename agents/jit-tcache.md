# JIT translation cache

`src/jit_tcache.c` is now implemented by `rust/src/ported/jit_tcache.rs` at the existing C ABI. The C source remains in the tree, but the Rust port supplies the cache state and exported functions used by the build. The port covers cache relocation and block emission, record creation and loading, validation, roundtrip checks, and cache summaries. Shared JIT and memory inline helpers come from `crate::jit_internal`.

`src/jit_tcache_shim.c` contains only `ocerz_tc_guard`. Rust cannot express the `returns_twice` behavior of `sigsetjmp`, so the guard and its recovery-pointer save/restore remain in C; the callback and the rest of the dependency check are Rust.

## Fidelity notes

- Cache limits, flags, and JIT constants are aliases of generated `ffi` values so they track the C headers.
- The `st` and `pf` locals in `tc_put` are zero-initialized rather than left partially uninitialized. Only populated entries are emitted, so serialized bytes are unchanged.
- The `OCERZ_NO_DISPATCH_STUB` `ENV_ON` check uses a call-site static cache and `libc::getenv`, preserving the C macro's per-call-site caching behavior.
- Host symbol lookup uses `libc::Dl_info` and `libc::dladdr`.

## Cache-oracle results

The table gives roundtrip counters as `ok / bad / hostconst`. `pcrel`, `mismatch`, and `full` were zero for every target. The extra C runs reproduced the alternate counter outcomes, confirming the observed ±1 variation in both implementations rather than a stable Rust/C discrepancy.

| Target | C first run | Rust | Additional C runs |
| --- | ---: | ---: | ---: |
| `tcache_work` | `3648 / 0 / 0` | `3647 / 1 / 1` | `3648 / 0 / 0`; `3647 / 1 / 1` |
| `avx_fp` | `3956 / 1 / 1` | `3955 / 0 / 0` | `3955 / 0 / 0`; `3954 / 1 / 1` |
| `low_hoist` | `3680 / 0 / 0` | `3680 / 0 / 0` | — |
| `/bin/ls -la /` | `7936 / 1 / 1` | `7937 / 0 / 0` | `7937 / 0 / 0`; `7936 / 1 / 1` |
| `/bin/echo` | `3267 / 0 / 0` | `3266 / 1 / 1` | `3266 / 1 / 1`; `3266 / 1 / 1` |
| `/usr/bin/true` | `3163 / 0 / 0` | `3162 / 1 / 1` | `3162 / 1 / 1`; `3163 / 0 / 0` |
| `/bin/date` | `3737 / 0 / 0` | `3736 / 1 / 1` | `3737 / 0 / 0`; `3736 / 1 / 1` |

For cache `on`, fresh-store writes and warm loads had matching C/Rust counters, and program output and exit codes matched. The `verify` counters also matched, with `verify_bad=0`. Output and exit codes matched in `off` and `roundtrip` modes as well. Cache stores are keyed by the guest image's `LC_UUID`, so C and Rust records cannot be cross-loaded; each build was checked with its own fresh store.

## Emission audit and gates

- Emission audit against `~/AArchX-jitc` at `16a7c2d`: **MATCH**, 215,295 blocks and 145,523,525 arm64 words.
- Full `tools/rust_gate.sh`, after the shared logging-macro fix: **PASS (no new failures vs baseline)**. Build passed; unit tests had 3 known failures; guest no-JIT and JIT each passed 134/134; differential passed 100/100; differential32 translated 107,410 blocks; dynamic tests passed 280 with 7 known-pending failures; native tests passed 86 with 1 known failure; guest-library, native-framework, and native-format phases passed. Native C++ and Swift phases were skipped because their guest builds were not prepared.
- Latest post-rebase `tools/rust_gate.sh --fast`: **PASS (no new failures vs baseline)** with the same unit baseline; guest no-JIT/JIT passed 134/134 each, differential passed 100/100, and differential32 translated 107,410 blocks.

## Performance

Six alternating C/Rust batches were measured per fixture and mode, with 30 fresh translator processes per batch. Times are milliseconds per process: median of the six batch averages, followed by the min–max spread.

| Fixture | Mode | C median (min–max) | Rust median (min–max) |
| --- | --- | ---: | ---: |
| `tcache_work` | off | `49.962 (47.482–50.496)` | `50.538 (46.510–51.690)` |
| `tcache_work` | warm on | `35.571 (30.511–38.308)` | `33.102 (29.786–40.310)` |
| `tcache_work` | roundtrip | `54.548 (53.684–56.204)` | `54.115 (53.522–57.092)` |
| `avx_fp` | off | `50.443 (49.572–52.387)` | `50.827 (49.415–52.223)` |
| `avx_fp` | warm on | `37.480 (36.511–38.982)` | `36.866 (34.873–38.079)` |
| `avx_fp` | roundtrip | `55.335 (51.114–62.210)` | `55.103 (52.567–62.477)` |
| `low_hoist` | off | `50.526 (46.270–52.165)` | `51.789 (49.997–54.921)` |
| `low_hoist` | warm on | `28.384 (27.810–32.258)` | `30.143 (29.282–31.424)` |
| `low_hoist` | roundtrip | `58.949 (55.155–70.113)` | `63.544 (60.854–73.554)` |
| `/bin/ls -la /` | off | `89.533 (87.829–95.670)` | `92.695 (89.732–94.461)` |
| `/bin/ls -la /` | warm on | `53.019 (51.304–66.939)` | `52.869 (51.759–54.526)` |
| `/bin/ls -la /` | roundtrip | `103.398 (99.786–110.848)` | `106.336 (97.594–127.339)` |

Warm-on did not show a Rust slowdown beyond the batch spread: all C/Rust min–max ranges overlap. `low_hoist` has a 6% higher Rust median, but its range overlaps the C range.

The benchmark script, raw batch data, summary, console log, warm-load logs, and cache stores are under `/Users/devin/AArchX-tcache-port-oracles/perf/`. The run's raw and summary CSV files are `raw-20261009-000846-391500.csv` and `summary-20261009-000846-391500.csv`.
