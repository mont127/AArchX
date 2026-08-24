<p align="center">
  <img width="460" alt="Ocerz" src="https://github.com/user-attachments/assets/9a5cd4c8-9cf1-45cd-b4e1-7865adca40b3" />
</p>

<h1 align="center"><em>Ocerz</em></h1>

<p align="center">
  <b>A from-scratch x86-64 to arm64 userspace binary translator for macOS.</b><br>
  <sub>No Rosetta translation at runtime.</sub>
</p>

<p align="center">
  <img alt="license" src="https://img.shields.io/badge/license-LGPL--2.1-blue.svg">
  <img alt="platform" src="https://img.shields.io/badge/platform-macOS%20Apple%20Silicon-lightgrey.svg">
  <img alt="language" src="https://img.shields.io/badge/C-C11-orange.svg">
</p>

> [!WARNING]
> Ocerz is experimental. Do not use it for production workloads.

Ocerz loads and executes x86-64 Mach-O programs on Apple Silicon with its own decoder, interpreter, JIT, dynamic linker, and syscall layer. It also executes i386 PE code inside Wine's WoW64 process.

The Rosetta package must be installed because it supplies macOS's x86-64 shared cache. Ocerz maps that cache itself and does not invoke Rosetta's translator.

## Build And Run

```sh
make -j
./ocerz tests/guest/bin/hello
./ocerz /Applications/SomeApp.app/Contents/MacOS/SomeApp
```

Run the main gates with:

```sh
make check
make i386diff
```

`i386diff` requires the Python `capstone` package.

## Status

| Area | Current result |
| --- | --- |
| arm64 emitter | encodings validated by execution |
| instruction corpus | 511 instructions |
| x86-64 decode | 199 / 199 cases |
| i386 decode | 102 cases, 26 rejects, 122 address cases |
| extension / SSE suites | 233 / 0, 246 / 0 |
| loader / syscall suites | 54 / 0, 250 / 0 |
| memory / shared mappings | 2690 / 0, 91 / 0 |
| i386 interpreter / JIT / WoW64 | passing |
| x86-64 guest gate | 65 / 65 |
| x86-64 differential gate | 53 / 53 |
| i386 differential gate | 20,032 / 20,032 |

Highlights:

- From-scratch Mach-O loader, x86 decoder, interpreter, arm64 JIT, mini-dyld, and syscall layer.
- Live `dyld_shared_cache_x86_64` mapping, fixups, initializers, Objective-C registration, and `dlopen`/`dlsym` support.
- Native guest threads, libdispatch workqueue bridging, Mach messages, signals, and x86-TSO memory ordering.
- JIT cache invalidation for guest code writes and executable mapping changes.
- Differential tests for x86-64 and i386 execution.

## Wine And i386

Wine 11.8 runs x86-64 and i386 PE applications through Ocerz. With a WoW64 prefix, 32-bit Notepad and WineMine load `winemac.drv` and create titled Cocoa windows. Wine launchers automatically enable the workqueue bridge and the targeted Objective-C category preload required by AppKit.

```sh
WINE="/path/to/Wine Devel.app/Contents/Resources/wine"
export WINEARCH=wow64
export WINEPREFIX="$HOME/.wine-ocerz"

./ocerz "$WINE/bin/wine" wineboot -u
./ocerz "$WINE/bin/wine" notepad
```

Rosetta also runs i386 PE code through Wine WoW64. Ocerz's i386 support is replacement parity, not an exclusive capability. Standalone i386 Mach-O applications are not supported by current macOS SDKs or macOS itself.

## Benchmarks

Ratio is Ocerz time divided by Rosetta time. Lower is better.

| Kernel | Static | Dynamic |
| --- | ---: | ---: |
| `depchain` | **0.85x** | **0.85x** |
| `jtab` | **0.93x** | **0.90x** |
| `memcpy` | **0.93x** | **0.97x** |
| `brmiss` | **0.96x** | **0.96x** |
| `fpvec` | **0.96x** | **0.96x** |
| `qsort` | **0.98x** | 1.00x |
| `str` | **0.99x** | 1.04x |
| `icall` | **0.99x** | **0.99x** |
| `chase` | 1.00x | **0.97x** |
| `hash` | 1.00x | 1.01x |
| `vm` | 1.00x | **0.96x** |
| `idiv` | 1.05x | 1.03x |
| `leafcall` | 1.18x | 1.17x |
| `fpsse` | 1.21x | 1.20x |
| `mixed` | 1.22x | 1.21x |

Apple M2 Max, 2026-08-17, `REPS=5`, paired delta `t(n) - t(n/2)`, byte-identical output. Reproduce with `python3 tests/xbench_compare.py`; set `XB=tests/guest/benchbin/xbench_dyn` for the dynamic binary.

## CLI

```text
usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-path file] [--] program [args...]
```

| Option | Effect |
| --- | --- |
| `-v` | increase logging level |
| `-trace` | trace guest instructions |
| `-strace` | trace guest syscalls |
| `-no-jit` | interpret this process only |
| `-path file` | load `file` while preserving the following guest arguments |
| `--` | end Ocerz option parsing |

| Environment | Effect |
| --- | --- |
| `OCERZ_NOJIT=1` | interpret the entire process tree |
| `OCERZ_NOJIT_EXE=<text>` | interpret matching process command lines |
| `OCERZ_HOSTWQ=1` | enable the host workqueue bridge |
| `OCERZ_NO_PLAIN_MEM=1` | use ordered memory forms from startup |
| `OCERZ_TSO_STRICT=1` | order stack-relative memory accesses too |
| `OCERZ_PRELOAD_OBJC=<paths>` | preload matching shared-cache Objective-C images |

## Architecture

| Component | Source | Responsibility |
| --- | --- | --- |
| Loader | `src/loader.c` | Mach-O parsing, mappings, initial stack |
| Decoder | `src/decode.c` | x86-64/i386 to the 411-operation internal IR |
| Interpreter | `src/interp*.c`, `src/flags.c` | reference execution and x86 flag semantics |
| JIT | `src/jit.c`, `src/a64emit.c` | native arm64 code generation and linking |
| Mini-dyld | `src/dyld.c`, `src/cache.c`, `src/dyldapi.c` | shared cache, symbols, fixups, Objective-C |
| Syscalls | `src/syscall.c` | BSD, Mach, signals, threads, and WoW64 host calls |

## Limitations

- Application compatibility is incomplete; unsupported syscalls and framework behavior remain.
- Generic late-loaded shared-cache Objective-C images are not fully registered. Wine uses a targeted compatibility preload.
- x87 uses 64-bit doubles instead of 80-bit extended precision.
- Dynamic MXCSR rounding modes and approximate `RCP`/`RSQRT` results are not implemented.
- Guest protection changes are resolved on the host's 16 KB page boundaries.

## License

[LGPL-2.1](LICENSE)
