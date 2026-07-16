<p align="center">
  <img width="1536" height="1024" alt="Ozlogo" src="https://github.com/user-attachments/assets/9a5cd4c8-9cf1-45cd-b4e1-7865adca40b3" />
</p>

<h1 align="center"><em>Ocerz</em></h1>

<p align="center">
  <b>A from-scratch x86_64 → arm64 userspace binary translator for macOS.</b><br>
  Zero Rosetta involvement.
</p>

<p align="center">
  <img alt="license" src="https://img.shields.io/badge/license-LGPL--2.1-blue.svg">
  <img alt="platform" src="https://img.shields.io/badge/platform-macOS%20·%20Apple%20Silicon-lightgrey.svg">
  <img alt="language" src="https://img.shields.io/badge/C-c11-orange.svg">
</p>

---

Ocerz runs x86_64 Mach-O binaries directly on Apple Silicon. It does its own loading, decoding, flag emulation, JIT translation, dynamic linking and syscall forwarding to the native arm64 kernel. **Rosetta 2 is never invoked.**

It started as a static-binary translator. It now brings up its **own dyld** — mapping the real `dyld_shared_cache_x86_64`, linking against the system frameworks, and driving the Objective-C runtime — which is enough to run **real, unmodified macOS applications**.

Ocerz ships **no Apple code**. It reimplements no framework. It links the guest against the x86_64 frameworks already installed on your own Mac, read at runtime from the system cryptex — the same way any program `dlopen`s a system library. Nothing is bundled or redistributed.

> **Dependency:** that x86_64 framework cache is the one macOS ships as part of Rosetta support, so Rosetta must be *installed*. It is never *invoked* — Ocerz does not execute a single instruction through it.

```sh
git clone https://github.com/mont127/Ocerz && cd Ocerz
make check                                                 # build + run every test
./ocerz tests/guest/bin/hello                              # a static guest binary
./ocerz /Applications/SomeApp.app/Contents/MacOS/SomeApp   # a real x86_64 app
```

## Highlights

- **Its own everything.** Loader, decoder, interpreter, JIT, dynamic linker, syscall layer. No Rosetta, no QEMU, no LLVM.
- **Bit-exact flags.** `flags.c` is an eager, bit-for-bit x86 reference the JIT must match — including architecturally-undefined flags, ADC/SBB carry-in relations, and x86 NaN semantics. A differential suite proves interpreter ≡ JIT.
- **A real dyld.** Maps the live shared cache at slide 0, walks export tries (re-exports, absolute symbols), applies chained fixups, runs initializers in dependency order, and performs the full objc↔dyld handshake.
- **Runs real apps.** Real Cocoa apps reach window creation on the live WindowServer.
- **More faithful than Rosetta, in places.** See the note under [Status](#status).
- **Encoders validated by execution.** Every arm64 encoding is proven by running it, not by reading it.

## Status

`make check` is green:

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

End-to-end: **14 / 14** guest binaries in interpreter mode, **14 / 14** in JIT mode, **15 / 15** in the JIT-vs-interpreter differential, and **2 / 2** dynamic-linking tests.

JIT speedup on `fib(30)`: **0.60s → 0.13s (~4.6×)**.

### Real applications

Ocerz runs real x86_64 Cocoa apps end-to-end with zero translator faults.

To be precise about what that means: Ocerz does not reimplement any framework. Everything above the translator is **Apple's own real x86_64 code, executed under translation** straight out of the shared cache — libobjc (classes, categories, `+load`, protocols), Swift, libdispatch, XPC, Foundation, AppKit. The only piece Ocerz supplies in that stack is **dyld**: it maps the real `dyld_shared_cache_x86_64`, resolves and fixes up symbols, runs the initializers, and answers the dyld API calls those frameworks make. That is what makes the genuine AppKit run.

`Mousecape.app` reaches **window creation on the live WindowServer**: its main window comes up at **711×342** against a native Rosetta control's **711×339**, alongside AppKit's four menu-bar windows at pixel-identical geometry.

This is early — the window is not yet interactive. See [What does NOT work yet](#what-does-not-work-yet).

> **Ocerz is more faithful than Rosetta for the test binaries.** The freestanding (`-nostdlib`, raw-syscall) guest tests actually *drop output lines* under Rosetta 2 — SSE printed 21 of 52 lines, even with vectorization off. Ocerz's output matches independent real-libc oracles (arm64-native and x86_64+printf) byte-for-byte. Goldens were regenerated from those oracles.

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

| Tier | Source | What it does |
| --- | --- | --- |
| **Loader** | `loader.c` | Parses `LC_UNIXTHREAD` and `LC_MAIN` Mach-O, maps segments, builds the stack. |
| **Decoder** | `decode.c` | x86_64 → internal `X86Insn` (the `OcerzOp` enum has 397 values). |
| **Interpreter** | `interp*.c`, `flags.c` | GP core, SSE through an SSE4.1 subset (plus SSE3 `ADDSUBPS/PD`), x87-on-doubles, CPUID/RDTSC. |
| **JIT** | `jit.c`, `a64emit.c` | Call-threaded basic-block translator emitting native arm64. |
| **Mini-dyld** | `dyld.c`, `cache.c`, `dyldapi.c` | Shared cache, symbol resolution, fixups, initializers, the dyld API surface, objc handshake. |
| **Syscalls** | `syscall.c` | BSD syscalls, Mach traps, signals, threads. |

**Eager flags.** Flags are evaluated eagerly into `cpu->rflags`; `flags.c` is the bit-for-bit reference the JIT must match — ADC/SBB folded via carry-in relations, INC/DEC preserving CF, deterministic values for architecturally-undefined flags, and x86 NaN semantics (negative QNaN indefinite, propagation rules).

**The JIT.** A block is the straight-line run from an entry rip to the first control-flow or system instruction. Each block is decoded once; cheap ops are inlined as arm64 and everything else calls back into the shared interpreter dispatch, so the JIT and interpreter can never disagree on semantics. One 64MB `MAP_JIT` region with `pthread_jit_write_protect_np` and per-block icache invalidation; a 65536-bucket cache keyed by guest rip with a lock-free lookup on the hot path. Aligned guest loads/stores are emitted as `ldar`/`stlr`, so x86's TSO ordering survives arm64's weak memory model.

**The mini-dyld.** Maps the real `dyld_shared_cache_x86_64` at slide 0 and implements the dyld runtime API surface that libdyld's trampolines dispatch through — `dlopen`/`dlsym`, image lists, TLV, unwind, `_dyld_register_for_bulk_image_loads` — plus the objc↔dyld handshake (`_dyld_objc_register_callbacks`, `map_images`/`load_images`, and the shared cache's selector and class perfect-hash tables).

**Syscalls.** BSD class-2 syscalls go to the native arm64 kernel via `svc #0x80` with per-syscall pointer-mask translation. Mach traps and `kernelrpc` vm calls are forwarded or intercepted onto the arena; Mach message OOL/port descriptors are translated in both directions. Guest signals are delivered with a faithful Darwin signal frame, and guest threads become real host threads, bridged to the host workqueue for libdispatch.

**Address translation.** Every guest address `G` maps to host `G + ocerz_guest_base`. Static binaries get a non-zero `guest_base` (the QEMU technique): Ocerz reserves the guest range as one hintless `PROT_NONE` region, because arm64 macOS refuses mappings below the ASLR-slid main executable and ignores non-`MAP_FIXED` placement hints. Dynamic binaries instead run **identity-mapped** (`guest_base = 0`, `g2h(G) = G`) so the shared cache sits at its unslid addresses, with a separate 12GB **low-shadow** reservation backing guest addresses below `OCERZ_LOW_LIMIT`.

## What does NOT work yet

An honest shortlist:

- **Real apps are slow, and their windows are not interactive.** A real Cocoa app takes minutes to reach its window, then pegs the CPU. Profiling shows ~44% of steady-state runtime inside `translate()`: a block that fails to translate is never remembered, so every later execution of that rip re-takes the global `jit_lock` and re-decodes it. Under investigation.
- **Wine** — bring-up is in progress (see `notes/wine_bringup.md`); `wineboot` runs end-to-end into the GUI layer.
- **Post-boot image loads** — categories and `+load` run for the launch closure, but later batch loads (post-boot `dlopen`) are not yet re-notified through `_dyld_register_for_bulk_image_loads`.

Other rough edges: x87 is 64-bit double, not 80-bit; `RSQRT`/`RCP` are exact rather than the ~12-bit approximations; MXCSR dynamic rounding is ignored (assumes round-to-nearest); the JIT block cache is never invalidated (no self-modifying-code support); guest `mprotect` is resolved onto 16KB host pages (permissive changes round outward, restrictive inward, so a shared page keeps the union); the stack guard is fixed rather than randomized; inbound OOL relocation covers flat Mach messages but not `MACH64_MSG_VECTOR` receives.

## Build

Plain `Makefile`: `clang -arch arm64 -std=c11 -O2 -Wall -Wextra`. Guest tests are cross-compiled `-arch x86_64 -nostdlib -static` (`crt0.s` + `libmini.c` + raw syscalls). `make check` builds and runs everything.

## License

[LGPL-2.1](LICENSE).

---

> [!WARNING]
> This project is experimental and was vibecoded. Use at your own risk.
