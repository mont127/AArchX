# Runtime ports

## a64emit

Ported the AArch64 emitter exports to `rust/src/ported/a64emit.rs`, retaining the C ABI, buffer overflow/sink behavior, and patch semantics. The benchmark constrains `mode` to 0–4 for `a64_frint_s` and `a64_v_frint`, whose C implementations index five-entry opcode tables without bounds checks; out-of-range inputs would invoke C undefined behavior.

Run `bash tools/bench/a64emit_bench.sh` to compare against `~/AArchX-c`. Both runs covered all 256 prototypes and reported `HASH MATCH` (`a92b607b1b78480d`).

| Run | Metric | C ns/call | Rust ns/call |
|---|---|---:|---:|
| 1 | `a64_mov_imm64` | 7.263 | 7.152 |
| 1 | four `a64_try_*_imm` | 3.976 | 3.974 |
| 1 | random mix | 8.742 | 9.239 |
| 2 | `a64_mov_imm64` | 6.852 | 7.164 |
| 2 | four `a64_try_*_imm` | 3.992 | 3.791 |
| 2 | random mix | 9.147 | 8.709 |

## stack

Ported initial guest stack construction to `rust/src/ported/stack.rs`, preserving the C layout, allocation failures, log format, and guest-memory stores. Re-exported the existing sysbridge guest-memory helpers crate-wide rather than duplicating address translation and store logic; no stack-layout deviations. Stack setup runs once per process and is not a hot path, so no benchmark was needed.

Verification: `make -j12 ocerz` passed with a single `T _ocerz_setup_stack` and no `src/stack.o`; `OCERZ_NO_ARM_EXEC=1 tests/unit/bin/test_loader` passed (54 checks, 0 failures). The `-v` args guest log matched C (`argc=4 envc=39 unixthread`, 16-byte-aligned RSP, `stack_hi-rsp=0x900`). `bash tools/rust_gate.sh --fast` passed; `diff32` ran 40,044 sequences with 0 failures and translated 107,410 JIT blocks.
