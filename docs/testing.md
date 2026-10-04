# Testing

```sh
make check
```

builds the unit and guest tests and runs every gate in order. It takes roughly
half an hour and prints a count for each phase. A single phase can be run on its
own:

```sh
make unit
bash tests/run_guest_tests.sh --no-jit
bash tests/run_guest_tests.sh
bash tests/run_diff_test.sh
bash tests/run_diff32.sh .
bash tests/run_dynamic_tests.sh
bash tests/run_native_tests.sh
bash tests/run_native_swift_tests.sh
```

## What each phase proves

**Unit harnesses.** Twenty-nine programs that link the translator's own object
files and drive one component directly: the instruction encoder, the decoder,
the interpreter, the ABI engine, the API database parser, the Objective-C
bridge, the block runtime. Several are exhaustive rather than illustrative — the
ABI engine is checked over twenty-eight thousand argument arrangements, and the
in-place string and memory routines against the C library over six million
inputs, with every string placed against an inaccessible page so that a routine
reading one byte too far dies here instead of in somebody's program.

**The guest suite, twice.** More than a hundred freestanding x86-64 binaries are
run under the interpreter and again under the JIT, and each one's output is
compared with a stored golden. Several are run a third and fourth time under
different settings, because each configuration reaches code the others cannot: a
stricter memory model, a code arena small enough that almost every block takes
the interpreted path, and the floating-point tests with every deferred check
forced to take its slow arm.

**The x86-64 differential gate.** Every guest binary is run interpreted and
translated and the two must agree byte for byte. The interpreter is the
reference, so this is the gate that catches a JIT that is fast and wrong.

**The i386 differential gate.** Twenty thousand generated instruction sequences
plus a set of hand-written ones are run both ways, comparing all sixteen general
registers at full width, the flags, the instruction pointer, the mode, the
segment selectors, the whole floating-point and vector register file, and every
byte of memory that was touched. It fails rather than passes if the JIT stops
translating 32-bit code, so it cannot quietly degrade into a second interpreter
run.

**The dynamic suite.** Real dynamically linked Mach-O programs built against the
system libraries, covering the parts a freestanding binary never reaches:
threads, signals and signal stacks, `fork` and `exec`, file and socket I/O,
`dlopen` and the dyld APIs, atomics and memory ordering, AVX2 and FMA, C++
exceptions, and writing code into a mapping and executing it. Each runs under
both engines.

**The native suite.** The largest single gate. It builds x86-64 fixtures and
runs each under native mode with the JIT, under native mode interpreted, and
under cache mode, and requires all three to print the same thing as a separately
compiled arm64 build of the same source. That comparison is the point: a bridged
call that returns plausible nonsense would pass a test that only checked the
exit code. It also pins the refusals — the calls native mode declines to make
rather than making wrongly — by name and by exit code.

**The native Swift suite.** Two Swift fixtures, compiled for x86-64 and arm64
from the same source, must print the same thing under native mode, with the JIT
and interpreted, as the arm64 build does running as itself. One covers the
language, the other the end of an object's life, including the last release
coming from native code. It needs the guest Swift runtime (`make guest-swift`)
and skips without it.

## Current state

On an Apple M5 running macOS 27, in September 2026:

| Phase | Result |
| --- | --- |
| Unit harnesses | all pass |
| Guest suite, interpreted | 131 / 131 |
| Guest suite, translated | 131 / 131 |
| x86-64 differential | 97 / 97 |
| i386 differential | 20,033 / 20,033 |
| Dynamic suite | 113 / 113 |
| Native suite | 87 / 87 |

The component suites inside the unit phase, as last counted in September 2026
(passes / failures):

| Suite | Result |
| --- | --- |
| arm64 emitter | encodings validated by execution |
| instruction corpus | 511 instructions |
| x86-64 decode | 246 / 246 cases |
| i386 decode | 102 cases, 26 rejects, 122 address cases |
| extension and SSE suites | 237 / 0, 246 / 0, SSE4.2 differential against Rosetta |
| loader, syscall | 54 / 0, 365 / 0 |
| memory, shared mappings | 2692 / 0, 105 / 0 |
| native mode: API database, image, bridge, ABI, callbacks, thread attach, Objective-C, blocks | 334 / 0, 142426 / 0, 1247 / 0, 28071 / 0, 131974 / 0, 270 / 0, 125400 / 0, 107 / 0 |
| in-place string and memory routines against the host's | 6,736,902 / 0 |
| xbench output against native | 15 / 15 kernels bit-identical |

## Conventions

The test binaries under `tests/guest/bin` and `tests/unit/bin` are committed,
not built on demand, so that a change to the translator can be compared against
exactly the binaries the previous run used. They are refreshed by their own
commits.

Benchmarks live in `tests/guest/benchbin`, deliberately apart from the test
binaries, because both gates glob the test directory and a benchmark there would
silently become an extra differential case.
