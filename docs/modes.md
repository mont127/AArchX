# Modes

AArchX can bind a guest program against two different sets of system libraries.
The choice is made once, before the program starts, and cannot change while it
runs.

| | Cache mode | Native mode |
| --- | --- | --- |
| Flag | `-cache`, the default | `-native` |
| Guest links against | Apple's x86-64 shared cache | the Mac's own arm64 frameworks |
| Needs Rosetta installed | yes, for the cache file | no |
| System library code is | x86, translated like the program | arm64, run as itself |
| Programs it runs | everything AArchX supports | a narrower set, growing |

Both modes translate the program's own code the same way. What differs is what
happens when the program calls `printf`, `CFRelease` or `-[NSString length]`.

## Cache mode

Cache mode is the default and the one that runs the most software. AArchX maps
Apple's `dyld_shared_cache_x86_64`, applies its fixups, registers its
Objective-C classes and runs its initializers, and from then on the guest's
libraries are x86 code that gets translated exactly like the guest's own.

The shared cache is shipped by the Rosetta package rather than by the base
system, which is why cache mode needs Rosetta installed. AArchX reads and maps
the file itself; Rosetta's translator is never invoked and never runs. On macOS
27 the cache lives in the Rosetta cryptex rather than its historical path, and
AArchX looks in both.

Because everything above the kernel is translated, cache mode behaves the way an
x86 Mac does, down to details a program can observe: the page size the guest
sees, the layout of the libraries it walks, the addresses `dladdr` reports.

## Native mode

Native mode runs the same x86 program against the Mac's own arm64 system
frameworks. There is no x86 shared cache involved and none needs to exist.

For each system library the guest imports, AArchX synthesizes an x86_64 Mach-O
image in memory: real headers, a real export trie, and a stub for every export.
The stubs do not contain x86 implementations. Each one loads an identifier and
jumps to a trap address, and AArchX turns that into a call to the host's real
arm64 function, moving arguments and results between the System V x86-64 and
Apple arm64 calling conventions on the way. What every export's signature is
comes from an API database under `runtime/apis`, one text file per library,
generated from the macOS SDK by `tools/sdkgen`.

Crossings go both ways. Native code calls guest code through a bank of
trampolines, so `qsort` can call an x86 comparator and Foundation can call an
x86 block. The Objective-C bridge sends messages in both directions and realizes
the guest's own classes, categories and protocols in the native runtime.

**What native mode buys.** The system frameworks run as native arm64 code at
full speed, with no translation and no x86 shared cache on the machine at all.
For a program that spends its time inside the frameworks rather than in its own
code, that is the whole difference.

**What it costs.** A crossing is not free. Calling `getpid` through the bridge
costs about 17 ns against roughly 1 ns natively, and an Objective-C send about
28 ns. A call back into guest code is far more expensive, around 430 ns, because
of the signal state a callback's fault-recovery point needs. Eleven string and
memory routines that programs call constantly, `strlen` and `memcpy` among them,
do not cross at all: AArchX has arm64 versions that run with the guest's
registers in place, so a short `strlen` costs 2 ns rather than 17.

**What it cannot do yet.** Swift, WebKit, C++ objects shared with native arm64
C++, and exceptions crossing a bridge frame are the large gaps, and some
libraries still have no database. A program that needs one of those stops with a
message naming what is missing, and exits 71 when an import cannot bind or 72
when a bound export has no crossing. It does not run on and produce wrong
answers.

## Choosing

Use cache mode unless you have a reason not to. It runs more software and needs
no thought.

Use native mode when you do not want Rosetta's shared cache on the machine, when
you want the system frameworks running as arm64 code, or when you are working on
native mode itself.

## How the mode is set

```sh
./ocerz -native ./program          # flag
OCERZ_MODE=native ./ocerz ./program   # environment
```

The flag wins over the environment, and the last flag wins over an earlier one.
An unrecognised `OCERZ_MODE` value is refused rather than ignored.

A program that starts another program passes its mode down: children inherit it
through `OCERZ_MODE`, even when they are started with an empty environment. In
native mode there is one deliberate exception. A system tool that has an arm64
slice of its own and is not a shell or another launcher runs as itself rather
than under AArchX, since nothing about it needs translating;
`OCERZ_NO_NATIVE_CHILDREN=1` turns that off.
