# JIT cache Rust port

`rust/src/ported/jit_cache.rs` replaces `src/jit_cache.c` in the mixed C/Rust
build. It preserves the C ABI, exports, shared/private state, cached
environment lookups, synchronization, fault recovery paths, and code-emission
behavior. The port uses `crate::jit_internal` for the shared JIT helpers.

The only C implementation retained for this piece is
`src/jit_cache_shim.c`. `js_note_fail` uses `sigsetjmp` around a guest-memory
copy (C lines 943–950); that recovery boundary must return to the same C frame.
The shim saves and restores `ocerz_jit_decode_recover` and copies exactly eight
bytes. Everything else from `jit_cache.c` is Rust.

## Review-sensitive details

- `ocerz_jit_fault_info` copies `sizeof(out->host_holds)` bytes at C line 1914.
  The output field is eight bytes, while the source block field is sixteen.
  Rust copies eight bytes, preserving C behavior and avoiding overwriting the
  following output metadata.
- `ocerz_jit_invalidate_range` and `ocerz_jit_require_ordered` use
  `__builtin_return_address(0)` at C lines 2565 and 2702. Both Rust functions
  retain no-inline boundaries and use `core::intrinsics::return_address()`.
- `ENV_ON` is implemented as a per-callsite cached `getenv`, since the shared
  helper module does not define it. C-facing byte strings are NUL-terminated;
  the register-name table passed to `fprintf` is explicitly typed as
  `*const c_char`.
- `jit_flags.rs` declares `sys_icache_invalidate(*const c_void, ..)` while
  cache/tcache use `*mut c_void`, so the build warns about a clashing extern;
  the owner should align it.

## Verification

- `make -j12 ocerz` linked the final executable with `src/jit_cache.o`
  excluded and `src/jit_cache_shim.o` included.
- All 127 C function definitions have Rust counterparts. The 55 global
  `T`/`D`/`B`/`S` names in the C reference object were present in the Rust
  archive. Apple `nm` returned 1 after reporting LLVM 23 bitcode-reader
  warnings for `compiler_builtins`; it still parsed 903 Rust archive global
  definitions and found no missing cache symbols.
- The emitted-code oracle matched 215,295 blocks and 145,523,525 arm64 words
  across offset and low-shadow layouts, with relocation payloads masked.
- The fast and full Rust gates passed with no new failures versus baseline.
  The full gate reported 134/134 guest cases in each mode, 100/100 differential
  cases, 280 dynamic passes with seven baseline-known failures, 86 native
  passes with one baseline-known failure, and passing guest-library, native
  framework, and native-format phases. Native C++ and Swift phases were skipped
  because their guest runtimes were not built.
- The requested unit binaries passed with `OCERZ_NO_ARM_EXEC=1`; the full
  gate's unit phase also passed its baseline comparison after the final
  C-string pointer typing change.

## Performance

Measurements compare the Rust-cache candidate with `/Users/devin/AArchX-jitc`
at reference commit `16a7c2dd3232f7e90ed4e94d69eb6f055e3a23e3`. Negative
changes are faster for the candidate. The post-rebase candidate code was
`f2722253dd7012f53c52a052a675759eb5649732`; measurements followed its full gate
and emitted-code audit.

| Workload | Reference | Candidate | Change |
| --- | ---: | ---: | ---: |
| i386 diff32 wall time, plain | 21.684 s | 21.191 s | -2.27% |
| i386 diff32 wall time, translation probe enabled | 21.225 s | 21.178 s | -0.22% |
| Time inside `translate` | 631.718 ms | 592.924 ms | -6.14% |
| `tcache_work` | 33.394 ms/process | 32.794 ms/process | -1.80% |
| `avx_fp` | 32.968 ms/process | 32.762 ms/process | -0.62% |
| `low_hoist` | 32.094 ms/process | 31.891 ms/process | -0.63% |

The diff32 harness used `--jit-required --seed 0x1 --cases 20000`; medians are
from four alternating-order runs per tree. Translation time was enabled by a
temporary constructor probe and did not change production sources. Dynamic
fixtures used `OCERZ_TCACHE=off`; medians are from six alternating-order
batches of 30 fresh processes per tree and fixture. All exits and expected
outputs were checked. These short-process measurements include loader and
scheduler noise, so the small wall-time differences should be interpreted
cautiously.
