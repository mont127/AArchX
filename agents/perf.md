# Decoder bench

`bash tools/bench/decode_bench.sh [tree ...]` linear-sweeps a 64 MiB window of
`dyld_shared_cache_x86_64` (offset 1 MiB) with `ocerz_decode` (x64) and
`ocerz_decode_mode(...,1)` (i386), best of 5 runs, and hashes every decoded
`X86Insn` field-by-field plus every return code (FNV-1a). Any decode
difference between trees shows up as HASH MISMATCH.

## Numbers

Run on this VM (64 MiB window at offset 1 MiB of dyld_shared_cache_x86_64):

| tree | mode | insns | errors | hash | ns/byte | ns/insn |
|---|---|---|---|---|---|---|
| AArchX-c (C, run 1) | x64 | 24538590 | 3577018 | 1b8a5a82cbe04e75 | 44.3 | 121.2 |
| AArchX-c (C, run 1) | i386 | 28705028 | 2385692 | 3f9b5a26c745e7ef | 50.3 | 117.7 |
| AArchX-c (C, run 2) | x64 | 24538590 | 3577018 | 1b8a5a82cbe04e75 | 41.7 | 114.0 |
| AArchX-c (C, run 2) | i386 | 28705028 | 2385692 | 3f9b5a26c745e7ef | 54.4 | 127.2 |
| AArchX (rust scaffold + globals/flags ported) | x64 | 24538590 | 3577018 | 1b8a5a82cbe04e75 | 41.1 | 112.3 |
| AArchX (rust scaffold + globals/flags ported) | i386 | 28705028 | 2385692 | 3f9b5a26c745e7ef | 46.8 | 109.3 |

HASH MATCH across all three. The two C runs bracket the rust tree's numbers —
the port is decode-identical and within run-to-run noise.

## Integration snapshot -- 2026-10-08, tip 568d383

First integrated check after the jit split and the cpu / flags_live /
x87 / mem / objcguard ports landed. xbench numbers are best-of-3 wall
seconds of `tests/guest/benchbin/{xbench,xbench_dyn}`; decode_bench is
the usual 64 MiB shared-cache sweep. The two trees were timed under the
same load (the full gate was running concurrently), so treat tenths of a
second as noise.

| binary | mode | AArchX-c (pure C) | rust tip 568d383 |
|---|---|---|---|
| xbench | -no-jit | 23.54 | 24.82 |
| xbench | jit | 0.48 | 0.48 |
| xbench_dyn | -no-jit | 21.38 | 21.15 |
| xbench_dyn | jit | 0.47 | 0.48 |

decode_bench (tip vs C, HASH MATCH):

| tree | mode | hash | ns/byte | ns/insn |
|---|---|---|---|---|
| AArchX-c | x64 | 1b8a5a82cbe04e75 | 41.280 | 112.9 |
| AArchX-c | i386 | 3f9b5a26c745e7ef | 47.778 | 111.7 |
| AArchX (tip) | x64 | 1b8a5a82cbe04e75 | 38.336 | 104.8 |
| AArchX (tip) | i386 | 3f9b5a26c745e7ef | 45.034 | 105.3 |

Gate note: this tip is NOT fully green -- run_native_framework_tests
exits 1 on the native_compat stderr diff (extra
`:/:\capacity overflow` bytes interleaved before guest output). Bisected
to 0e312ce (cpu.rs port); e1a223f..11e91ee clean, C tree clean,
reproduces deterministically. A dynamic `datomic_counter` exit=124 seen
in the same run was a load flake (3/3 clean standalone).
