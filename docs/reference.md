# Reference

## Command line

```text
usage: ocerz [-v] [-trace] [-strace] [-no-jit] [-native|-cache] [-path file] [--] program [args...]
       ocerz version
```

| Option | Effect |
| --- | --- |
| `-v` | more logging: the mode, the arena, the shared cache, image loads, the JIT arena |
| `-trace` | trace guest instructions |
| `-strace` | trace guest system calls |
| `-no-jit` | interpret this process only, without translating |
| `-native` | bind the guest against native arm64 frameworks instead of the x86 shared cache |
| `-cache` | bind the guest against Apple's x86-64 shared cache; the default |
| `-path file` | load `file`, and keep every following argument for the guest |
| `--` | end of AArchX's own options |
| `version` | print the name and version |

The binary is named `ocerz`, which is what the project was called before it was
renamed to AArchX. The source tree, the symbols and the environment variables
still carry that name.

## Environment

These are the variables meant to be used. The source defines roughly three
hundred more, nearly all of them switches that turn one optimisation or one
diagnostic on or off while a bug is being narrowed down; they are documented
where they are read.

### Choosing what runs

| Variable | Effect |
| --- | --- |
| `OCERZ_MODE=native\|cache` | pick the mode when no flag does. This is how a program's children inherit it. An unrecognised value is refused rather than ignored. |
| `OCERZ_NOJIT=1` | interpret the whole process tree |
| `OCERZ_NOJIT_EXE=<text>` | interpret only the processes whose command line matches |
| `OCERZ_APIDB=<dir>` | in native mode, read the API databases from this directory instead of `runtime/apis` beside the binary |

### Memory ordering

x86 has a stronger memory model than arm64. A program that never creates a
thread, forks, or maps shared memory cannot observe the difference, so AArchX
runs it with plain loads and stores; the first time it does any of those,
ordered forms are used from then on.

| Variable | Effect |
| --- | --- |
| `OCERZ_NO_PLAIN_MEM=1` | use ordered forms from the start |
| `OCERZ_TSO_STRICT=1` | order stack-relative accesses too |
| `OCERZ_TSO_VECTOR=1` | order SSE loads and stores too, which costs a great deal |

### Turning off what makes it fast

Each of these makes AArchX slower and is useful only for narrowing down a
suspected miscompilation: if a program misbehaves and one of these fixes it, the
bug is in what that switch controls.

| Variable | Effect |
| --- | --- |
| `OCERZ_NO_AFP=1` | check NaN results in translated code even where the processor can produce x86's NaNs itself |
| `OCERZ_NO_LEAF_INPLACE=1` | call `strlen`, `memcpy` and the nine other string and memory routines the ordinary way instead of through AArchX's own arm64 versions |
| `OCERZ_NO_MEMFN_PLAIN=1` | translate the system's string and memory routines with ordered accesses when the rest of the process has them |
| `OCERZ_NO_FPB_DEFER=1` | check every floating-point result immediately instead of once per batch |
| `OCERZ_NO_FLIP=1` | no profile-driven retranslation of superblocks |
| `OCERZ_NO_MOVFUSE=1` | do not fold a `mov` into a following shift |
| `OCERZ_NO_BRIDGE_FASTCALL=1` | in native mode, reach every bridged call through the trap and the dispatcher instead of calling it from inside the translated block |
| `OCERZ_NO_COMPACT=1` | keep every block's decoded instructions after translation |

### Native mode

| Variable | Effect |
| --- | --- |
| `OCERZ_BRIDGESTAT=1` | print how many times each bridged function was called, at exit |
| `OCERZ_BRIDGELOG=1` | name every bridged call as it happens |
| `OCERZ_NO_NATIVE_CHILDREN=1` | run the system tools a guest starts under AArchX as well; by default one that has an arm64 slice and is not a shell or launcher runs as itself |
| `OCERZ_NO_TSD_DTORS=1` | do not run the destructors of the guest's thread-specific data keys at thread exit |

### Diagnostics

| Variable | Effect |
| --- | --- |
| `OCERZ_DLPATH=1` | print every `dlopen` and `dlsym` failure with its reason; inherited by child processes, which `-v` is not |
| `OCERZ_PERFSTAT=1` | at exit, print block counts, the hottest blocks and where instructions went |
| `OCERZ_FAULTLOG=1` | print the mapping state around every guest fault |
| `OCERZ_EXCLOG=1` | print every Objective-C and C++ exception with its throw site |
| `OCERZ_WILDLOG=1` | report indirect branches whose target is outside the guest address space |
| `OCERZ_JITLOCKLOG=1` | report JIT lock waits over three seconds, with the holder's state |
| `OCERZ_SUSPLOG=1` | in native mode, log each `thread_suspend` and `thread_resume` a guest makes |
| `OCERZ_XLATPAGES=1` | log each distinct 4 KB page the process translates |

### Behaviour

| Variable | Effect |
| --- | --- |
| `OCERZ_STRICT_SYSCALL=1` | stop on an unimplemented system call instead of returning `ENOSYS` |
| `OCERZ_NO_HOSTWQ=1` | turn off the host workqueue bridge, which is on by default |
| `OCERZ_NO_UNSTICK=1` | never interrupt a guest thread out of a long wait |
| `OCERZ_NO_FILEMAP=1` | read every private file mapping into anonymous memory instead of mapping it from the file |
| `OCERZ_PRELOAD_OBJC=<paths>` | put matching shared-cache Objective-C images into the startup batch; `@cat` does it for every image that defines categories |

## Exit codes

| Code | Meaning |
| --- | --- |
| the guest's own | the program ran and exited normally |
| 64 | AArchX's own arguments were wrong, or native mode was given a statically linked program |
| 65 | the program could not be read or loaded |
| 70 | the guest arena or the guest stack could not be set up |
| 71 | native mode: an import could not be bound, and the message names it |
| 72 | native mode: a bound export has no crossing, and the message names it |
| 139 | a fault inside a native frame that cannot be resumed or unwound, with a report naming the call |

Every one of these is preceded by a line on standard error beginning `ocerz:`
that says what went wrong.
