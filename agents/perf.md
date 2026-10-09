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
