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
