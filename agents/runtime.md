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
