# Performance

AArchX is measured against Rosetta, on the same machine, running the same x86-64
binary. **Ratio is AArchX time divided by Rosetta time, so lower is better and
anything under 1.00x means AArchX is faster.**

## Method

`tests/xbench_compare.py` runs fifteen kernels. For each one it first calibrates
a size so that Rosetta takes about a third of a second, then times both engines
at that size and at half of it and takes the difference. Subtracting the
half-size run removes process startup and translation warm-up from the
measurement, so what is left is the steady-state cost of the work itself. Each
kernel is run five times and the median is reported, and both engines' output is
compared byte for byte, so a kernel that is fast because it computed the wrong
answer fails rather than scores.

```sh
python3 tests/xbench_compare.py                     # static build, plain memory
OCERZ_NO_PLAIN_MEM=1 python3 tests/xbench_compare.py   # ordered memory
XB=tests/guest/benchbin/xbench_dyn python3 tests/xbench_compare.py   # linked against libSystem
XB=tests/guest/benchbin/xbench_dyn OCERZ_MODE=native python3 tests/xbench_compare.py
```

## Results

Apple M5, macOS 27, 21 September 2026, five repetitions.

| Kernel | What it does | static | static, ordered | dynamic | dynamic, ordered | dynamic, native mode |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `icall` | indirect calls through a table | **0.92x** | **0.98x** | **0.93x** | **0.82x** | **0.84x** |
| `jtab` | a switch compiled to a jump table | **0.89x** | **0.86x** | **0.98x** | **0.92x** | **0.97x** |
| `depchain` | a dependent chain of arithmetic | **0.90x** | **0.90x** | **0.90x** | **0.90x** | **0.90x** |
| `brmiss` | unpredictable branches | **0.97x** | **0.99x** | **0.97x** | **0.98x** | **0.96x** |
| `memcpy` | copies of random small sizes | 1.04x | 1.22x | **0.72x** | **0.73x** | **0.73x** |
| `str` | `strlen` and `strcmp` on short strings | 1.05x | 1.16x | **0.77x** | **0.96x** | **0.82x** |
| `hash` | multiply-shift hashing | 1.00x | 1.00x | **0.99x** | **0.99x** | **0.99x** |
| `idiv` | integer division | **0.99x** | **0.99x** | **0.99x** | **0.99x** | **0.99x** |
| `fpsse` | scalar SSE floating point | **0.86x** | **0.88x** | **0.86x** | **0.86x** | **0.85x** |
| `fpvec` | packed single-precision arithmetic | **0.82x** | **0.82x** | **0.80x** | **0.83x** | 1.05x |
| `chase` | pointer chasing through a shuffled list | 1.00x | 1.23x | 1.00x | 1.22x | 1.00x |
| `qsort` | sorting with a comparator | **0.98x** | 1.04x | **0.97x** | 1.04x | **0.97x** |
| `leafcall` | a tight loop of small function calls | 1.03x | 1.02x | 1.03x | 1.03x | 1.03x |
| `mixed` | struct updates, branches and small loops | **0.88x** | **0.88x** | **0.83x** | **0.80x** | **0.90x** |
| `vm` | a bytecode interpreter loop | **0.85x** | **0.83x** | **0.84x** | **0.76x** | **0.73x** |

The **static** columns are a freestanding binary that makes no library calls, so
every instruction measured is translated code. The **dynamic** columns are the
same source linked against libSystem, which is what a real program looks like:
its `memcpy` and `str` kernels spend their time in the system's own routines.
**Ordered** is `OCERZ_NO_PLAIN_MEM=1`, the stricter memory model every program
with a second thread runs under. **Native mode** binds against the Mac's own
arm64 frameworks.

The dynamically linked build wins fourteen of fifteen kernels in cache mode. The
static build wins eleven and loses four, none by more than five percent.

An older run on an Apple M2 Max, in September 2026, gave thirteen wins and two
ties on the static build; the numbers above supersede it.

## What makes it fast

- **The guest's registers stay in registers.** All sixteen general registers and
  all sixteen vector registers live in arm64 registers for a whole block, with
  one layout shared by every block, so control passes from one block's body
  straight into another's without touching memory.
- **Flags are computed where they are read**, not where x86 says they are
  written, which is nearly every arithmetic instruction. A block whose flags die
  unread computes none of them.
- **Blocks are chained and grown.** Direct branches replace dispatch, a host
  return stack keeps the processor's return predictor aligned with the guest's
  calls, and a block can continue past a conditional branch with the rare side
  out of line.
- **The processor produces x86's NaNs.** Where `FEAT_AFP` is available, one bit
  makes arm64 arithmetic follow x86's NaN rules, and the checks that otherwise
  guard every floating-point result disappear. That is the whole of `fpvec`'s
  0.82x: the check was three vector operations on top of the eight doing the
  work, and the vector pipelines were the limit.
- **Hot library routines do not go through the library.** `strlen`, `memcpy`,
  `memset` and eight others are hand-written arm64 code that runs with the
  guest's registers in place. In native mode that turns a 17 ns bridged call
  into a 2 ns branch; in cache mode it replaces Apple's translated x86 routine.

## What is still slower

- **`leafcall`**, about 3% behind everywhere. Both calls are already inlined
  into a single translated block; what is left is the frame bookkeeping x86's
  calling convention asks for.
- **`chase` under ordered memory**, 1.22x. A dependent load through a shifted
  index is a single instruction on x86 and two on arm64, because arm64's
  acquiring loads have no register-index form. It is an instruction-set floor,
  not a missing optimisation.
- **The static `memcpy` and `str` under ordered memory**, 1.22x and 1.16x. These
  are the program's own byte loops at arbitrary alignments; an ordered access
  that straddles a 16-byte boundary needs a barrier.
- **`fpvec` in native mode**, 1.05x, because native mode keeps the software NaN
  checks. Clearing and restoring the floating-point control bit around every
  crossing would cost about as much as the crossing itself.

## The cost of a native-mode crossing

Measured as a guest loop making ten million calls and timing itself, on the same
M5. The arm64 column is the same loop compiled for arm64 and run directly.

| guest loop, ns per iteration | through the trap | default | arm64 build |
| --- | ---: | ---: | ---: |
| `getpid` | 40.5 | 17.0 | 0.7 |
| `strlen` of a 12-byte string | 42.0 | 2.0 | 0.9 |
| `memcpy` of 48 bytes | 45.6 | 3.0 | 2.7 |
| `-[NSString length]` | 51.3 | 28.3 | 3.6 |
| a `qsort` comparator, per call back into guest code | 426 | 430 | 3.1 |

The `strlen` and `memcpy` rows are low because those routines do not cross at
all. With `OCERZ_NO_LEAF_INPLACE=1` they read 17.3 and 20.8, which is what an
ordinary crossing costs.

Calling back **into** guest code is the expensive direction, and it is dominated
by the signal state that a callback's fault-recovery point has to save and
restore.

## Reading these numbers honestly

These are microbenchmarks. They isolate one thing each, which is what makes them
useful for finding costs, and it is also what makes them a poor predictor of how
long a real application takes to start or how smoothly it runs. A kernel within
a couple of percent of 1.00x is a tie: it will land on either side of it from run
to run, and a busy machine moves every number by that much.
