# native1 porting notes

The native1-owned C/assembly boundary ports are recorded here with their ABI
constraints, verification evidence, and performance measurements.

## Ported modules

| Source | Rust replacement | Commits |
|---|---|---|
| `src/leaf.s` | `rust/src/ported/leaf.rs` | `bbfbff1` |
| `src/abicall.s` | `rust/src/ported/abicall.rs` | `4179094` |
| `src/apidb.c` | `rust/src/ported/apidb.rs` | `0b97297`, `7c0dac7` |
| `src/blocks.c` | `rust/src/ported/blocks.rs` | `cfed640`, `5212e30` |
| `src/vdylib.c` | `rust/src/ported/vdylib.rs` | `c6ed01f`, `e8d66a6` |
| `src/abi.c` | `rust/src/ported/abi.rs` | `360ad00` |
| `include/ocerz/mem.h` helpers | `rust/src/inline.rs` | `bca08bc` |

The shared inline commit adds `ocerz_g2h`, `ocerz_h2g`, `ocerz_ld`, and
`ocerz_st`. All six status rows are `ported` and owned by `native1`. The C and
assembly sources remain in the tree as references; the Makefile filters a
source from the native object list when its Rust port is present.

## Deviations and ABI-sensitive decisions

- `leaf.rs` preserves the assembly and appends a trailing `.text` after its
  `.data` block. `abicall.rs` retains `.subsections_via_symbols`.
- Leaf, abicall, and apidb were pushed as a stack because of upstream push
  races. The apidb log-macro fix is the separate `7c0dac7` follow-up.
- `e8d66a6` combines the apidb, blocks, and vdylib static-audit fixes. It has
  no `Signed-off-by` trailer; it was pushed as-is and was not amended.
- `5212e30` makes the blocks dispose-helper identity check compare function
  addresses with `ptr::fn_addr_eq`, matching the C pointer-identity check.
- `vdylib_xmm_contract` initializes its local `OcerzAbiSig` with `Default`
  before calling `ocerz_abi_parse`; the C local is uninitialized, but the
  parser fully writes the signature on success. The ABI port otherwise keeps
  C-uninitialized call frames and arrays in `MaybeUninit` rather than
  zero-filling them. `OcerzGuestCall::default()` is used where the C frame is
  zero-initialized.
- The ABI reservation helper initially omitted the top-shadow range and the
  arena lower bound. It was corrected before the ABI commit to match
  `ocerz_host_in_guest_reservation`'s low-shadow, top-shadow, and half-open
  arena checks.

## Static parity and hot-path rules

The full working audit is at `~/native1/static_audit.md`. Its key storage
comparisons are:

| Rust storage | C declaration / behavior |
|---|---|
| `G_ABI_CB: [AbiCallback; 65536]`, `G_ABI_CB_N: u32`, `G_ABI_SHAPES`, and `G_ABI_CB_BUCKET: [u32; 16384]` | `g_abi_cb[]`, unsigned `g_abi_cb_n`, pointer shape buckets, and `uint32_t` callback buckets; zero initialization; `used` is a sequentially consistent 32-bit atomic |
| `G_ABI_TIMEBASE` and `G_ABI_TIMEBASE_ONCE` | `mach_timebase_info_data_t` zero initialization and `pthread_once_t = PTHREAD_ONCE_INIT` |
| `G_AD_SLOTS: AtomicPtr<AdSlot>`, `G_AD_CHOSEN: AtomicI32`, `G_AD_MINOS: u32`, `G_AD_DIR: [c_char; PATH_MAX]` | pointer/int atomics, unsigned 32-bit minimum OS, and `char[PATH_MAX]`; zero initialization and sequential consistency match C |
| Blocks maps, `G_BLK_THUNK: AtomicU64`, and `G_AUTORELEASE: AtomicPtr<c_void>` | zero-initialized pointer buckets, C's `uint64_t` atomic, and pointer-width autorelease function storage; atomics use C's default sequential consistency |
| vdylib internal, filler, and leaf tables; GSS counters; library slots; trampoline page; `G_VD_HOOKED` | C table order and `#[repr(C)]` layouts are retained; unsigned counters and pointer atomics match C; the function-local `hooked` retains its `-1` initializer |
| `inline.rs` | no static storage; address conversions wrap, and load/store atomics preserve the C acquire/release orderings |

Unsigned hash, address, offset, allocation, alignment, and counter arithmetic
uses `wrapping_*` where C wraps. Raw C arrays on hot or guest-call paths use
pointer arithmetic rather than bounds-checked Rust indexing. Guest-call,
callback, and block-helper frames that can run guest code use no `Drop` types,
because guest code can `longjmp` through them. The ABI rounding helpers remain
private to `abi.rs`; they use `mrs`/`msr fpcr` and write only when rounding
bits differ. `blocks.rs` has compile-time size and offset assertions for its
Block ABI layouts.

## Gotchas

- `tests/run_diff32.sh` links `ls src/*.o`; stale `src/<ported>.o` files can
  cause duplicate Rust/C symbols. Remove the port's `.o` and `.d` before
  gates. A gate phase can misleadingly say `ok` when its runner failed to
  link, so inspect `/tmp/rust_gate/diff32.log` and the i386 unit/framework
  logs by hand. The verified diff32 result is
  `differential32: 40044 passed, 0 failed`.
- Strings passed to C or read by C tables must be NUL-terminated: use `c"..."`
  or explicit `b"...\0"` storage cast to `*const c_char`. A Rust `str`'s
  `.as_ptr()` is not a C string. The apidb log-macro follow-up uses the
  NUL-terminating `ocerz_log!` macro.
- The vdylib internal, filler, and leaf table order is ABI: trampoline slots
  encode the internal table index. `vdylib_xmm_contract` calls the C ABI
  parser and uses its fully initialized result.
- Vdylib image comparison covered 178 install names. After masking only the
  random 8-byte `libSystem` stack-guard slot at offset 207176, 177 images
  matched. The 22 differing offsets in `AutomaticAssessmentConfiguration`
  reproduced in a C-versus-C comparison and were not masked. The trampoline
  page matched byte-for-byte across 4096 bytes.
- Callback dispatch needs a current guest CPU or a successful thread attach,
  then enters `ocerz_vm_call_abi`; a standalone host-only callback round-trip
  is not feasible without a VM. The native-mode qsort guest-comparator
  workload exercises this path.
- One full-gate cache-mode `attach_apply` failure reported
  `attach_apply bad:00000080 n=384 sum=13f873a6`. It did not reproduce in the
  follow-up: the same fixture passed 30/30 times on the Rust tip and 30/30 on
  the C-fallback worktree. Logs: `~/attach_apply_current_30.log` and
  `~/attach_apply_fallback_30.log`.

## Performance

All timings are best of five on the same VM, comparing the Rust tree with
`~/AArchX-c`. The throwaway harnesses and run logs are not committed. Most ABI
and vdylib harnesses are under `~/native1`; the apidb harness and logs are
`~/apidb_bench.c` and `~/apidb_bench_{c,rust}_runs.log`.

| Workload | Method | C | Rust |
|---|---|---:|---:|
| apidb parse and lookup | 178 `.api` files, three full parse passes, and 608,712 successful export lookups per sample | 0.192051 s | 0.185614 s |
| vdylib native dispatch | `vdylib_perf`: 10M iterations each of `strlen`, `strcmp`, and imported `memcpy`, plus 10M `getpid` calls | 0.285337 s | 0.281450 s |
| ABI parse | 3,201 distinct signature notations × 100 passes (320,100 parse calls/sample) | 0.017508 s | 0.015977 s |
| ABI register-only crossing | 10M calls using a hand-built `OcerzCPU` with guest base 0 and a trivial host function | 0.077124 s | 0.074125 s |
| ABI general crossing | 10M calls using the same hand-built CPU, with stack spill, a double, and a struct argument | 0.540955 s | 0.479882 s |
| ABI native vdylib workload | Same `vdylib_perf` workload, rerun for the ABI port | 0.285354 s | 0.284231 s |
| ABI qsort guest callback | Native-mode guest qsort; 1,089,401 comparator calls | 0.630397 s | 0.618525 s |

The corrected ABI microbench harness is `~/native1/abi_bench.c`; parse
signatures and runner are `~/native1/abi_signatures.txt` and
`~/native1/run_abi_bench.py`. The native workload source and runner are
`~/native1/vdylib_perf.c` and `~/native1/run_vdylib_bench.py`. The qsort
fixture and runner are `~/native1/abi_qsort_guest.c` and
`~/native1/run_qsort_bench.py`. An initial synthetic ABI run used an invalid
guest RSP and crashed; the corrected harness installs a valid return slot and
resets RSP before every crossing. All reported ABI numbers use the corrected
binaries.

## Verification

Direct test evidence:

| Test | Result |
|---|---|
| `test_leaf` | 6,736,902 checks passed |
| `test_abi` | 28,116 checks, 0 failed |
| `test_callback` | 132,015 checks, 0 failed |
| `test_apidb` | 328 checks; the two known failures are missing macOS 27 API files `libSystem.B.dylib.api` and `CoreFoundation.api` |
| `test_blocks` | 107 checks, 0 failed |
| `test_vdylib` | 140,954 checks, 0 failed |
| `test_bridge` | 1,245 checks, 0 failed |
| `test_objcbridge` | 123,753 checks passed |

Assembly identity evidence:

- `~/leaf_bytes.diff` and `~/leaf_call_disasm.diff` are empty; the leaf byte
  difference is only the required trailing `.text`.
- `~/abicall_call_disasm.diff` and `~/abicall_common_disasm.diff` are empty.
  `~/abicall_bank_encodings.txt` confirms the callback bank is 65,536 slots
  of 8 bytes and that the first and last slot encodings match C.

Gate evidence, including gates run after upstream code arrivals:

| Module(s) | Full gate | Fast gate |
|---|---|---|
| leaf | `~/leaf_gate_full.log` | `~/leaf_gate_race4_fast.log` |
| abicall + apidb stack | `~/apidb_stack_gate_full.log` | `~/final_stack_gate_fast.log` |
| blocks | `~/blocks_gate_full_postrebase.log` | `~/blocks_gate_fast_race2.log` |
| vdylib | `~/vdylib_staticfix_full.log` | `~/vdylib_postrebase_staticfix_fast.log` |
| ABI | `~/native1/abi_gate_full.log` | `~/native1/abi_gate_postrebase2_fast.log` |

Each listed gate passed with no new failures against the recorded baseline.
The full gates reported 134/134 guest no-JIT, 134/134 guest JIT, and 100/100
differential cases. They also reported three known unit failures, seven known
dynamic failures, and one known native failure against the recorded baseline.
The current dynamic baseline includes `dtest_jcc_gap_low-{jit,no-jit}`, which
also fails on pristine C. Direct `test_apidb` has the two macOS 27 omissions
listed above. Full-gate diff32 logs contain completed runs of 40,044 passed,
0 failed; i386 decode32/decode32_addr/interpreter/JIT tests passed, as did the
i386 LDT tests in JIT, interpreter, and slow-bridge framework modes. After
upstream code arrivals, the fast gates were rerun and their diff32/i386 logs
were inspected manually.
