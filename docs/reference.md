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

### Process trees

Wine and Steam start many processes, and a setting is usually wanted in one of them only. Each of these picks processes by a substring of their command line, and the setting is inherited through every exec.

| Variable | Effect |
| --- | --- |
| `OCERZ_EXE_ENV="a.exe:K=V,K=V;b.exe:K=V"` | set variables only in the processes whose command line matches |
| `OCERZ_STRACE_EXE=<text>` | trace system calls only in the processes whose command line matches |
| `OCERZ_NOJIT_EXE=<text>` | interpret only the processes whose command line matches |
| `OCERZ_STDERR_FILE=<file>` | append every process's standard error to one file |

### Keeping translations

Translated code is kept on disk and shared between processes: a block that one
process has translated is loaded, not translated again, by any process that
needs it later, in the same session or the next. Only code that sits at the
same address every run is kept, which is all of it under Wine and the system
libraries otherwise. The stores live under `~/Library/Caches/ocerz`, one
directory for each build of ocerz and set of `OCERZ_` variables; a Steam session
fills about 2 GB. A store that reaches its limit is emptied and started again by
the next process that finds no other process using it. Directories of other
builds are removed after a day, or sooner while they add up to more than 2 GB.
Each directory also keeps a 1 MB filter of the names the shared cache defines
weakly, which a process binding a C++ library would otherwise build for itself.

| Variable | Effect |
| --- | --- |
| `OCERZ_TCACHE=off` | translate everything afresh and keep nothing |
| `OCERZ_TCACHE_DIR=<dir>` | keep the stores under this directory instead |
| `OCERZ_TCACHE_MAX_MB=<n>` | the size at which a store stops growing (4096 by default) |
| `OCERZ_TCACHE_MIN_FREE_MB=<n>` | free space the cache leaves on its disk: it stops writing below this, takes at most half of what is free above it, and clears its stores when the disk is already under it (10240 by default) |
| `OCERZ_TCACHE_LOG=1\|<file>` | print, for each process, how many blocks it loaded and stored |
| `OCERZ_TCACHE=verify` | a check, not for use: translate everything anyway and compare each block with the stored one |
| `OCERZ_TCACHE=roundtrip` | a check, not for use: store nothing, but move every translation to a new address and report anything in it that still pointed at the old one |

### Memory ordering

x86 has a stronger memory model than arm64. A program that never creates a
thread, forks, or maps shared memory cannot observe the difference, so AArchX
runs it with plain loads and stores; the first time it does any of those,
ordered forms are used from then on. A mapping the kernel hands the program
counts as shared memory only when another process can see it: a private one,
like the buffers the system's logging library maps into nearly every program,
leaves memory plain.

| Variable | Effect |
| --- | --- |
| `OCERZ_NO_PLAIN_MEM=1` | use ordered forms from the start |
| `OCERZ_NO_REMAP_PLAIN=1` | use ordered forms once any mapping the kernel hands the program has to be moved into guest memory, private or not, as before 2026-10-09 |
| `OCERZ_TSO_STRICT=1` | order stack-relative accesses too |
| `OCERZ_TSO_VECTOR=1` | order SSE loads and stores too, which costs a great deal |
| `OCERZ_TSO_NARROW=1` | order `movd` and `movq` loads and stores; `=2` orders every SSE access of 8 bytes or fewer |

### Turning off what makes it fast

Each of these makes AArchX slower and is useful only for narrowing down a
suspected miscompilation: if a program misbehaves and one of these fixes it, the
bug is in what that switch controls.

| Variable | Effect |
| --- | --- |
| `OCERZ_NO_AFP=1` | check NaN results in translated code even where the processor can produce x86's NaNs itself |
| `OCERZ_NO_JIT_MMX=1` | run MMX instructions in the interpreter |
| `OCERZ_NO_FAST_LOW_GUARD=1` | in a Wine process, map every guest address with the general three-range check instead of the short form |
| `OCERZ_NO_WINE_LIM=1` | in a Wine process, test each address against 12 GB with a shift and a compare again, instead of comparing it with x29 (which then holds 12 GB) and laying out the side it was on when translated |
| `OCERZ_WLIM_CHECK=1` | in a Wine process, trap before every guard whose x29 is not 12 GB (a debugging aid) |
| `OCERZ_NO_LOW_STACK_PTR=1` | in a Wine process, keep rsp's own value in its register and add the stack's offset on every stack access, instead of keeping the host address rsp points at and converting only when rsp is read or written as a value |
| `OCERZ_LOWSTACK_CHECK=1` | in a Wine process, trap before any instruction where the stack offset kept for rsp no longer matches rsp (a debugging aid) |
| `OCERZ_LOW_TOP_GUARD=1` | in a Wine process, test every access for the top strip instead of learning which blocks reach it from their first fault there |
| `OCERZ_ALIGN_TEST_ALL=1` | test every ordered access for a 16-byte crossing instead of patching the sites that fault |
| `OCERZ_NO_LEAF_INPLACE=1` | call `strlen`, `memcpy` and the nine other string and memory routines the ordinary way instead of through AArchX's own arm64 versions |
| `OCERZ_NO_LEAF_LOW=1` | in the Wine layout, leave those routines to their translated x86 code instead of handing AArchX's versions translated pointers |
| `OCERZ_NO_MEMFN_PLAIN=1` | translate the system's string and memory routines with ordered accesses when the rest of the process has them |
| `OCERZ_NO_FPB_DEFER=1` | check every floating-point result immediately instead of once per batch |
| `OCERZ_NO_FLIP=1` | no profile-driven retranslation of superblocks |
| `OCERZ_NO_MOVFUSE=1` | do not fold a `mov` into a following shift |
| `OCERZ_NO_BRIDGE_FASTCALL=1` | in native mode, reach every bridged call through the trap and the dispatcher instead of calling it from inside the translated block |
| `OCERZ_NO_COMPACT=1` | keep every block's decoded instructions after translation |
| `OCERZ_NO_JIT_FLUSH=1` | when the translation arena fills, run everything translated after that in the interpreter instead of starting the arena again |

### Native mode

| Variable | Effect |
| --- | --- |
| `OCERZ_BRIDGESTAT=1` | print how many times each bridged function was called, at exit |
| `OCERZ_BRIDGELOG=1` | name every bridged call as it happens, with the guest's return address and first argument |
| `OCERZ_NO_NATIVE_CHILDREN=1` | run the system tools a guest starts under AArchX as well; by default one that has an arm64 slice and is not a shell or launcher runs as itself |
| `OCERZ_NO_TSD_DTORS=1` | do not run the destructors of the guest's thread-specific data keys at thread exit |

### Diagnostics

| Variable | Effect |
| --- | --- |
| `OCERZ_DLPATH=1` | print every `dlopen` and `dlsym` failure with its reason; inherited by child processes, which `-v` is not |
| `OCERZ_PERFSTAT=1` | every 15 seconds and at exit, print block counts, the hottest blocks, where instructions went, and return-address stack misses split by cause with the `ret` sites that miss most |
| `OCERZ_FAULTLOG=1` | print the mapping state around every guest fault |
| `OCERZ_EXCLOG=1` | print every Objective-C and C++ exception with its throw site |
| `OCERZ_WILDLOG=1` | report indirect branches whose target is outside the guest address space |
| `OCERZ_JITLOCKLOG=1` | report JIT lock waits over three seconds, with the holder's state |
| `OCERZ_SUSPLOG=1` | in native mode, log each `thread_suspend` and `thread_resume` a guest makes |
| `OCERZ_XLATPAGES=1` | log each distinct 4 KB page the process translates |
| `OCERZ_ALLMISS=1` | list every unresolved import rather than the first two dozen |
| `OCERZ_DYNLOOKUPLOG=1` | name each symbol imported with `-undefined dynamic_lookup` that nothing defines, which is allowed and otherwise not reported |
| `OCERZ_DLSYMLOG=1` | print each `dlsym` with its handle, result and caller |
| `OCERZ_MACSYSLOG=1` | print each `__mac_syscall` with its policy, number and argument words |
| `OCERZ_MODELOG=1` | print every far transfer with its selector, target mode and address: the WoW64 32/64-bit switches |
| `OCERZ_FPS=1` | in cache mode, print frames per second, counted at `CGLFlushDrawable`, once a second |
| `OCERZ_GUESTPROF=<usec>` | sample every running guest thread at about that interval, and every `OCERZ_GUESTPROF_PERIOD` seconds (10 by default) and at exit print the hottest guest code, host symbols and interpreted instruction forms |
| `OCERZ_GUESTPROF_HOT=1` | with `OCERZ_GUESTPROF`, sample in each period only the thread that used the most CPU in the period before |
| `OCERZ_ORDERLOG=1` | print what first made the process use ordered memory forms, with a backtrace |
| `OCERZ_TRIPSTAT=1` | count exits from translated code to the dispatcher, and print their commonest destinations every ten seconds |
| `OCERZ_BLACKLOG=1` | print the pages most often refused translation because they kept changing |
| `OCERZ_INVSRC=1` | attribute each of those refusals to the code that invalidated the page |
| `OCERZ_IPCLOG=1` | send the translated Steam `ipcserver`'s output to `/tmp/ocerz_ipcserver.err` |
| `OCERZ_SELPOOLLOG=1`, `OCERZ_SELVERIFY=1` | describe the selector index built when the shared cache's own table cannot be used, and check the cache's answers against it |

### Behaviour

| Variable | Effect |
| --- | --- |
| `OCERZ_STRICT_SYSCALL=1` | stop on an unimplemented system call instead of returning `ENOSYS` |
| `OCERZ_NO_HOSTWQ=1` | turn off the host workqueue bridge, which is on by default |
| `OCERZ_NO_UNSTICK=1` | never interrupt a guest thread out of a long wait |
| `OCERZ_NO_FILEMAP=1` | read every private file mapping into anonymous memory instead of mapping it from the file |
| `OCERZ_PRELOAD_OBJC=<paths>` | put matching shared-cache Objective-C images into the startup batch; `@cat` does it for every image that defines categories |
| `OCERZ_NO_UPWARD_INIT=1` | do not initialize a library reached only through an upward dependency, as before 2026-09-22 |
| `OCERZ_NO_DELAY_INIT=1` | load and initialize at launch the libraries reached only through delayed-init dependencies, which dyld leaves until the program asks for them, as before 2026-10-09 |
| `OCERZ_NO_WEAK_MAIN_FIRST=1` | look a weak definition up in the shared cache before the main executable, as before 2026-10-09 |
| `OCERZ_NO_LOADMAP=1` | do not map a shared-cache image's Objective-C classes before its `+load` runs |
| `OCERZ_NO_LATE_CATLIST=1` | do not report category lists for shared-cache images loaded after startup |
| `OCERZ_NO_THREADACT=1` | hand `thread_suspend`, `thread_resume` and `thread_get_state` on guest threads to the kernel instead of emulating them |
| `OCERZ_UNSTICK_ALL=1` | let the unstick monitor interrupt every blocking call, `read`, `recvmsg` and `poll` included |
| `OCERZ_NO_VMMAP_STEER=1` | let `mach_vm_map` place mappings anywhere in host space instead of at guest-visible addresses |
| `OCERZ_REFAULT_INVAL=1` | invalidate again on every repeated alignment or commpage fault, including faults from retired translations |
| `OCERZ_NO_MOCK_KEYCHAIN=1` | launch Steam's `Steam Helper` without `--use-mock-keychain`, so macOS asks for the login password |

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
