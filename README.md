<p align="center">
  <img width="460" alt="Ocerz" src="https://github.com/user-attachments/assets/9a5cd4c8-9cf1-45cd-b4e1-7865adca40b3" />
</p>

<h1 align="center"><em>Ocerz</em></h1>

<p align="center">
  <b>A from-scratch x86_64 → arm64 userspace binary translator for macOS.</b><br>
  <sub>Zero Rosetta translation involvement.</sub>
</p>


> [!WARNING]
> This project is in BETA do not expect much of it in its current state. (ROSSETA IS STILL NEEDED)


<p align="center">
  <img alt="license" src="https://img.shields.io/badge/license-LGPL--2.1-blue.svg">
  <img alt="platform" src="https://img.shields.io/badge/platform-macOS%20·%20Apple%20Silicon-lightgrey.svg">
  <img alt="language" src="https://img.shields.io/badge/C-c11-orange.svg">
</p>

---

Ocerz runs x86_64 Mach-O binaries directly on Apple Silicon. It does its own loading, decoding, flag emulation, JIT translation, dynamic linking and syscall forwarding to the native arm64 kernel. Rosetta 2 is never invoked.

It started as a static-binary translator. It now brings up its own dyld, mapping the real `dyld_shared_cache_x86_64`, linking against the system frameworks and driving the Objective-C runtime, which is enough to run unmodified macOS applications.

Ocerz ships no Apple code and reimplements no framework. It links the guest against the x86_64 frameworks already on your Mac, read at runtime from the system cryptex, the same way any program `dlopen`s a system library. Nothing is bundled or redistributed.

> **Dependency:** that x86_64 framework cache is the one macOS ships as part of Rosetta support, so Rosetta must be *installed*. It is never *invoked* — Ocerz does not execute a single instruction through it.

```sh
git clone https://github.com/mont127/Ocerz && cd Ocerz
make check                                                 # build + run every test
./ocerz tests/guest/bin/hello                              # a static guest binary
./ocerz /Applications/SomeApp.app/Contents/MacOS/SomeApp   # a real x86_64 app
```

## Highlights

- Own loader, decoder, interpreter, JIT, dynamic linker and syscall layer. No Rosetta, no QEMU, no LLVM.
- `flags.c` is an eager bit-for-bit x86 flag reference, including the architecturally-undefined
  bits, ADC/SBB carry-in and x86 NaN semantics. A differential suite checks interpreter against JIT.
- The mini-dyld maps the live shared cache at slide 0, walks export tries, applies chained
  fixups, runs initializers in dependency order and does the objc/dyld handshake.
- Cocoa apps reach window creation against the real WindowServer.
- arm64 encodings are validated by executing them, not by reading the manual.

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

End-to-end: **17 / 17** guest binaries in interpreter mode, **17 / 17** in JIT mode, **18 / 18** in the JIT-vs-interpreter differential, and **2 / 2** dynamic-linking tests.

JIT speedup on `fib(30)`: **0.59s → 0.08s (~7.5×)**.

### Real applications

Ocerz runs real x86_64 Cocoa apps end-to-end with zero translator faults.

Nothing above the translator is reimplemented. libobjc (classes, categories, `+load`, protocols), Swift, libdispatch, XPC, Foundation and AppKit are Apple's own x86_64 code out of the shared cache, executed under translation. The one piece Ocerz supplies is dyld: it maps the cache, resolves and fixes up symbols, runs the initializers and answers the dyld API calls those frameworks make.

`Mousecape.app` reaches window creation on the live WindowServer. Its main window comes up at 711×342 against a native Rosetta control's 711×339, alongside AppKit's four menu-bar windows at pixel-identical geometry.

This is early. The window comes up on roughly half of launches; the other half hang before it appears. See [What does NOT work yet](#what-does-not-work-yet).

> **Ocerz is more faithful than Rosetta for the test binaries.** The freestanding (`-nostdlib`, raw-syscall) guest tests actually *drop output lines* under Rosetta 2 — SSE printed 21 of 52 lines, even with vectorization off. Ocerz's output matches independent real-libc oracles (arm64-native and x86_64+printf) byte-for-byte. Goldens were regenerated from those oracles.

## Benchmarks

Same x86_64 binary run under both engines, same machine, best of three:

| Kernel | Rosetta 2 | Ocerz |
| --- | --- | --- |
| `alu` (register ALU loop) | 0.49s | 0.49s |
| `br` (data-dependent branches) | 0.88s | 0.31s |
| `fib(42)` (call/return) | 0.67s | 1.09s |

Branch-heavy code wins from two-way block linking, which jumps straight between compiled
blocks instead of returning to the dispatcher. Both engines produce byte-identical output.
`tests/run_bench_compare.sh` reproduces it.

The wider `xbench` suite (`tests/guest/xbench.c`, 15 kernels) is measured with the
paired-delta method of `tests/xbench_compare.py`: each kernel is timed at a scale and at
half that scale under both engines, and the difference is compared, which cancels process
startup and JIT warm-up. Ratio is Ocerz time over Rosetta 2 time (lower is better; below
1.0 Ocerz is faster). Same static x86_64 binary, byte-identical output on every kernel:

| Kernel | What it exercises | Rosetta 2 | Ocerz | Ratio |
| --- | --- | --- | --- | --- |
| `depchain` | long dependent integer chains | 0.154s | 0.140s | **0.91x** |
| `jtab` | switch / jump-table dispatch | 0.256s | 0.247s | **0.96x** |
| `memcpy` | memcpy of mixed sizes | 0.287s | 0.276s | **0.96x** |
| `chase` | pointer chasing (cache latency) | 0.301s | 0.304s | 1.01x |
| `brmiss` | unpredictable branches | 0.200s | 0.204s | 1.01x |
| `str` | strlen/strcmp-style byte loops | 0.294s | 0.299s | 1.03x |
| `hash` | hashing (mul/xor/shift + loads) | 0.286s | 0.294s | 1.03x |
| `qsort` | recursive quicksort (compare + swap) | 0.293s | 0.299s | 1.04x |
| `fpvec` | packed single-precision loop (SSE) | 0.289s | 0.300s | 1.04x |
| `vm` | bytecode-interpreter dispatch loop | 0.156s | 0.166s | 1.07x |
| `icall` | indirect calls through a function table | 0.236s | 0.257s | 1.09x |
| `idiv` | 64/32-bit unsigned division | 0.258s | 0.286s | 1.11x |
| `fpsse` | scalar double chain with sqrt/div | 0.292s | 0.354s | 1.21x |
| `leafcall` | many small non-recursive calls | 0.295s | 0.369s | 1.24x |
| `mixed` | struct updates, FP compares, branches | 0.279s | 0.361s | 1.29x |

(Apple M2 Max, `REPS=7`, quiet machine; kernels within about 3% of 1.0 trade places from
run to run.)  Three kernels are faster than Rosetta 2, six more are within 5%, three within
11%, and the rest within 1.3x. What made the difference so far: full guest-register pinning
with body-to-body block chaining, lazy flags with static producer/consumer fusion
(`cmp`/`test`/shift results feed `jcc`, `setcc`, `cmov`, `adc` straight from NZCV, including
the narrow 8/16-bit forms), per-site caches for indirect branches, direct
`[guest_base, rsp]` addressing for push/pop/call/ret with the host `bl`/`ret` protocol,
hoisted and cached effective addresses (loop-invariant bases in registers, base+index
reused across neighbouring accesses), superblocks across forward branches,
patch-and-signal stop requests instead of interrupt polls in loops, and FP "batches": runs of SSE
arithmetic execute in place at native speed with one NaN check per run, replaying exactly
from a checkpoint only when a NaN shows up (x86 NaN semantics are still bit-exact, checked
by a 2,859-case golden test against Rosetta). Where Rosetta still wins it has hardware
help Ocerz lacks: an x86-flavoured FP mode on Apple silicon and identity-mapped guest
memory, and its ahead-of-time translation lays out call/return code without the per-call
return-stack bookkeeping a block JIT needs.

Loads and stores are plain `ldr`/`str` while the process is single-observer, and switch to
`ldar`/`stlr` for x86-TSO once guest memory can be seen by another thread or process.

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

**The JIT.** A block is the straight-line run from an entry rip to the first control-flow or system instruction. Each block is decoded once; cheap ops are inlined as arm64 and everything else calls back into the shared interpreter dispatch, so the JIT and interpreter can never disagree on semantics. A 1GB `MAP_JIT` reservation (address space, not RSS) with `pthread_jit_write_protect_np` and per-block icache invalidation; a 2^20-bucket cache keyed by guest rip with a lock-free lookup on the hot path. Stack traffic (`push`/`pop`) is inlined natively, since it is ~27% of everything a real app executes. If the arena ever fills, a block is demoted to the interpreter tier rather than retried forever. Aligned guest loads/stores use ordinary `ldr`/`str` while the process is provably single-observer and upgrade to `ldar`/`stlr` once guest memory can be shared with another thread or process, so x86's TSO ordering survives arm64's weak memory model without paying for barriers that single-threaded code cannot observe.

**The mini-dyld.** Maps the real `dyld_shared_cache_x86_64` at slide 0 and implements the dyld runtime API surface that libdyld's trampolines dispatch through — `dlopen`/`dlsym`, image lists, TLV, unwind, `_dyld_register_for_bulk_image_loads` — plus the objc↔dyld handshake (`_dyld_objc_register_callbacks`, `map_images`/`load_images`, and the shared cache's selector and class perfect-hash tables).

**Syscalls.** BSD class-2 syscalls go to the native arm64 kernel via `svc #0x80` with per-syscall pointer-mask translation. Mach traps and `kernelrpc` vm calls are forwarded or intercepted onto the arena; Mach message OOL/port descriptors are translated in both directions. Guest signals are delivered with a faithful Darwin signal frame, and guest threads become real host threads, bridged to the host workqueue for libdispatch.

**Address translation.** Every guest address `G` maps to host `G + ocerz_guest_base`. Static binaries get a non-zero `guest_base` (the QEMU technique): Ocerz reserves the guest range as one hintless `PROT_NONE` region, because arm64 macOS refuses mappings below the ASLR-slid main executable and ignores non-`MAP_FIXED` placement hints. Dynamic binaries instead run **identity-mapped** (`guest_base = 0`, `g2h(G) = G`) so the shared cache sits at its unslid addresses, with a separate 12GB **low-shadow** reservation backing guest addresses below `OCERZ_LOW_LIMIT`.

## What does NOT work yet

- A real app hangs on roughly half of launches. When it works, `Mousecape.app` reaches its window in ~19s and settles to 0% CPU. When it does not, the process parks at 0% CPU with no window and macOS reports "Application not responding": the main thread never services the run loop. Over 24 consecutive launches, 13 OK, 8 hung, 3 died. It is a lost-wakeup race in the libdispatch workloop bridge, where one workloop is armed (`EVFILT_WORKLOOP`, `EV_ADD|EV_ENABLE`) over 140 times, never drained, and everything parks. This is the single blocker to a usable app.
- Wine bring-up is in progress (see `notes/wine_bringup.md`); `wineboot` runs end-to-end into the GUI layer.
- Categories and `+load` run for the launch closure, but later batch loads (post-boot `dlopen`) are not yet re-notified through `_dyld_register_for_bulk_image_loads`.

Other rough edges: x87 is 64-bit double, not 80-bit; `RSQRT`/`RCP` are exact rather than the ~12-bit approximations; MXCSR dynamic rounding is ignored (assumes round-to-nearest); the JIT block cache is never invalidated (no self-modifying-code support); guest `mprotect` is resolved onto 16KB host pages (permissive changes round outward, restrictive inward, so a shared page keeps the union); the stack guard is fixed rather than randomized; inbound OOL relocation covers flat Mach messages but not `MACH64_MSG_VECTOR` receives.

## Build

Plain `Makefile`: `clang -arch arm64 -std=c11 -O2 -Wall -Wextra`. Guest tests are cross-compiled `-arch x86_64 -nostdlib -static` (`crt0.s` + `libmini.c` + raw syscalls). `make check` builds and runs everything.

## License

[LGPL-2.1](LICENSE).

---

> [!WARNING]
> This project is experimental and was vibecoded. Use at your own risk.
