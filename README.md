# Ocerz

> [!WARNING]
> This project is experimental and was vibecoded. Use at your own risk.

**A from-scratch x86_64 → arm64 userspace binary translator for macOS — zero Rosetta involvement.**

Ocerz loads static `LC_UNIXTHREAD` x86_64 Mach-O binaries and runs them directly on Apple Silicon. It does its own loading, decoding, flag emulation, JIT translation, and syscall forwarding to the native arm64 kernel. Rosetta 2 is never invoked.

## Status

Full `make check` is green:

| Suite | Result |
| --- | --- |
| `a64emit` (run-don't-read encoder) | pass |
| corpus length validation | 511 / 511 |
| decode | 190 cases |
| ext (strings/bits/CPUID/x87) | 158 checks |
| interp | 166 assertions |
| loader | 54 |
| sse | 246 |
| syscall | 94 |

End-to-end: **11 / 11** guest binaries pass in interpreter mode, **11 / 11** in JIT mode, and **11 / 11** in the JIT-vs-interpreter differential.

Measured JIT speedup on `fib(30)`: **0.52s → 0.18s (~2.9×)**, 22.9M guest instructions, 14 blocks translated.

> Note: the freestanding (`-nostdlib`, raw-syscall) guest test binaries actually *drop output lines* when run under Rosetta 2 (e.g. SSE printed 21 of 52 lines), even with vectorization off. Ocerz's output matches independent real-libc oracles (arm64-native and x86_64+printf) byte-for-byte — Ocerz is more faithful than Rosetta for these binaries. Goldens were regenerated from those oracles.

## Quickstart

```sh
git clone <repo-url> && cd Ocerz-main
make check                              # build + run all unit and e2e tests
./ocerz tests/guest/bin/hello           # run a guest binary (JIT by default)
```

## CLI

```
usage: ocerz [-v] [-trace] [-strace] [-no-jit] [--] program [args...]
```

| Flag | Effect |
| --- | --- |
| `-v` | verbose (repeatable, raises level) |
| `-trace` | per-instruction trace |
| `-strace` | syscall trace |
| `-no-jit` | force the interpreter (JIT is on by default) |
| `--` | end of Ocerz options; everything after is guest `argv` |

## Architecture

Five tiers:

- **Loader** (`src/loader.c`) — parses static `LC_UNIXTHREAD` Mach-O, maps segments, sets up the stack.
- **Decoder** (`src/decode.c`) — x86_64 → internal `X86Insn` (op widened to `uint16_t`; the `OcerzOp` enum has 364 values).
- **Eager-flags interpreter** (`src/interp*.c`, `src/flags.c`) — GP core, full SSE through an SSE4.1 subset, x87-on-doubles, CPUID/RDTSC. Flags are evaluated **eagerly** into `cpu->rflags`; `flags.c` is the bit-for-bit reference the JIT must match (ADC/SBB folded via carry-in relations, INC/DEC preserve CF, deterministic values for architecturally-undefined flags). x86 NaN semantics are reproduced (negative QNaN indefinite, propagation rules).
- **Call-threaded JIT** (`src/jit.c`, `src/a64emit.c`) — basic-block translator. A block is decoded once; cheap ops (NOP, sized MOV reg↔reg/imm) are inlined as arm64, everything else calls back into the shared interpreter dispatch. RIP threading is automatic because each decoded insn carries its own rip. One 64MB `MAP_JIT` region with `pthread_jit_write_protect_np` and per-block icache invalidation; open-chained 65536-bucket cache keyed by guest rip. Every a64 encoding is validated by **execution**, not inspection.
- **Syscall forwarding** (`src/syscall.c`) — BSD class-2 syscalls go to the native arm64 kernel via `svc #0x80` with per-syscall pointer-mask translation; Mach traps and `kernelrpc` vm calls are forwarded or intercepted onto the arena; machdep trap 3 sets the guest's TLS base.

**The `guest_base` trick:** arm64 macOS refuses any mapping below the ASLR-slid main executable and (on 26.5) ignores non-`MAP_FIXED` placement hints, so the guest's `0x100000000` cannot be identity-mapped. Instead Ocerz reserves the entire guest range `[0x100000000, 0x900000000)` as one hintless `PROT_NONE` region and derives `guest_base = host_base − arena_lo`; every guest address `G` is translated through `g2h(G) = G + guest_base`, and `MAP_FIXED` is then safe inside the owned reservation.

## What does NOT work yet

Honest shortlist:

- **Dynamic binaries / dyld** — only static `LC_UNIXTHREAD` runs; `LC_MAIN`/dyld binaries are rejected (next phase: map `dyld_shared_cache_x86_64` manually + intercept the shared-region syscalls).
- **Threads** — single-threaded only; `bsdthread_create` is a loud FATAL.
- **Signals** — `sigaction`/`sigprocmask`/`sigaltstack` are recorded and faked-successful, but signals are never delivered.

Other rough edges: x87 is 64-bit double, not 80-bit; `RSQRT`/`RCP` are exact, not the ~12-bit approximations; MXCSR dynamic rounding is ignored (assumes round-to-nearest); the JIT block cache is never invalidated (no SMC support); guest `mprotect` is coarsened to 16KB unions; Mach OOL message descriptors are not yet translated.

## Build

Plain `Makefile`: `clang -arch arm64 -std=c11 -O2 -Wall -Wextra`. Guest tests are cross-compiled `-arch x86_64 -nostdlib -static` (`crt0.s` + `libmini.c` + raw syscalls). `make check` builds and runs everything.

## License

[Apache-2.0](LICENSE).

---


