# Architecture

AArchX is one process. The guest program is mapped into it, its instructions are
translated to arm64 and run on real threads, and everything the guest thinks is
the operating system is answered by AArchX.

| Component | Source | Responsibility |
| --- | --- | --- |
| Loader | `src/loader.c` | Mach-O parsing, mappings, the initial stack |
| Decoder | `src/decode.c` | x86-64 and i386 into a 548-operation internal representation |
| Interpreter | `src/interp*.c`, `src/flags.c` | reference execution and x86 flag semantics |
| JIT | `src/jit.c`, `src/a64emit.c` | arm64 code generation, block chaining, superblocks |
| Mini-dyld | `src/dyld.c`, `src/cache.c`, `src/dyldapi.c` | the shared cache, symbols, fixups, Objective-C, `dlopen` and `dladdr` |
| Virtual dylibs | `src/vdylib.c` | native mode's synthesized x86_64 system images and their stubs |
| API database | `src/apidb.c`, `runtime/apis/`, `tools/sdkgen/` | native mode's export lists and signatures, generated from the SDK |
| Bridge | `src/bridge.c` | native mode's crossings, and which call a thread is in when it faults |
| System bridge | `src/sysbridge.c` | the libSystem calls a generic crossing cannot make |
| Objective-C bridge | `src/objcbridge.c`, `src/objcclass.c` | message sends both ways, and the guest's own classes |
| ABI engine | `src/abi.c`, `src/abicall.s` | System V x86-64 to Apple arm64 in both directions |
| Blocks | `src/blocks.c` | native mode's blocks in both directions |
| Syscalls | `src/syscall.c` | BSD, Mach, signals, threads, and Wine's WoW64 calls |

## Guest memory

The guest gets a large reserved region of the host address space, and a guest
address is turned into a host address by adding a base. Which base depends on
how the program is loaded. A statically linked program gets an arena at a fixed
guest address with a non-zero offset. A dynamically linked one gets an identity
mapping, where guest addresses *are* host addresses and the translation drops
the add entirely, which is the fast case and the usual one. Wine, whose main
image insists on living at a low address the host will not hand out, gets a
third mapping with a separate base for the low region.

Guest pages are 4 KB and host pages are 16 KB, so AArchX tracks protection per
4 KB slot inside each host page and takes the union when it has to ask the
kernel. That mismatch shapes most of the memory code, and it is why a guest's
protection changes are resolved on 16 KB boundaries.

## From x86 bytes to arm64 code

The decoder turns instruction bytes into an internal operation with its
operands. The interpreter can execute that directly, and is the reference: every
guest test in the suite is run both interpreted and translated, and the two must
agree byte for byte.

The JIT translates a **block** at a time, from an entry point to the branch that
ends it. Inside a block:

- **All sixteen guest general registers live in arm64 registers**, and the
  sixteen XMM registers live in vector registers, for the whole block. Nothing
  is spilled to memory unless something needs the architectural state, such as a
  call out to C. Because every block uses the same layout, control can pass
  from one block's body straight into another's.
- **Flags are computed only where they are read.** x86 writes six status flags
  on nearly every arithmetic instruction and reads them rarely. AArchX records
  what produced them and materialises only what a later instruction actually
  needs; a block whose flags die unread computes none of them.
- **Blocks are chained.** When a block's successor is already translated, its
  exit is patched into a direct branch. A returning `ret` uses a host return
  stack that mirrors the guest's calls, so the processor's own return predictor
  stays aligned with the guest's.
- **Superblocks.** A forward conditional branch does not have to end a block:
  the block can continue past it with the taken side in an out-of-line stub.
  When the program turns out to take the other edge, the block is retranslated
  the other way round, decided by watching which edge runs.

A guest that writes over code it has already executed is handled by making the
page fault on write and dropping the translations covering it.

## Memory ordering

x86 guarantees more about the order of memory operations than arm64 does. A
program that never creates a thread, forks, or maps shared memory cannot observe
the difference, so AArchX translates it with plain loads and stores. The first
time it does any of those, AArchX switches to acquire and release forms for
scalar accesses and keeps them for the rest of the process's life.

Vector loads and stores are left plain even then, which is what other
translators do as well, because ordering them costs several times more than it
is worth; `OCERZ_TSO_VECTOR=1` orders them for anyone who wants it.

## Floating point

x86 and arm64 disagree about NaNs: an invalid operation gives a negative default
NaN on one and a positive one on the other, and when both operands are NaNs x86
returns the first where arm64 lets a signalling one win. Programs do notice.

AArchX handles this two ways. Where the processor reports `FEAT_AFP`, setting
one bit in the floating-point control register makes arm64 arithmetic follow
x86's rules, and the instructions are emitted bare. Where it does not, results
are checked and a whole run of floating-point work is replayed exactly through
the interpreter if a NaN appears — once per run rather than once per
instruction, with the registers it overwrites checkpointed so the replay can
start from the beginning.

## The system beneath the guest

Guest system calls do not reach the kernel as they are. AArchX implements the
BSD and Mach interfaces itself, because most of them need the arguments or the
results rewritten: a structure that differs between the two architectures, a
pointer into guest memory, a thread port that names a guest thread rather than a
host one. Signals are delivered onto the guest's stack in the frame layout x86
code expects, and a fault inside translated code is turned back into a fault at
the guest instruction that caused it, with every guest register recovered from
the arm64 registers holding them.

What the guest links against is the part that differs between the two
[modes](modes.md), and it is the largest difference in the project: cache mode
maps Apple's x86-64 shared cache and translates it like everything else, while
native mode synthesizes x86 images whose stubs cross into the Mac's own arm64
frameworks.

## Where to read further

Every source file opens with a prose block explaining what it does and why it
has the shape it has, including the mistakes that shaped it. `src/jit.c` and
`src/dyld.c` are the two longest and the two most worth reading.
