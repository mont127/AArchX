//! The guest-to-host syscall boundary: Rust port of emulated x86_64 syscalls onto the arm64
//! kernel.
//!
//! ---- the return ABI is Rosetta's, not the kernel's ----
//! XNU returns every unix syscall through rax AND rdx (the second result slot,
//! zero unless the call fills it), on success and on error alike, and Rosetta
//! reproduces that - so a caller whose wrapper forgot to declare rdx clobbered
//! sees rdx = 0 there, and leaving rdx alone was silently kinder.  Rosetta also
//! returns from a trap with a fixed arithmetic-flag pattern whatever the caller
//! had (probed 2026-09-03): unix success PF|AF, unix error CF|ZF|PF, mach traps
//! AF.  Only CF is architecturally meaningful - the libSystem stubs branch on it
//! - but the rest is matched so a program that reads them sees what it sees on
//! Rosetta.  Machdep (class 3) calls are the exception: they return through rax
//! alone, leaving rdx and the flags untouched, because Wine's syscall dispatcher
//! keeps its frame pointer in rdx across thread_set_tsd_base and applying the
//! unix rule there cost every Wine process a NULL dereference at its first
//! return to PE code (2026-09-04).
//!
//! ---- 4 KB guest pages on a 16 KB host ----
//! A MAP_PRIVATE file mapping used to be read into anonymous memory in full,
//! because the host cannot map a file at every 4 KB guest offset.  When the file
//! offset and the guest address agree modulo 16 KB - which they do for nearly
//! everything Wine maps: PE images at 64 KB-aligned bases, fonts, resources -
//! the aligned interior becomes a real private mapping of the file and its pages
//! sit in the shared page cache instead of being copied per process.  A GUI Wine
//! process carried 280 MB of such copies, and one winemine run read 5 GB of
//! Songti.ttc while enumerating fonts.  Partial head and tail pages, and
//! anything past EOF, are still read in.
//!
//! A read-only shared mapping that is not 16 KB-aligned used to become a private
//! snapshot up front.  Steam's UI stream is exactly that - steam.exe opens the
//! SteamChrome ring buffer read-only and the browser writes it - so a snapshot
//! never showed the writes, Steam re-created the stream forever, and no window
//! came up (2026-09-06).  The real shared overlay is tried first and the private
//! copy is only the fallback.  Writable shared mappings are communication
//! channels, so guest memory backing them is kept TSO-ordered.
//!
//! mach_vm_map(FIXED|OVERWRITE) is 4 KB-granular for the guest and 16 KB-granular
//! for the kernel, which replaces whole host pages and takes the guest bytes on
//! either side with it - memory the request never named.  mmap() goes through
//! mem.c, which owns the slot machinery, but this path hands the message straight
//! to the kernel and so copies the fragments out and back itself.  Safari asked
//! for 0x4000 at a ...e000 address inside the shared cache and lost the 8 KB
//! below it, which held a constant Engram checks with a Swift precondition: it
//! trapped one framework away from the cause.  A mach_vm_map(ANYWHERE) under the
//! low-base shadow is likewise placed by hand at a guest-visible identity
//! address, because relocating the kernel's answer afterwards leaves dangling
//! host pointers wherever the kernel recorded the original (IOSurface's
//! bulk-attachment page, read back by CoreAnimation).  A mach reply must never
//! land in the shared cache either: moving it would deallocate a range every
//! image is linked against, and Safari died one mach_msg later when the next
//! _platform_* access faulted.
//!
//! A mach_vm_map that is FIXED without OVERWRITE is how Wine asks whether a range
//! is free before it maps there, and under the low-base shadow the kernel is
//! asked about the host's numbers, which say nothing about the guest's: a range
//! the guest already uses looked free, the reply was moved into place over it,
//! and Wine then mapped over its own loader at 0x200000000.  So a FIXED request
//! that comes back for guest pages already in use is answered KERN_NO_SPACE, as
//! the kernel would have answered it, and the host pages it got are given back
//! (OCERZ_NO_VMMAP_TAKEN=1 turns this off).  Requests aimed at another task -
//! mach_vm_read, read_overwrite, write, protect and msync, which is how the
//! wineserver reads and writes its clients - name that task's guest addresses,
//! so a low address is moved by the shadow base the whole Wine tree inherits in
//! OCERZ_LOWBASE before the kernel sees it; untranslated, Chromium's sandbox read
//! a child's TEB from the wrong memory and every launch of its GPU process failed
//! with sandbox error 43 (OCERZ_NO_PEER_VM=1 turns this off).
//!
//! A raw host pointer that a service or the kernel hands the guest is made
//! readable by aliasing the host region behind it into the shadow window at the
//! same number.  The alias stops at the first page that holds guest data, and a
//! pointer whose own page holds guest data is not aliased at all: a whole host
//! region used to be mapped over the guest, and a pointer into the host's arm64
//! shared cache brought two gigabytes of it down on top of Wine's loader, whose
//! header SkyLight then walked as four billion load commands until Chromium's GPU
//! watchdog killed the process (OCERZ_NO_ALIAS_CLIP=1 restores the old reach).
//!
//! The host does not keep its memory where it was: Metal frees a buffer and maps
//! the next one at the same address, so an alias kept showing the freed buffer
//! to the guest, and Metal's own completion state with it - a compute job read
//! the previous job's results or never finished.  Each alias is recorded with
//! the VM object it shows.  When a pointer comes back into a recorded alias that
//! still shows that object, while the host has a different one there, the region
//! is aliased again over it; an alias the guest has since mapped its own memory
//! over shows another object and is left alone.  Memory nobody has touched yet
//! has no object at all, so an alias of such memory is not recorded: fresh guest
//! memory mapped over it would look the same.  The page clipping above treats
//! such intact aliases as free, so a buffer that grew into an older neighbour's
//! alias is shown whole (OCERZ_NO_ALIAS_REFRESH=1 turns the refresh off).
//!
//! The pointers an IOKit reply hands back are not all page-aligned.  Creating
//! an IOSurface answers with the surface's pixels and with two records inside
//! memory the kernel shares with the process.  The reply scan took page-aligned
//! values only, so those records were reached through whatever the shadow held
//! at their number: a fault aliased them when nothing was there, and anything
//! else there answered in their place.  R.E.P.O.'s CAMetalLayer then fetched
//! a new drawable's properties into a buffer of garbage size (four gigabytes,
//! with the right size in its low bits), the kernel refused, and the game
//! aborted on a nil drawable in two launches of three; with the records
//! aliased from the reply, none of five did.  An 8-byte-aligned value inside
//! IOKit or shared host memory is now a candidate too, and it is skipped when
//! the guest has its own memory at that number (OCERZ_NO_IOKIT_INNER_PTR=1
//! restores page-aligned values only).
//!
//! ---- sysctl ----
//! An x86_64 process must see 4 KB pages, as it does under Rosetta, and the
//! override has to mirror the real node's width rather than assume 4: reached by
//! name "hw.pagesize" is a 64-bit node while the legacy {CTL_HW, HW_PAGESIZE}
//! mib is the 32-bit one, and writing 4 bytes into the 8 the caller offered left
//! the top half of its variable dirty - sysconf(_SC_PHYS_PAGES) divides
//! hw.memsize by exactly that variable, so it returned 0 whenever the stack
//! happened to be non-zero and sort(1) died on "sysconf pages".  The arm64
//! kernel has no machdep.cpu nodes at all, so passed through they all failed
//! with ENOENT and code that asks hw.optional.sse4_1 before taking a fast path
//! took none; the x86 CPU description is synthesized to agree with ocerz's own
//! CPUID (SSE through SSE4.2, CX16, POPCNT; no AES, PCLMULQDQ or AVX) and pinned
//! by the dynamic test x86_sysctl.
//!
//! ---- threads ----
//! The host workqueue bridge is on unless OCERZ_NO_HOSTWQ says otherwise.  It
//! used to be opt-in, which meant every Cocoa application deadlocked out of the
//! box: AppKit and libdispatch hand work to workqueue threads and then wait for
//! it, so with nothing servicing those queues the first dispatch_sync parked the
//! main thread in __ulock_wait forever, and Calculator, TextEdit and Safari all
//! registered as foreground apps without ever reaching the WindowServer.  Native
//! mode turns the bridge off outright: registering it claims the process's single
//! _pthread_workqueue_init_with_workloop slot, and there the host's own
//! libdispatch is what needs that slot.  The mode itself travels to children as
//! OCERZ_MODE in the environment the spawn and exec paths build, so a bundled
//! helper comes up in the same universe as its parent.
//!
//! A new thread's cpu is a copy of its creator's taken inside the creator's
//! syscall, so nothing about the creator's own wait or suspension may come along
//! - a creator suspended by the GC at that moment would hand the child
//! suspend_count 1 and the child would park at its first safe point with nobody
//! left to resume it.  The signal mask, by contrast, is inherited as POSIX
//! requires.  bsdthread_create blocks until the kernel port is in the pthread
//! struct, matching the kernel: without that a pthread_join right after
//! pthread_create can read an empty port slot, conclude the thread exited, and
//! free the stack of a live thread.
//!
//! thread_info(THREAD_IDENTIFIER_INFO) answers with the thread's TSD base, which
//! the kernel reports as the host's.  It is replaced by the guest TSD of the cpu
//! whose host thread owns that base - the pthread one even while Wine has gs on
//! the TEB - rather than recognised by its address: Wine saves this value and
//! switches gs back to it on every syscall, and a host thread whose stack landed
//! above 12 GB used to pass its own TSD through untouched, so that Wine thread
//! lost its TEB and died at its first server call.
//!
//! ---- signals ----
//! The two sigaction directions do NOT share a layout: the kernel takes a
//! `struct __sigaction` (24 bytes, sa_tramp at 8) and hands back a `struct
//! sigaction` (16 bytes, no trampoline).  Writing the inbound layout to oldact
//! overran the caller's buffer by 8 bytes; in sort(1) those bytes were a saved
//! rbx, so its `outfile` pointer came back NULL from the epilogue.
//!
//! Asynchronous signals obey the guest mask; synchronous faults never come
//! through here and are never gated.  Enforcing that is what keeps Wine's server
//! protocol in one piece: Wine blocks server_block_set around a wineserver
//! request/reply pair so a suspend kick lands only between calls, and delivering
//! one mid-pair nests a second round trip inside the first on the same strictly
//! ordered fd pair, desynchronising the stream until every client parks in
//! read() forever.  Pending signals are also offered on the ENTRY edge of a
//! syscall, with rip rewound so the call re-executes afterwards: the host
//! handler only sets a bit and the return-edge drain would otherwise leave a
//! signal unnoticed until some later syscall returned - and if the next one
//! blocks, it parks with the wakeup still undelivered.  A signal that arrives on
//! a thread running a guest cpu stays pending on that cpu, because Wine aims its
//! SIGUSR1 and SIGQUIT at one thread through __pthread_kill and the handler
//! expects to run there; only a signal that reaches a thread with no guest cpu
//! goes to that thread's pool, from which every take - the entry and return
//! edges of a syscall, the bridge, sigsuspend - removes just the signals the
//! guest mask lets through (OCERZ_NO_THREAD_SIGNALS=1 pools them all again).  sigsuspend and __sigwait
//! are emulated against that pending mask (they waited for a signal the host
//! kernel could never deliver, so libc returned ENOSYS) and are pinned by the
//! dynamic test signal_wait.
//!
//! A self-directed pthread_kill - the guest's abort()/raise(), and every
//! Chromium CHECK - is delivered to the GUEST rather than forwarded to the host.
//! Forwarding raises a real signal on the ocerz process, so a guest abort became
//! a host SIGABRT, ReportCrash spawned and pegged a core, and under Steam's
//! GPU-process crash loop that starved the emulation past Steam's own watchdogs.
//! Wine has a handler for these anyway, which is both the faithful emulation and
//! free of ReportCrash.
//!
//! sigreturn leaves the gs base alone.  The real kernel saves and restores it
//! only for a task with an LDT, so a base set inside a handler survives the
//! return - measured with a probe when b5279e8 first made this rule - and Wine's
//! leave_handler depends on exactly that, installing the TEB before it goes
//! back to PE code.  ocerz stashes the interrupted bases past the 56-byte
//! ucontext of the frames it builds itself, behind a cookie; only fs is taken
//! back from there.  Putting gs back as well undid leave_handler, and PE code
//! ran on the pthread base until something read %gs:0x30.
//!
//! ---- signals in native mode ----
//! Native mode has no x86 libc in front of these calls, so sigaction, signal,
//! sigprocmask, sigaltstack, raise and pthread_kill arrive as bridged calls with
//! plain arguments and hand back 0 or a positive errno.  Each is built from the
//! syscall's own body with the register plumbing peeled off - the table install
//! and its host mirror, the oldact writer, the mask and altstack updates, the
//! decision a self-directed kill makes between pending, delivered, fatal and
//! forwarded - so a program that mixes a raw syscall with a bridged call sees one
//! handler table and one mask, and a fix to either path is a fix to both.  The
//! argument checks are the native side's alone, so the syscall path goes on
//! accepting exactly what it always has: a signal outside 1..31 is EINVAL, and so
//! is any sigaction on SIGKILL or SIGSTOP, a pure query included, which is what
//! the host kernel answers under Rosetta and natively alike.  One check is
//! shared: an alternate stack smaller than MINSIGSTKSZ is refused with ENOMEM on
//! both paths, as the kernel refuses it, after the old stack has been written
//! back; the syscall path used to install it.
//! signal() installs with an empty mask and SA_RESTART, as Apple's x86 libc does
//! for any signal siginterrupt() has not been told about.
//!
//! What native mode lacks is the trampoline.  libc's sigaction hands the kernel
//! _sigtramp as sa_tramp, and ocerz_signal_deliver enters it with the handler in
//! rdi, the signal in edx, the siginfo in rcx, the ucontext in r8 and the
//! sigreturn token in r9, on a stack 8 bytes below a 16-byte boundary as though a
//! call had just pushed a return address.  The stand-in is Apple's routine
//! reassembled without its __in_sigtramp counter: the same frame push, the
//! arguments moved into (sig, siginfo, ucontext), which a one-argument handler
//! receives as harmlessly as an SA_SIGINFO one does, the ucontext and token held
//! in callee-saved rbx and r12 across the handler, then sigreturn(ucontext,
//! UC_FLAVOR, token) made as a syscall in place rather than through a stub.  It
//! realigns the stack before the call whatever it was entered with, and a ud2
//! follows the syscall, so a sigreturn that fails can only stop the thread and
//! never runs into whatever lies beyond.  It is written once into a guest page
//! of its own that is then made read-execute; the host never runs guest bytes,
//! only their translations, so the host page is simply read-only.
//!
//! A bridged raise does not deliver.  The handler's frame has to sit on the state
//! that follows the call, and that state only exists once the crossing has
//! returned, so a signal that has a handler is left pending and the bridge hands
//! it to ocerz_guest_deliver_pending afterwards; one without a handler takes the
//! syscall path's default action on the spot.  The syscall path meets the same
//! rule from the other side: its self-directed kill does build the frame at once,
//! so it writes the syscall's result first, since the frame records the state
//! the handler returns to, and a frame built before the result was written used
//! to hand the program back a raise that had run its handler and then reported
//! failure.  pthread_kill aimed at another
//! thread is forwarded to the host just as the syscall forwards it: the real
//! signal lands on the target, whose mirrored handler records it exactly as it
//! records one sent from outside the process, and that thread delivers it at its
//! own next syscall or crossing.  Only a thread running a guest cpu can be named,
//! and any other is ESRCH.
//!
//! ---- pointers the kernel will write through ----
//! Before a syscall whose buffer the kernel writes (a read, a mach receive), any
//! page of that buffer carrying translations is unarmed: a copyout onto a
//! read-only page fails instead of faulting, and for a mach receive the reply is
//! destroyed with it - wineboot hung forever on exactly that (2026-09-07).  An
//! EFAULT with armed pages about is retried once after unarming them.  Syscalls
//! whose argument structs hide pointers the ptr_mask cannot reach (connectx,
//! sendfile, recvmsg_x) are correct only when a guest address is already a host
//! address, so outside identity mode they return ENOSYS rather than let the
//! kernel read or write the wrong memory.
//!
//! __mac_syscall is one of those, and too common to refuse: every sandbox_check
//! and quarantine query goes through it, with an argument struct whose layout
//! belongs to the policy.  What goes wrong in practice is the caller's own
//! stack - out-structures and locals the struct points at - and under Wine
//! that stack is a Unix-side kernel stack in the low-shadow window, where a
//! guest address is not the host's.  So each word of the first twelve that
//! points within 64 KB below to 1 MB above the caller's rsp is rewritten to its
//! host address for the call and put back before the guest runs again; a word
//! pointing anywhere else is left alone, because an integer that happens to
//! look like a low address must not be rewritten.  The sandbox check failed with
//! EFAULT, the Metal OpenGL renderer took that as no access to the GPU, and
//! every CGLChoosePixelFormat from a Wine thread returned 10002 - wined3d found
//! no GL adapter and Counter-Strike 2 stopped at an error box.  The dynamic test
//! mac_syscall_low_stack pins it; OCERZ_NO_MACSYS_XLATE=1 turns it off and
//! OCERZ_MACSYSLOG=1 prints the policy, call and first words of each argument.
//!
//! Metal's newBufferWithBytesNoCopy hands the GPU driver memory the caller owns:
//! the x86 framework sends the address to the GPU device user client in the
//! struct input of io_connect_method, selector 9, a 104-byte struct carrying the
//! address twice (at 0x38 and 0x40) and the length at 0x48.  The kernel wires
//! the host pages at that address, and under Wine the address is a low-shadow
//! guest address, so the GPU read and wrote whatever host memory lay at the
//! guest's number.  DXMT backs D3D11 buffers this way, so R.E.P.O.'s first frame
//! died on a GPU address fault.  When those two fields hold the same low guest
//! address and the range is mapped guest memory, both are rewritten to the host
//! address for the call and put back after it, like the descriptors above.
//! Neither the address nor the length has to be page-aligned: Metal wraps any
//! range, and DXMT's ring allocator hands it memory from the Windows heap.  The
//! call is recognised by that shape alone: IOKit will not name the class behind
//! a connection port.  The host address is right even for memory the guest only
//! sees through an alias of a host region, since an alias shares the pages it
//! shows.  OCERZ_NO_GPU_NOCOPY_XLATE=1 turns it off; OCERZ_MSGNEEDLE=<value>
//! reports every outgoing message that carries a given 64-bit value, which is
//! how this one was found.
//!
//! host_create_mach_voucher_trap (Mach trap 70) takes the recipes and the place
//! for the new voucher by address, and went to the kernel untranslated, as the
//! semaphore and switch traps it shares a case with need nothing translated.
//! From a Wine thread both live on a low-shadow stack, so the kernel answered
//! KERN_MEMORY_ERROR and libdispatch, which creates vouchers for the work it
//! queues, crashed on purpose: a dispatch_async from IOSurface code on such a
//! thread was enough.  Its sibling, trap 72, already translated its pointers.
//! OCERZ_IOKITERR=1 prints every io_connect_method call that comes back with
//! kIOReturnVMError or kIOReturnBadArgument - selector, struct input and the
//! out-of-line buffers - which is what names the field a translation missed.
//! OCERZ_IOKITSEL=<selector> prints every call with that selector the same way,
//! successful or not, with the scalar inputs, the first words of the reply and
//! the guest rip; OCERZ_IOKITSEL=all prints every call.  With either knob a
//! call whose message itself fails is printed with the mach_msg error.
//!
//! ---- what a vm_region query answers with ----
//! A guest asking mach_vm_region about its own memory is asking about the guest
//! address space, not about the host arena that happens to hold it, so the reply
//! is rewritten from ocerz's own map of the guest.  Only the low-shadow map used
//! to get this; every other map handed back the host's view, where a whole arena
//! is one read-write-execute region and an image's __DATA_CONST is therefore
//! read-write.  Electron asks about a region it expects to be read-only and
//! executes an int3 when the answer is anything else, which is what killed
//! Discord's renderers.  A guest address ocerz has no mapping for still falls
//! through to the host reply, because that is the only answer left to give.
//!
//! ---- System V shared memory ----
//! shmat cannot be told where to land: the kernel refuses a fixed address even
//! over ground the arena already reserves.  So the segment is attached wherever
//! the host wants it and mach_vm_remap carries that mapping into guest space,
//! which shares the pages rather than copying them - a write from another
//! process still shows through.  The host attachment is kept, not detached, so
//! shm_nattch counts this process once, and shmdt reverses both halves.  A
//! segment two processes share also makes their ordering visible, so attaching
//! puts the jit in ordered mode for the rest of the run.
//!
//! ---- a child with nothing to translate ----
//! A spawned or exec'd binary that carries an arm64 slice and no x86_64 slice
//! runs natively in every mode, the way Rosetta runs it.  There is nothing in
//! it for ocerz to translate, and running it under ocerz anyway ended with the
//! child dying on "cannot read": Steam spawns /usr/bin/open, which on macOS 27
//! ships as arm64 only, three times a launch.  The native-mode rules for system
//! binaries that do have Intel code are unchanged.
//!
//! The developer tools run natively in every mode too: xcrun, xcode-select and
//! the shims in /usr/bin, git, clang, make, swift and the rest, which all link
//! /usr/lib/libxcselect.dylib and run the real tool from the developer
//! directory.  The Command Line Tools on Apple silicon ship libxcrun.dylib for
//! arm64 alone, so the x86_64 slice of xcrun cannot load it under ocerz or
//! Rosetta and stopped with "unable to load libxcrun", which is what an app
//! that runs xcrun saw in cache mode.  A system binary, one under /usr, /bin,
//! /sbin, /System or /Library/Apple with an arm64 slice, that names
//! libxcselect is run as itself.
//!
//! ---- failure policy ----
//! An unimplemented call reports once and returns ENOSYS rather than aborting:
//! aborting kills the guest thread where it stands, and under Wine that is often
//! inside LdrLoadDll holding the loader lock, which deadlocks every other
//! thread.  OCERZ_STRICT_SYSCALL restores the abort for bring-up.  A contended
//! os_unfair_lock whose owner died is broken the way PTHREAD_MUTEX_ROBUST does,
//! because libplatform's reaction to EOWNERDEAD is to crash the process.
//!
//! ---- diagnostics ----
//! OCERZ_SYSFAIL logs the failures a healthy guest almost never earns (ENOMEM
//! and EFAULT are what a missing pointer translation looks like from inside),
//! and the OCERZ_*LOG family - STRACE_CPU, FDOPLOG, FDLOG, MSGLOG, SOCKLOG,
//! PIPELOG, ULOCKLOG, PREADLOG, EXITLOG, MAPFAILLOG - each exist because one
//! real failure needed exactly that view.  OCERZ_MACHMSG also prints each
//! mach_msg2 result with the id it received, which is what a diff of two runs
//! needs to see where an exchange first goes differently.  IMAGE-CLOBBER is printed, always,
//! when a guest fixed-address mmap or a munmap lands on a segment of a loaded
//! image, or an mprotect takes read or execute away from an executable one; a
//! library unmapped under running code otherwise shows up only as a wild jump
//! much later.  A fatal guest thread names its process's pid and command line,
//! because under Wine a dozen processes share one stderr.
//!
//! OCERZ_SIGMASKLOG prints every change of a guest signal mask to a wide one -
//! sixteen or more signals blocked, which Wine's own masks never reach - with
//! the code and caller chain that set it and the Windows stack: sigprocmask,
//! signal delivery and sigreturn all report.  It is what showed that the eight
//! threads spinning in R.E.P.O. with every signal blocked had been through
//! libc's abort(), called from Metal's validation layer, whose SIGABRT Wine had
//! turned into an exception while the thread kept abort's mask.
//!
//! Every cpu keeps its last 24 syscalls - number, first three arguments, result,
//! and the first words a read, write or writev moved, which for a Wine client
//! are its server request and reply headers - and SIGINFO prints them under the
//! thread's Windows stack; a signal delivery goes into the same ring as a
//! negative number.  OCERZ_SYSRING=<power of two> also keeps one ring of that
//! many entries for the whole process, each tagged with the low bits of its
//! host thread id, printed at the end of the dump.  Put on the wineserver with
//! OCERZ_EXE_ENV it shows every request, reply and wakeup the server moved, and
//! that is how a Steam hang was followed from a client's unanswered wait to a
//! SIGUSR1 the server had sent and the client had lost (see vm.c).
//!
//! Guest syscall handling: emulated x86_64 syscalls onto the native arm64
//! kernel.
//!
//! Pending asynchronous signals are offered on the ENTRY edge of a syscall, not
//! only on return: the entry hook rewinds rip to the syscall instruction so the
//! call re-executes once the handler returns, and reports nonzero to say the
//! syscall must not run yet.  Whether any LDT descriptor is present tells the
//! rest of the emulator that this process has built the 32-bit world a WoW64
//! mode switch needs, and the signal frame's flavour is gated on it.
//!
//! Native mode reaches the same guest signal table through calls rather than
//! syscalls.  In cache mode a guest's sigaction goes through Apple's x86 libc,
//! which hands the kernel its own _sigtramp; native mode has no x86 libc, so
//! ocerz_native_sigtramp writes an equivalent trampoline into guest memory once,
//! and ocerz_guest_sigaction_user installs a handler from the user-level struct
//! sigaction with that trampoline filled in.  There is one table for both modes,
//! so a raw syscall and a bridged call can never disagree about a handler.  The
//! other entries mirror the syscalls a native-mode program would otherwise
//! make: they return 0 on success or a positive errno, and the bridge turns that
//! into -1 with errno set.  ocerz_guest_deliver_pending builds a frame for every
//! unmasked pending signal on the given cpu, as the syscall entry edge does, and
//! returns 1 when it built one; that is how a signal raised during a bridged
//! call, or one that arrived while the thread sat in native code, reaches its
//! handler when the crossing returns.  ocerz_guest_altstack_flags is the ss_flags
//! word sigaltstack would report, and ocerz_guest_set_onstack is what
//! sigreturn(NULL, UC_SET_ALT_STACK or UC_RESET_ALT_STACK) does, the call a
//! longjmp out of a handler uses to say the thread has left its alternate stack.
//!
//! Memory and processes reach ocerz's own implementations the same way.  In
//! cache mode an x86 libc turns mmap, fork, execve and posix_spawn into syscalls
//! and mach_vm_allocate into a Mach trap; native mode has no x86 libc, so the
//! bridge calls these entry points instead, and each is the syscall's or trap's
//! body with the register plumbing peeled off.  The memory entries keep guest
//! memory accounting, translation invalidation and the 4 KB guest page exactly as
//! the syscall path keeps them, for every range that touches memory ocerz
//! tracks; a range that lies wholly outside it is the host's own memory, a buffer
//! native malloc or a native Mach call handed the guest, and goes to the host
//! kernel after any translations of it are dropped.  The Mach entries answer a
//! kern_return_t and go to the host outright for a task other than this one.
//! ocerz_guest_fork is the fork syscall's body and answers the child 0 and the
//! parent the child's pid through pid_out; ocerz_fork_register installs ocerz's
//! own fork handlers, which native mode does before any guest code can register
//! one of its own, so that ocerz's prepare handler runs after every guest one
//! and its child handler before every guest one.  ocerz_guest_execve and
//! ocerz_guest_posix_spawn take host argument and environment vectors and start
//! the program under ocerz exactly as the syscalls do; execve returns only on
//! failure.  posix_spawn's attributes are read through the host's own getters,
//! since in native mode the guest built them with the host's
//! posix_spawnattr_init.  All of these answer 0 or an errno, as the signal
//! entries do.
#![allow(
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
#![allow(unsafe_op_in_unsafe_fn, unused_unsafe)]

use core::ffi::{c_char, c_int, c_uint, c_void};

use crate::ffi::{OcerzCPU, OcerzVM};

mod machabi;
pub(super) mod raw;
pub(super) mod util;
use machabi::*;
use util::env_set;
pub(super) mod bsd;
pub(super) mod entry;
pub(super) mod hostwq;
pub(super) mod ldt;
pub(super) mod mach;
pub(super) mod machmsg;
pub(super) mod mem;
pub(super) mod signals;
pub(super) mod spawn;
pub(super) mod sysctl;
pub(super) mod threads;
pub(super) mod workers;

pub(super) const OCERZ_BSD_MAX: c_int = 600;
pub(super) const OCERZ_ENOMEM_V: c_int = 12;
pub(super) const OCERZ_ENOTSUP_V: c_int = 45;
pub(super) const OCERZ_ENOSYS_V: c_int = 78;
pub(super) const OCERZ_NSIG: usize = 64;

pub(super) const OCERZ_MACH_KERN_SUCCESS: c_int = 0;
pub(super) const OCERZ_MACH_KERN_FAILURE: c_int = 5;
pub(super) const OCERZ_MACH_KERN_NO_SPACE: c_int = 3;
pub(super) const OCERZ_MACH_KERN_NOT_SUPPORTED: c_int = 46;
pub(super) const OCERZ_MACH_KERN_INVALID_ARGUMENT: c_int = 4;

pub(super) const OCERZ_F_PREALLOCATE: c_int = 42;
pub(super) const OCERZ_F_GETPATH: c_int = 50;
pub(super) const OCERZ_IOV_MAX: usize = 64;

pub(super) const DARWIN_NSIG: c_int = 32;
pub(super) const DARWIN_MINSIGSTKSZ: u32 = 32768;
pub(super) const DARWIN_SA_ONSTACK: u32 = 0x0001;
pub(super) const DARWIN_SA_RESTART: u32 = 0x0002;
pub(super) const DARWIN_SA_RESETHAND: u32 = 0x0004;
pub(super) const DARWIN_SA_NODEFER: u32 = 0x0010;
pub(super) const DARWIN_SA_SIGINFO: u32 = 0x0040;

pub(super) const SYSRET_FLAGS_OK: u64 = crate::inline::OCERZ_PF | crate::inline::OCERZ_AF;
pub(super) const SYSRET_FLAGS_ERR: u64 =
    crate::inline::OCERZ_CF | crate::inline::OCERZ_ZF | crate::inline::OCERZ_PF;
pub(super) const SYSRET_FLAGS_MACH: u64 = crate::inline::OCERZ_AF;
pub(super) const SYSRET_ARITH_FLAGS: u64 = crate::inline::OCERZ_CF
    | crate::inline::OCERZ_PF
    | crate::inline::OCERZ_AF
    | crate::inline::OCERZ_ZF
    | crate::inline::OCERZ_SF
    | crate::inline::OCERZ_OF;

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct ocerz_iovec {
    pub(super) iov_base: u64,
    pub(super) iov_len: u64,
}

pub(super) type ocerz_bsd_fn = unsafe fn(*mut OcerzVM, *mut OcerzCPU, *mut [u64; 8]) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct ocerz_bsd_entry {
    pub(super) name: *const c_char,
    pub(super) nargs: u8,
    pub(super) ptr_mask: u8,
    pub(super) dual_ret: u8,
    pub(super) intercept: Option<ocerz_bsd_fn>,
}

unsafe impl Sync for ocerz_bsd_entry {}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub(super) struct GuestSigact {
    pub(super) handler: u64,
    pub(super) tramp: u64,
    pub(super) mask: u64,
    pub(super) flags: u32,
}

pub(super) static mut guest_sigact: [GuestSigact; OCERZ_NSIG] = [GuestSigact {
    handler: 0,
    tramp: 0,
    mask: 0,
    flags: 0,
}; OCERZ_NSIG];

unsafe fn guest_sigact_store_user(oact: u64, sa: *const GuestSigact) {
    unsafe {
        let sa = &*sa;
        util::ocerz_st(oact, 8, sa.handler);
        util::ocerz_st(oact.wrapping_add(8), 4, sa.mask);
        util::ocerz_st(oact.wrapping_add(12), 4, sa.flags as u64);
    }
}

pub(super) static mut g_pthread_start: u64 = 0;
pub(super) static mut g_wqthread_start: u64 = 0;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ocerz_is_wqthread_exit(rip: u64) -> c_int {
    let start = unsafe {
        core::sync::atomic::AtomicU64::from_ptr(core::ptr::addr_of_mut!(g_wqthread_start))
            .load(core::sync::atomic::Ordering::Acquire)
    };
    c_int::from(start != 0 && rip == start.wrapping_add(0xf))
}

pub(super) fn ocerz_hostwq_on() -> c_int {
    unsafe {
        if crate::ffi::ocerz_mode == (crate::ffi::OCERZ_MODE_NATIVE as c_int) {
            return 0;
        }
    }
    static ON: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
    let mut on = ON.load(core::sync::atomic::Ordering::Relaxed);
    if on < 0 {
        on = c_int::from(env_set!("OCERZ_NO_HOSTWQ") == false);
        ON.store(on, core::sync::atomic::Ordering::Relaxed);
    }
    on
}

pub(super) fn ocerz_fcntl_ptr_cmd(cmd: c_int) -> c_int {
    match cmd {
        7 | 8 | 9 | 66 | 90 | 91 | 92 | OCERZ_F_PREALLOCATE | 44 | 49 | 65 | OCERZ_F_GETPATH
        | 102 | 52 | 99 => 1,
        _ => 0,
    }
}

pub(super) const SYSRET_FLAGS_OK_: u64 = SYSRET_FLAGS_OK;

pub(super) fn sysret_flags(cpu: *mut OcerzCPU, pattern: u64) {
    unsafe {
        (*cpu).rflags = ((*cpu).rflags & !SYSRET_ARITH_FLAGS) | pattern;
    }
}

pub(super) fn ret_ok(cpu: *mut OcerzCPU, value: u64) {
    unsafe {
        sysret_flags(cpu, SYSRET_FLAGS_OK);
        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] = value;
        (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] = 0;
    }
}

pub(super) fn ret_ok2(cpu: *mut OcerzCPU, value: u64, value2: u64) {
    unsafe {
        sysret_flags(cpu, SYSRET_FLAGS_OK);
        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] = value;
        (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] = value2;
    }
}

pub(super) fn ret_err(cpu: *mut OcerzCPU, errno_v: u64) {
    unsafe {
        sysret_flags(cpu, SYSRET_FLAGS_ERR);
        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] = errno_v;
        (*cpu).gpr[crate::ffi::OCERZ_RDX as usize] = 0;
    }
}

pub(super) fn mach_ret(cpu: *mut OcerzCPU, kr: u64) {
    unsafe {
        sysret_flags(cpu, SYSRET_FLAGS_MACH);
        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] = kr;
    }
}

pub(super) fn machdep_ret(cpu: *mut OcerzCPU, value: u64) {
    unsafe {
        sysret_flags(cpu, SYSRET_FLAGS_MACH);
        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] = value;
    }
}

pub(super) fn machdep_err(cpu: *mut OcerzCPU, errno_v: u64) {
    unsafe {
        sysret_flags(cpu, SYSRET_FLAGS_ERR);
        (*cpu).gpr[crate::ffi::OCERZ_RAX as usize] = errno_v;
    }
}

pub(super) fn errno_name(e: c_int) -> &'static [u8] {
    match e {
        1 => b"EPERM\0",
        2 => b"ENOENT\0",
        3 => b"ESRCH\0",
        4 => b"EINTR\0",
        5 => b"EIO\0",
        9 => b"EBADF\0",
        11 => b"EDEADLK\0",
        12 => b"ENOMEM\0",
        13 => b"EACCES\0",
        14 => b"EFAULT\0",
        17 => b"EEXIST\0",
        20 => b"ENOTDIR\0",
        21 => b"EISDIR\0",
        22 => b"EINVAL\0",
        24 => b"EMFILE\0",
        35 => b"EAGAIN\0",
        45 => b"ENOTSUP\0",
        78 => b"ENOSYS\0",
        _ => b"E?\0",
    }
}

pub(super) type RawIovec = ocerz_iovec;
pub(super) type C_void = c_void;
pub(super) type C_uint = c_uint;
