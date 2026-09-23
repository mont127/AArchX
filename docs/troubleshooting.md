# Troubleshooting

Every failure AArchX reports begins with `ocerz:` on standard error, and the
first such line is nearly always the one that matters. What follows is worth
reading too: AArchX tries to say what it could not do and which side was at
fault, rather than only that something went wrong.

## Start here

```sh
./ocerz -v ./program 2> log.txt          # the mode, the arena, the cache, image loads
OCERZ_DLPATH=1 ./ocerz ./program         # every dlopen and dlsym failure, in children too
./ocerz -no-jit ./program                # interpret instead of translating
```

`-no-jit` is the single most useful switch. The interpreter is the reference
implementation, so if a program works interpreted and fails translated, the bug
is in the JIT and worth reporting. If it fails both ways, the cause is in the
loader, the system-call layer or the libraries.

## The messages

**`no bridge for <symbol> in <library>`, exit 71.** Native mode could not bind
an import. The library has no API database, or the database does not list that
export. Cache mode will usually run the program.

**`<library> <symbol> not implemented`, exit 72.** Native mode bound the export
but has no crossing for it: it is a stub. The message names it exactly. An exit
72 often runs the program's own exit handlers on the way out and produces a
second, confusing crash report; the line just above is the real one.

**`native mode cannot run the static image`, exit 64.** Native mode needs a
dynamically linked program, because its whole mechanism is standing in for the
libraries a program links against.

**`BRIDGE-FAULT … inside a bridged call`, exit 139.** In native mode, a fault
happened inside the Mac's own arm64 code while it was running on a guest thread.
The report names the library, the export, its signature and the faulting
address, and says which side of the guest boundary that address fell on: inside
means the guest passed a bad pointer, outside means AArchX marshalled the call
wrongly. The process stops there because a native frame cannot be resumed or
unwound.

**`guest crash … rip=…`.** The guest program itself faulted, the way it would on
an x86 Mac. The register dump is the guest's state at the faulting instruction.

**`cannot read` or `cannot load`, exit 65.** The path is wrong, or the file has
no x86-64 slice AArchX can use.

**A program that prints nothing and exits 70** failed to get its arena or its
stack, which normally means something else on the machine has taken the address
space it needs.

## When output is wrong rather than absent

This is the case worth narrowing down, because it means something translated
incorrectly. The switches under "Turning off what makes it fast" in the
[reference](reference.md) each disable one optimisation. Turning them on one at
a time until the program behaves identifies the area:

```sh
OCERZ_NO_FLIP=1 ./ocerz ./program        # superblock retranslation
OCERZ_NO_FPB_DEFER=1 ./ocerz ./program   # deferred floating-point checks
OCERZ_NO_AFP=1 ./ocerz ./program         # hardware NaN results
OCERZ_NO_MOVFUSE=1 ./ocerz ./program     # instruction fusion
OCERZ_NO_PLAIN_MEM=1 ./ocerz ./program   # stricter memory ordering from the start
```

If one of them fixes it, that names the bug. If `-no-jit` fixes it and none of
them do, it is still a translation bug, just not in one of those.

## When a program hangs

```sh
sample <pid>                             # where its threads are
OCERZ_JITLOCKLOG=1 ./ocerz ./program     # translation lock held too long
OCERZ_PERFSTAT=1 ./ocerz ./program       # at exit: hottest blocks, translation counts
```

A guest thread parked in `mach_msg` or `kevent` is usually waiting for something
that never arrived rather than stuck in translation. `OCERZ_PERFSTAT=1` shows
whether AArchX is translating the same pages over and over, which is what a
program that rewrites its own code looks like.

## Things that are not bugs

- **A window taking much longer to appear than it does natively.** Everything
  the application does at startup is being translated for the first time.
- **`OCERZ_NO_PLAIN_MEM=1` being slower.** That is the stricter memory model,
  and it costs what it costs; AArchX switches to it by itself as soon as a
  program creates a thread.
- **Cache mode needing Rosetta installed.** The Rosetta package ships the
  x86-64 shared cache. AArchX maps that file; it never invokes Rosetta's
  translator. [Native mode](modes.md) needs nothing from it.
