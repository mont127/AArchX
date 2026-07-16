# Ocerz



**A from-scratch x86_64 → arm64 userspace binary translator for macOS - zero Rosetta involvement.**

Ocerz runs x86_64 Mach-O binaries directly on Apple Silicon. It does its own loading, decoding, flag emulation, JIT translation, dynamic linking and syscall forwarding to the native arm64 kernel. Rosetta 2 is never invoked.

It started as a static-binary translator. It now brings up its **own dyld**: it maps the real `dyld_shared_cache_x86_64`, links against the system frameworks, drives the objc runtime, and runs **real, unmodified macOS applications**.

## Status

Full `make check` is green:

| Suite | Result |
| --- | --- |
| `a64emit` (run-don't-read encoder) | all encodings validated |
| corpus length validation | 511 / 511 |
| decode | 190 cases |
| cache (shared-cache map/resolve) | 14 |
| ext (strings/bits/CPUID/x87) | 165 |
| interp | all assertions |
| loader | 54 |
| sse | 246 |
| syscall | 96 |

End-to-end: **14 / 14** guest binaries pass in interpreter mode, **14 / 14** in JIT mode, **15 / 15** in the JIT-vs-interpreter differential, and **2 / 2** dynamic-linking tests.

Measured JIT speedup on `fib(30)`: **0.60s → 0.13s (~4.6×)**.

### Real applications

Ocerz runs real x86_64 Cocoa apps end-to-end with zero translator faults: its own dyld + shared cache, objc (classes, categories, `+load`, protocols), Swift metadata, libdispatch workqueues, XPC, and AppKit. A real app (`Mousecape.app`) reaches **window creation on the live WindowServer** — its main window comes up at 711x342 against a native Rosetta control's 711x339, alongside AppKit's four menu-bar windows at pixel-identical geometry.

This is early. See *What does NOT work yet*.

> Note: the freestanding (`-nostdlib`, raw-syscall) guest test binaries actually *drop output lines* when run under Rosetta 2 (e.g. SSE printed 21 of 52 lines), even with vectorization off. Ocerz's output matches independent real-libc oracles (arm64-native and x86_64+printf) byte-for-byte — Ocerz is more faithful than Rosetta for these binaries. Goldens were regenerated from those oracles.

## Quickstart

```sh
git clone https://github.com/mont127/Ocerz && cd Ocerz-main
make check                              # build + run all unit and e2e tests
./ocerz tests/guest/bin/hello           # run a static guest binary (JIT by default)
./ocerz /Applications/SomeApp.app/Contents/MacOS/SomeApp   # run a real x86_64 app
```

## CLI

```
usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-path file] [--] program [args...]
```

| Flag | Effect |
| --- | --- |
| `-v` | verbose (repeatable, raises level) |
| `-trace` | per-instruction trace |
| `-strace` | syscall trace |
| `-no-jit` | force the interpreter (JIT is on by default) |
| `--` | end of Ocerz options; everything after is guest `argv` |

## Architecture

- **Loader** (`src/loader.c`) — parses `LC_UNIXTHREAD` and `LC_MAIN` Mach-O, maps segments, sets up the stack.
- **Decoder** (`src/decode.c`) — x86_64 → internal `X86Insn` (op widened to `uint16_t`; the `OcerzOp` enum has 397 values).
- **Eager-flags interpreter** (`src/interp*.c`, `src/flags.c`) — GP core, full SSE through an SSE4.1 subset (plus SSE3 `ADDSUBPS/PD`), x87-on-doubles, CPUID/RDTSC. Flags are evaluated **eagerly** into `cpu->rflags`; `flags.c` is the bit-for-bit reference the JIT must match (ADC/SBB folded via carry-in relations, INC/DEC preserve CF, deterministic values for architecturally-undefined flags). x86 NaN semantics are reproduced (negative QNaN indefinite, propagation rules).
- **Call-threaded JIT** (`src/jit.c`, `src/a64emit.c`) — basic-block translator. A block is decoded once; cheap ops are inlined as arm64, everything else calls back into the shared interpreter dispatch. One 64MB `MAP_JIT` region with `pthread_jit_write_protect_np` and per-block icache invalidation; open-chained 65536-bucket cache keyed by guest rip, with a lock-free lookup on the hot path. Guest stores/loads are emitted as `stlr`/`ldar` when aligned so x86's TSO ordering survives arm64's weak memory model. Every a64 encoding is validated by **execution**, not inspection.
- **Mini-dyld** (`src/dyld.c`, `src/cache.c`, `src/dyldapi.c`) — maps the real `dyld_shared_cache_x86_64` at slide 0, walks export tries (re-exports and absolute symbols included), applies chained fixups, loads non-cache disk dylibs, runs initializers in dependency order, and implements the dyld runtime API surface that libdyld's trampolines dispatch through (`dlopen`/`dlsym`, image lists, TLV, unwind, `_dyld_register_for_bulk_image_loads`) plus the objc↔dyld handshake (`_dyld_objc_register_callbacks`, `map_images`/`load_images`, the shared cache's selector and class perfect-hash tables).
- **Syscall forwarding** (`src/syscall.c`) — BSD class-2 syscalls go to the native arm64 kernel via `svc #0x80` with per-syscall pointer-mask translation; Mach traps and `kernelrpc` vm calls are forwarded or intercepted onto the arena; Mach message OOL/port descriptors are translated in both directions; guest signals are delivered with a faithful Darwin signal frame; guest threads become real host threads, bridged to the host workqueue for libdispatch.

**Address translation:** every guest address `G` maps to host `G + ocerz_guest_base`. For static binaries Ocerz reserves the guest range as one hintless `PROT_NONE` region and derives a non-zero `guest_base` (the QEMU technique), because arm64 macOS refuses mappings below the ASLR-slid main executable and ignores non-`MAP_FIXED` placement hints. Dynamic binaries instead run **identity-mapped** (`guest_base = 0`, `g2h(G) = G`) so the shared cache sits at its unslid addresses, with a separate 12GB **low-shadow** reservation backing guest addresses below `OCERZ_LOW_LIMIT`.

## What does NOT work yet

Honest shortlist:

- **Real apps are slow, and their windows are not yet interactive.** A real Cocoa app takes minutes to reach its window and then pegs the CPU. Profiling shows ~44% of steady-state runtime inside `translate()`: a block that fails to translate is never remembered, so every later execution of that rip re-takes the global `jit_lock` and re-decodes it. Under investigation.
- **Wine** — bring-up is in progress (see `notes/wine_bringup.md`); `wineboot` runs end-to-end into the GUI layer.
- **Post-boot image loads** — categories and `+load` now run for the launch closure, but later batch loads (post-boot `dlopen`) are not yet re-notified through `_dyld_register_for_bulk_image_loads`.

Other rough edges: x87 is 64-bit double, not 80-bit; `RSQRT`/`RCP` are exact, not the ~12-bit approximations; MXCSR dynamic rounding is ignored (assumes round-to-nearest); the JIT block cache is never invalidated (no SMC support); guest `mprotect` is resolved onto 16KB host pages (permissive changes round outward, restrictive changes inward, so a shared page keeps the union); the stack guard is a fixed value rather than randomized; inbound OOL relocation covers flat Mach messages, not `MACH64_MSG_VECTOR` receives.

## Build

Plain `Makefile`: `clang -arch arm64 -std=c11 -O2 -Wall -Wextra`. Guest tests are cross-compiled `-arch x86_64 -nostdlib -static` (`crt0.s` + `libmini.c` + raw syscalls). `make check` builds and runs everything.

## License

[Apache-2.0](LICENSE).

---
> [!WARNING]
> This project is experimental and was vibecoded. Use at your own risk.
