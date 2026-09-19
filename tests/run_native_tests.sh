#!/usr/bin/env bash
# Gate for the native execution mode (M2): the mode exists, it is selectable
# from the command line and from the environment, and with the shared cache
# switched off a dynamically linked guest binds every one of its system imports
# against a libSystem that ocerz synthesizes in memory, reaches main, calls
# real host code through those imports, and runs to completion.
#
# That last step is what M2 moved, and it is why the headline native case is no
# longer an exit code but an output comparison. Under M0 nothing answered a
# libSystem import at all: every one was collected at the end of the main
# image's fixups, printed as an "ocerz: native: no bridge for <sym> in <dylib>"
# line, and the process exited 71 without the guest running an instruction of
# its own. Under M1 the imports resolved to stubs inside a virtual
# /usr/lib/libSystem.B.dylib, so the guest ran, and the first call through one
# of those stubs printed a single line and exited 72. Under M2 the stub trap
# reads the arguments out of the guest's x86 register state, calls the real
# arm64 function linked into ocerz and puts the result back in RAX, so the call
# returns and the guest keeps going.
#
# An exit code cannot prove that. A guest that ran to completion having been
# handed nonsense by every bridged call exits 0 just as happily as one that was
# handed the right answers, so the native cases compare OUTPUT instead: the
# same binary, the same arguments, run under -native and under -cache, must
# write byte-identical stdout. Cache mode reaches the system libraries the way
# the rest of the suite does, through the real shared cache, so agreeing with
# it is agreeing with the real libSystem. xbench_dyn is the case worth having
# because its kernels checksum their own work: a memcpy that copied the wrong
# bytes or a strcmp that ordered two strings backwards changes the checksum,
# and the checksum is the only thing it prints. Two kernels are run rather than
# one, and each under both the JIT and -no-jit, so a bridge that only agrees
# with the translator it was debugged against is caught.
#
# The other native fixture is bridge_probe, compiled at test time because it is
# the only way to exercise the bridged calls xbench does not make. It checks
# its own results and prints one line per group -- str, mem, heap, env, misc,
# io -- each either "<group> ok" or "<group> bad:<hex>", where the hex is a
# bitmask of the checks in that group that did not hold, numbered in source
# order from bit 0. That keeps a failure legible without turning the output
# into a transcript, and keeps it exactly comparable against the cache-mode
# run. It writes those lines with write(2) and the last one with putchar and
# puts, so both paths out of a guest are covered and the buffered one is last
# in program order, where a flush at exit cannot reorder it.
#
# Two of its groups get their own cases. heap does a malloc/fill/read-back/free
# round trip, including one allocation large enough that no arena serves it,
# and env reads back a variable this script puts in the environment. Both are
# questions about the address model rather than about any one function: the
# first asks whether the guest can dereference memory the HOST heap handed it,
# the second whether it can dereference a pointer into the host's own
# environment. Native mode runs in the identity map, so both should be a
# formality, and either one failing means the map is not what the bridge
# assumes rather than that malloc or getenv is wrong.
#
# Keeping 71 and 72 alive still matters, and both now need their own fixture.
# 71 means nothing bound, so the loader never handed control to the guest at
# all; native_unbound pins it with a two-line program calling zlibVersion from
# libz, a library native mode synthesizes no image for. It used to call
# getpwnam, until the generated libSystem database exported every function the
# real libSystem does and getpwnam bound. 72 means
# everything bound and the guest ran, and what is missing is the bridge behind
# one export rather than the export itself; xbench_dyn no longer reaches it, so
# bridge_unimpl pins it with a program whose only import is scanf, which the
# virtual library exports and the bridge deliberately does not implement -- it
# is variadic, Apple's arm64 passes variadic arguments on the stack where x86-64
# passes them in registers, and no fixed signature can say where the named
# arguments stop. That fixture's one import used to be qsort, until M5 gave a
# callback a way back into guest code, and then printf, until M10 put a format
# veneer in front of it that reads the format string to learn what every
# argument is. No veneer stands in front of scanf, and the generated database
# keeps every member of its family a stub. Being the only import, it is also the
# only symbol the bridge line can name, so that case pins the symbol as well as
# the shape of the line, and it checks the fixture's import table with nm first
# so that a toolchain which starts importing something else is reported as that
# rather than as a bridge naming the wrong export. Both fixtures skip where
# there is no x86_64 clang, the way the dynamic gate does.
#
# Cache mode must come out of all this untouched, and it is checked the way the
# rest of the suite checks translation: the JIT and the interpreter running the
# same command must agree byte for byte. That needs no Rosetta on the box.
#
# M4 adds the one fault the mode could not previously explain. A crossing runs
# the host's own arm64 code on a thread the guest is driving, so a guest that
# hands strlen a pointer it had no business handing it faults inside Apple's
# code, at an instruction pointer belonging to nothing the translator emitted.
# The crash handler used not to ask whether a crossing was in flight, so it
# blamed the guest anyway: the fault was either delivered as an access
# violation at whatever rip the crossing trapped from, which is an instruction
# that did nothing wrong, or handed to a JIT recovery that tried to rebuild
# guest state from a host pc that had never been in the arena. Both read as a
# translator bug. A crossing is now looked for before anything else attributes
# the fault, and one taken inside a crossing prints a report naming the library,
# the export, its signature, the host function, the signal, the faulting
# address and the host pc, says which side of the guest boundary that address
# fell on -- inside means the guest passed a bad pointer, outside means ocerz
# marshalled the crossing wrong -- and stops the process rather than delivering
# or recovering, because the thread is several native frames deep in code ocerz
# can neither resume nor unwind.
#
# bridge_fault_native pins that report, against a fixture whose only unusual
# act is calling strlen on a page of guest space nobody mapped.
# bridge_fault_not_guest pins the other half from a second run of the same
# fixture with OCERZ_FAULTLOG and OCERZ_SIGTRACE set, which are the two knobs
# that make the old attribution path say out loud that it ran: for the bad
# address neither may print, no guest-crash report may appear, and the line the
# fixture writes after the call must never be reached. A knob that had been
# renamed would be silent for the wrong reason and the case would pass on
# nothing, so it is paired with a control run of the frame-lowered fixture under
# the same knobs, whose fault at the same address really is the guest's: both
# knobs have to speak there before their silence over the crossing means
# anything. Both halves are needed,
# because a report that names the symbol correctly and then ALSO delivers the
# fault to the guest would satisfy a test that only looked for the report. The
# status is 139, which is what the misattributing path exited with too, so the
# status distinguishes nothing here and the message carries the whole
# difference.
#
# bridge_fault_cache is the regression guard that matters most, because cache
# mode is what runs Steam and Wine today and M4 has to be purely additive. The
# same fixture under -cache crosses no bridge at all -- strlen there is the real
# libSystem's, translated like everything else -- so it must still be an
# ordinary guest fault, reported as one at the address the guest actually
# dereferenced, with the JIT and the interpreter agreeing on the status, and
# with no bridged-call report anywhere in either.
#
# bridge_frame_lowered is the failure mode of a frame raised and not lowered. A
# fixture that makes a bridged call, sees it return, and only then faults in its
# own code has to be blamed the old way, because by then it is guest code
# faulting again. A missing lower would not show up here as a wrong answer; it
# would show up much later as some unrelated fault reported as being inside a
# call that had returned long before, which is a miserable thing to diagnose
# from the report alone.
#
# bridgelog covers OCERZ_BRIDGELOG, which prints one line per crossing naming
# the library, the export and its signature. The case checks that the lines
# appear, that they name the library and an export the fixture's own import
# table lists, and that stdout is byte-identical to the same run without the
# variable: a diagnostic that changes what it observes is worse than no
# diagnostic.
#
# M5 adds calls in the other direction. Until M5 every crossing went from guest
# code into native code, but qsort and bsearch take a function pointer and call
# it, and when the guest supplies one it names x86 code native code cannot jump
# to. Such an argument is now interned to a slot of a fixed bank of arm64
# trampolines, and native code calling the slot runs the guest function on the
# calling thread, below the native caller's stack pointer, and gets its result
# back. Five fixtures cover that end to end, compiled at test time like
# bridge_probe and importing only functions the bridge implements. The four that
# are meant to succeed check their own work and write a status line first,
# "<name> ok ..." or "<name> bad:<hex> ...", with the bits numbered in source
# order the way the probe's are, and each case's failure message says what every
# bit means.
#
# callback_qsort sorts three hundred distinct ints up, down and up again with
# two guest comparators, so sorted and the right permutation are one check: the
# result must be exactly -150..149. The comparators return plus or minus 65536
# rather than one, so a result cut down to a byte or a word on the way back
# reads as equality and the sort comes out wrong. callback_nested_bridge sorts
# strings with a comparator that calls strcmp, which is a bridged crossing
# inside a guest callback inside a bridged crossing, checks the order with a
# comparison of its own rather than with the strcmp under test, and then puts
# the sorted strings themselves, so the cache comparison sees the order too.
# callback_bsearch looks up every present key and a set of absent ones through
# a key whose layout differs from the element's, and its comparator checks that
# the key pointer arrives unchanged and the element pointer lands on an element,
# so arguments delivered in the wrong order or converted wrongly cannot find
# anything. callback_recursion gives each comparator a qsort of its own over a
# small local array, with the other comparator, twenty-four levels deep, so
# native and guest frames alternate on one thread; every level must be entered
# once and left once in order, run the comparator it was handed, sort its array,
# and find its caller's elements unchanged when the level below returns, which
# is what a guest stack placed on top of a live native frame would break. Every
# comparator also checks that it was entered with the stack aligned the way the
# System V ABI promises. All four run under the JIT and under -no-jit, which
# must agree byte for byte, and native output must equal cache mode's, where the
# same comparators are called by the real x86 libSystem.
#
# callback_guest_fault is the reason M5 clears the thread's bridge frame while
# guest code runs. Its comparator dereferences the same unmapped guest address
# the M4 fixtures use, 0x6000000000 and never 0x500000000, which is where
# ocerz_vm_call maps its readable return sentinel, so a read there succeeds and
# the case would fail for a reason unrelated to bridges. When the comparator
# faults, native qsort is still live on the host stack, and a frame left
# raised for that stretch would send the fault down M4's path: a BRIDGE-FAULT
# report blaming _qsort and an immediate stop, for a fault that is entirely the
# guest's, and one a guest with its own SIGSEGV handler is entitled to recover
# from. So the case demands exactly what bridge_frame_lowered demands of a
# fault in plain guest code -- a guest-crash report naming the address, status
# 139, the line after qsort never written -- under both engines, and fails on
# any BRIDGE-FAULT line at all.
#
# M6 lets native code call guest code on threads the guest never created. Every
# M5 callback ran on the thread that made the crossing, a thread that already had
# a guest cpu, but a native framework also calls back on threads of its own --
# libdispatch's workers above all -- and such a thread has no guest cpu, no guest
# stack and no thread block behind gs. The callback dispatcher now gives it a
# personality the first time it calls a guest function, and the proof is
# libdispatch itself: dispatch_async_f, dispatch_sync_f and dispatch_apply_f,
# with the global queues, the semaphores and dispatch_release around them,
# called from plain C. Four of the five attach_* fixtures are shaped like the
# callback ones, a single status line "<name> ok ..." or "<name> bad:<hex> ..."
# with the bits in source order, run under both engines and compared with cache
# mode, where the real x86 libdispatch runs the same work functions on guest
# worker threads.
#
# attach_async submits a work function to a global queue and waits on a
# dispatch_semaphore the work function signals, twice, so the second submission
# usually lands on a worker that already has a personality. The work function
# records a value the main thread checks and the address __error() returns,
# which is per thread in both modes, so a work function run on the calling
# thread fails instead of passing on nothing. It is the first guest code in the
# project to run on a thread the guest never created, and its failure message
# says what that means. attach_apply runs dispatch_apply_f over 384 iterations,
# each doing enough work that libdispatch spreads them over several workers and
# the calling thread at once and each writing only its own slot: every slot must
# be written exactly once with the value the main thread computes for it, and at
# least one iteration must have run off the calling thread, or nothing was
# attached and the case proved nothing. How many threads took part varies from
# run to run, so the fixture writes that to stderr, where the PASS line picks it
# up, and never to the stdout the comparisons read. attach_nested_bridge's work
# function calls strlen and strcmp itself and then qsort with a guest comparator
# that calls strcmp, so a worker's personality carries a crossing, a callback
# inside it and a crossing inside that, and the comparator must run on the work
# function's own thread. attach_sync calls dispatch_sync_f, which runs the work
# on the calling thread, one that already has a guest cpu: the work function's
# frame must sit just below its caller's on the caller's own guest stack, which
# it would not if a second personality had been built for a thread that did not
# need one, and the caller's own state must come through the call intact.
#
# attach_guest_fault is callback_guest_fault moved onto a worker. Its work
# function writes a mark and then reads the same unmapped 0x6000000000, and the
# process must end the way a guest fault ends everywhere else in this gate -- a
# guest-crash report naming the address, status 139, nothing written after the
# read and no BRIDGE-FAULT line -- under both engines. The worker is running
# guest code when it faults, so a bridged-call report there means the attached
# thread's bridge frame was not cleared while its callback ran. It is not
# compared with cache mode, where the thread that faults is one ocerz started
# for the guest and nothing is attached.
#
# The failure these cases are likeliest to meet is not a wrong answer but a wait
# that never returns: a work function that never runs leaves the main thread in
# dispatch_semaphore_wait forever, and an iteration that never comes back leaves
# it in dispatch_apply_f. So every attach run is bounded at ATTACH_TIMEOUT
# seconds, shorter than the gate's usual bound, and each fixture writes progress
# notes to stderr -- calling a dispatch function, returning from it, the work
# function entering guest code, signalling, the wait returning -- so a case that
# times out says how far it got, and says first whether the callback dispatcher
# reported that it could not attach a personality at all.
#
# M7a gives native mode guest threads without a thread mechanism of its own.
# pthread_create's start routine is a callback held past the call and run on a
# thread the guest never created, which is M5 and M6 together, so pthread_create
# is bridged as i(ppc{p(p)}p): the native pthread_create starts a host thread,
# that thread enters the callback trampoline, is given a guest personality on the
# way in and runs the start routine, and the value the start routine returns comes
# back through pthread_join. pthread_self, pthread_detach and the mutex and
# condition functions are forwarded as they are, which is sound only because
# pthread_mutex_t, pthread_cond_t and their static initializers have the same
# size, alignment and signature on x86_64 and arm64, so a mutex a guest
# initialized statically is a valid native mutex. Four of the five thread_*
# fixtures are shaped like the attach ones, a single status line with the bits in
# source order and progress notes on stderr, run under both engines and compared
# with cache mode, where the real x86 libpthread runs the same start routines on
# threads ocerz creates for the guest.
#
# The thread, tlv_*, signal_*, cf_*, M10 and M11 fixtures alone are built without
# -fno-stack-protector, and that is deliberate. clang emits the stack protector
# by default, and a protected x86_64 function reads ___stack_chk_guard, which is
# a data symbol rather than a function. Until M7a the virtual libSystem exported only functions, so a
# protected program could not bind in native mode at all, which is to say nearly
# no real program could, and every older fixture passes the flag because it was
# written while that was true; they keep it, so that each still isolates the one
# thing it was written for. M7a exports the guard and the thread fixtures prove
# it. thread_bridged_work and thread_guest_fault keep buffers of their own on the
# new thread's stack and the others the buffer of the progress note they write,
# so the protector reads the guard on the new thread as well as in main, and each
# case checks with nm that its fixture really imports ___stack_chk_guard, because
# a toolchain that stopped emitting the protector would let the case pass without
# proving anything. A guard that does not bind is an unresolved import like any
# other, and the case names it as the missing data export. These fixtures also
# include <pthread.h> where the older ones declare what they call by hand, because
# the real x86_64 PTHREAD_MUTEX_INITIALIZER is the thing being forwarded.
#
# thread_create_join creates eight threads and joins them, then does it again, so
# the second eight run on host threads created after the first eight were torn
# down. Each start routine returns a value derived from its argument with bits
# set above bit 32, so a result cut to 32 bits anywhere between the guest's RAX
# and pthread_join reads as wrong, and records pthread_self(), which must equal
# the pthread_t pthread_create handed back, differ from the main thread's, stay
# the same across the routine and not be shared by two threads alive at once.
#
# thread_mutex runs eight threads that each take a PTHREAD_MUTEX_INITIALIZER mutex
# 25000 times and, holding it, read a shared counter, add one and write it back;
# the count must come out exactly 200000. The main thread holds the mutex while it
# creates them, so all eight contend from the start. The count is sized so that a
# mutex which did not exclude would almost certainly lose increments: a build of
# this fixture with the lock and unlock replaced by calls that take time and
# exclude nothing lost more than two thirds of the 200000 on every run under
# Rosetta. The fixture also counts entries into the critical section that find
# another thread already inside, and keeps a second tally under a mutex from
# pthread_mutex_init.
#
# thread_cond hands a thousand values from a producer thread to a consumer thread
# through one mutex and two condition variables, one statically initialized and
# one from pthread_cond_init, and every wait sits in a loop that rechecks its
# predicate, so a spurious wakeup costs a turn of the loop and nothing more. The
# consumer asks for each value and looks for it without letting go of the mutex
# it asked under, so the producer cannot have supplied it yet and the consumer
# really blocks in pthread_cond_wait at least once per value: fewer waits than
# values means the slot was filled while the consumer held the mutex. Each value
# carries its sequence number and a payload derived from it, so a value lost,
# repeated, reordered or corrupted each sets a bit of its own, and every thread
# checks on taking the mutex, including on return from a wait, that no other
# thread holds it. The main thread waits for the consumer to finish on the same
# condition the producer waits on, which is why an ask is a
# pthread_cond_broadcast: a signal could wake the main thread instead of the
# producer and leave both waiting.
#
# thread_bridged_work runs six threads that make bridged calls from their start
# routines -- strlen and memcpy into a buffer on the thread's stack, malloc,
# memcpy and free on the heap, and qsort with a guest comparator that calls
# pthread_self -- so each thread carries crossings, callbacks inside them and a
# crossing inside those. The comparator must run on the thread that called qsort,
# and each thread's checksum must equal the one the main thread computes
# afterwards without a bridged call.
#
# thread_guest_fault is attach_guest_fault moved onto a guest thread. Its start
# routine writes a mark and then reads the same unmapped 0x6000000000, and the
# process must end the way a guest fault ends everywhere else in this gate -- a
# guest-crash report naming the address, status 139, nothing written after the
# read, pthread_join never returning and no BRIDGE-FAULT line -- under both
# engines, with identical output. A bridged-call report there would mean the new
# thread's bridge frame was raised while its start routine ran. It is not compared
# with cache mode, for the reason attach_guest_fault is not.
#
# The thread runs are bounded at THREAD_TIMEOUT seconds, and their fixtures write
# progress notes to stderr -- calling and returning from pthread_create and
# pthread_join, a start routine entering and leaving guest code, the main
# thread's own condition wait -- because the likeliest failure is again a wait
# that never returns: a thread that never reaches its start routine leaves the
# main thread in pthread_join forever, and a mutex or condition that never wakes
# its waiter leaves a start routine blocked. A timeout says which it was, in
# cache mode as well as native, since the notes are written in both.
#
# M7b gives native mode thread-local variables. A __thread variable is reached
# through a descriptor in its image's __thread_vars section, whose first word is
# a thunk the compiler calls with the descriptor's address in RDI and which must
# return the variable's address in RAX. In cache mode that thunk is dyld's own
# x86 tlv_get_addr out of the shared cache. Native mode has no such code: the
# thunk slot of every descriptor binds to __tlv_bootstrap, which the virtual
# libSystem did not export, so a program with a single thread-local variable
# exited 71 without running an instruction. The virtual libSystem now exports
# it, the loader rewrites each image's descriptors with a key of ocerz's own,
# and the trap behind the export answers from a table of per-image blocks that
# each thread keeps in its guest thread block: a block is allocated from the
# image's template the first time a thread touches one of that image's
# variables, and a thread's blocks are freed when an attached thread, which
# every pthread_create thread is, goes away. Six tlv_* fixtures cover it, shaped
# like the thread ones -- a single status line with the bits in source order and
# progress notes on stderr, run under both engines, which must agree byte for
# byte, and compared with cache mode, where the real tlv_get_addr serves the
# same descriptors. Each case checks with nm that its fixture imports
# __tlv_bootstrap, which is what shows the variables were compiled as
# descriptors bound to that thunk at all, and ___stack_chk_guard, and that it
# imports nothing the bridge does not implement, so that a failure is about
# thread-local variables and not about an import the fixture had no business
# making. A run stopped by the unresolved thunk says so by name.
#
# tlv_main is the plainest of them: an int and a double with distinctive
# initializers, read on first touch and then changed by forty-eight calls to a
# function that adds to both, each round read back in main and through other
# functions, with each variable's address the same whether taken in main or
# returned from elsewhere. The thunk's convention is not the ordinary one: the
# compiler may keep RSI, RDX, RCX, R8, R9 and R10 live across the call, and at -O1
# it does. So the fixture also calls a function whose six 64-bit arguments are
# still needed after a thread-local access in its body; a trap, or a translated
# path, that disturbed one of them would otherwise go unnoticed until a program
# computed something wrong with it.
#
# tlv_bss is about the part of a block the template does not supply. A 1 MB
# thread-local array with no initializer lies in __thread_bss, after a 32-byte
# initialized one in __thread_data, and every byte of it must read zero on first
# touch, then read back a pattern written over all of it without the initialized
# array or a second zero-initialized variable changing. Before that first touch
# the fixture fills an ordinary global array of the same size, which the linker
# lays out just past the thread-local sections, so a block copied from the wrong
# range of the image reads non-zero, and a thread-local array that is really
# ordinary data shows up as the ordinary array changing.
#
# tlv_layout gives one image fifteen thread-local variables of different sizes
# and alignments -- char, short, int, signed char, long long, double, float, a
# padded struct, an int array, a pointer, a 16-byte-aligned array and a trailing
# char, with three that have no initializer placed among them -- so the linker
# gives each an offset of its own. Every initializer reads back under a bit of
# its own, so a wrong offset names its variable. The pointer is initialized with
# the address of a string, which puts a rebase inside __thread_data: a block
# copied from the file, or from the image before its fixups were applied, holds
# something other than the string's address. Every address must sit on its
# type's alignment, no two variables may overlap and all of them must fit in a
# page; then every variable is written with a new value and each must read back
# its own. That last check is the old descriptor bug as a guest sees it: every
# variable in an image collapsed onto offset 0, so writing one wrote them all. A
# copy of this fixture with every descriptor's offset patched to 0 sets fourteen
# of its seventeen bits under Rosetta, and tlv_main and tlv_bss fail the same way.
#
# tlv_threads starts six threads one at a time, each only after the main thread
# and every thread before it have replaced their own values, so a thread whose
# first read is anything but the initializers, or whose thread-local array is not
# zero, is reading a block that is not its own. Each thread then writes values of
# its own, checks them, and waits until all six have done the same, so the
# addresses they recorded belong to seven threads alive at once and must all
# differ. Released, each checks that nothing the others wrote reached its copy,
# and the main thread checks its own copy last.
#
# tlv_thread_churn creates and joins 320 threads one after another. Each must
# start from the initializers and from a 2 MB thread-local array that is zero
# where the thread before it wrote, and then writes values of its own and a mark
# on every 4 KB page of the array. A thread's blocks are freed when it goes
# away, so the memory a later thread is handed may be memory an earlier one
# wrote: a block reused without being initialized again, or a table that
# outlives its thread and is found by the next one, hands a thread its
# predecessor's values, and the status line counts such threads as stale. A leak
# shows nowhere in the output, so the case measures one. The JIT run happens
# under /usr/bin/time -l, as does a second run of only 16 threads, and the
# larger run's peak memory footprint may exceed the smaller's by no more than a
# quarter of a block for each extra thread. Under Rosetta the two runs came out
# 4 MB apart, and a copy of the fixture that also allocated and touched 2 MB per
# thread without freeing it put the larger run 640 MB ahead. Both engines must
# also finish inside TLV_CHURN_BUDGET seconds, half the bound on the run, so
# that a cost that grows with every thread fails as a cost rather than as a
# timeout. The measured runs are the only ones in this gate with a process
# between the timeout and ocerz, so one that times out also kills whatever of
# the fixture is still running, since the fallback timeout loop signals only its
# own child.
#
# tlv_dylib moves thread-local variables into a disk dylib, built at test time
# with the install name @executable_path/libtlvdep.dylib and written beside the
# fixture, which links against it by path, so the loader has to register a second
# image's descriptors under a key of their own. The dylib's imports are checked
# the way a fixture's are. Its variables must read their initializers on the main
# thread and on four threads alive at once, at distinct addresses, and must not
# share a block with the main image's own thread-local int, which lies at offset
# 0 of its image's block just as the dylib's first variable does in the dylib's.
# The main image also reaches the dylib's int directly, through an import of the
# thread-local symbol itself, and the address that yields must be the one the
# dylib's own accessor returns. A dylib that never loaded leaves every import from
# it unresolved, and the case then says the loader did not find it rather than
# blaming thread-local variables.
#
# The thread-local runs are bounded at TLV_TIMEOUT seconds and write the same
# progress notes as the thread fixtures, so a run that times out says how far it
# got in the same terms.
#
# M7c gives native mode signals. With no x86 libc in front of them, sigaction,
# signal, sigprocmask, pthread_sigmask, sigaltstack, raise, kill and pthread_kill
# arrive as bridged calls, and the handler table, the mask and the alternate stack
# they change are the ones the same syscalls change in cache mode. A handler is
# guest code, so ocerz has to build its frame on the guest's stack, enter it and
# take it back through sigreturn, just as it does for a signal delivered at a
# syscall. Nine signal_* fixtures cover it. Eight are shaped like the thread ones
# -- a status line "<name> ok ..." or "<name> bad:<hex> ..." with the bits in
# source order and progress notes on stderr, run under both engines, which must
# agree byte for byte, and compared with cache mode, where the real x86 libSystem
# makes the same calls as syscalls. Each case checks with nm that its fixture
# imports nothing the bridge does not implement, so that a failure is about
# signals and not about an import the fixture had no business making.
#
# ocerz delivers a signal only at an edge where it has control again: in cache
# mode a syscall, in native mode the return of a bridged call. A raise is its own
# edge in both modes, so its handler has run by the time it returns, and the
# fixtures rely on that. A guest spinning in its own code reaches no edge at all
# and never sees a signal sent from anywhere else, in either mode, so every
# fixture that waits for a handler calls getpid on each turn of the wait, a
# syscall in cache mode and a crossing in native mode, and gives up after
# SIGNAL_WAIT_SECS seconds with a bad status line rather than spinning into the
# run's bound.
#
# signal_sigaction installs an SA_SIGINFO handler for SIGUSR1 and raises it from
# a function holding six 64-bit values and a double read from volatile globals, a
# 48-byte pattern on its own stack and a running checksum, every one of them still
# needed after raise returns, so the compiler keeps them in callee-saved registers
# and stack slots that the handler's frame and sigreturn must leave alone. The
# handler must be handed SIGUSR1, a siginfo naming it and a non-null ucontext,
# raise must return 0 with the handler already run, and the checksum must equal
# one computed without a signal. A second sigaction must hand back the first
# handler, its SA_SIGINFO flag and its mask as the old action, a query with no new
# action must report the second, and a raise must then run the second handler and
# not the first.
#
# signal_signal is the older interface. The first signal(SIGUSR2, h) must return
# SIG_DFL and the second h, the handler must run on each of two raises and stay
# installed between them, a SIGUSR1 set to SIG_IGN must be raised without ending
# the process or running anything, and restoring both defaults must hand back
# SIG_IGN and h.
#
# signal_mask blocks SIGUSR1 and raises it. The handler must not run, a query with
# no new set must report SIGUSR1 blocked, and the handler must have run exactly
# once by the time the call that unblocks it returns, which is where a pending
# signal falls due. It does that with sigprocmask, with pthread_sigmask, and once
# blocking with the first and querying and unblocking with the second, since the
# two share one mask; the last query of each round passes how as 0, which the
# kernel never looks at when there is no new set. Inside the handler a
# pthread_sigmask query must find SIGUSR1 itself blocked, and SIGUSR2 too, which
# the handler's sa_mask names, and both must be open again once it has returned.
# The fixture also calls sigemptyset, sigaddset, sigdelset, sigfillset and
# sigismember as functions rather than through the header's macros, which is the
# only way a program comes to import them, and checks what they build against
# what the macros build.
#
# signal_altstack queries sigaltstack before installing anything and must find
# SS_DISABLE, installs a malloc'd alternate stack of four times SIGSTKSZ and must
# read back the same ss_sp and ss_size, then raises one signal whose handler has
# SA_ONSTACK, which must find the address of a local of its own inside that stack,
# and one whose handler does not, which must not. Back from both, the thread must
# no longer be on the alternate stack; once it is disabled again the query must
# report SS_DISABLE and the SA_ONSTACK handler must run on the thread's own stack.
# The handler deliberately does not query sigaltstack itself: macOS answers
# SS_DISABLE there, natively and under Rosetta, and cache mode answers SS_ONSTACK,
# so the answer would compare two things neither of which this gate is about.
#
# signal_pthread_kill first sends SIGUSR1 to the main thread with pthread_kill,
# whose handler must have run on that thread before the call returns. It then
# starts a worker that waits for the handler in the getpid loop, sends the worker
# SIGUSR1 once it is running and joins it. The handler records pthread_self, and
# it must have run exactly once, on the worker, before the worker gave up. A
# signal aimed at another thread is the one delivery that cannot happen at the
# sender's own edge: it has to reach the target by way of the host and be
# delivered at the target's next crossing.
#
# signal_kill sends SIGUSR1 to the whole process with kill(getpid(), ...). Which
# thread the host kernel hands a process-directed signal to is its own choice, and
# in native mode it may pick a thread ocerz runs rather than the guest's, so the
# fixture waits for the handler in the getpid loop rather than demanding it before
# kill returns. The handler must still run exactly once, and on the main thread,
# the only thread the guest has, and kill with signal 0 must succeed and deliver
# nothing.
#
# signal_errors writes what its calls returned, not only whether they failed,
# over two lines, so the comparison with cache mode judges the values themselves.
# sigaction on SIGKILL, SIGSTOP, 0, 200 and NSIG, and a query of SIGKILL with no
# new action, must each return -1 with errno EINVAL, and so must raise and kill of
# signal 200; pthread_kill must return EINVAL for 200 and NSIG, and sigaltstack
# must refuse a stack one byte short of MINSIGSTKSZ with ENOMEM. Signal 0 is the
# other side of that line: raise, kill and pthread_kill of 0 ask only whether the
# target exists and must succeed, so a range check that turned away 0 along with
# the numbers past the end shows up there. Natively, under Rosetta and in cache
# mode every one of these rows agrees.
#
# signal_default raises SIGWINCH and then SIGTERM with no handler installed for
# either. SIGWINCH is ignored by default, so the process must carry on past it;
# SIGTERM ends the process by default, so it must stop inside that raise, never
# writing the line after it, with status 143, which is 128 plus SIGTERM's 15, and
# cache mode must stop at the same place with the same status. bash reports a
# child that a signal ended on its own stderr, so this case's runs are made with
# the script's stderr sent to /dev/null.
#
# signal_handler_bridge's SIGUSR1 handler makes bridged calls while it runs:
# strlen, memcpy into a buffer on the handler's own stack, strcmp, a write of a
# line to stdout, and a raise of SIGUSR2, whose handler writes a line of its own
# and must run nested inside the first, before that raise returns. The main
# thread raises SIGUSR1 four times, so stdout holds eight handler lines in a fixed
# order ahead of the status line, and cache mode must write the same lines in the
# same order. A crossing inside a handler is a crossing made while a delivery is
# still in progress, and the signal delivered at the return of the nested raise is
# a signal delivered inside a handler.
#
# The signal runs are bounded at SIGNAL_TIMEOUT seconds, and a run that times out
# names the last progress note its fixture wrote. A fixture whose handler was
# never found usually does not time out at all: the signal takes its default
# action and ends the process, with status 158 for SIGUSR1 or 159 for SIGUSR2, and
# the case says that is what the status means.
#
# M8 gives native mode its first framework. A program linking CoreFoundation
# binds its imports to a second virtual library, whose functions are crossings
# into the host's own arm64 CoreFoundation and whose data exports are the host's
# own variables: ___CFConstantStringClassReference, which every CFSTR literal in
# the guest's __cfstring section carries as its isa, kCFTypeArrayCallBacks and
# its dictionary siblings, which CoreFoundation may recognize by address, the
# allocators, the booleans, kCFNull, the run loop modes and the three special
# numbers. A CFTypeRef crosses unchanged in both directions, because native mode
# runs in the identity map, where the object CoreFoundation returns is an
# address the guest can hold, compare and hand back. What the bridge does
# convert is a structure of function pointers passed by address -- an array's or
# a dictionary's callbacks, a timer's, observer's or source's context -- whose
# guest words it replaces with callback trampolines in a copy it passes instead.
# Three cf_* fixtures cover it, compiled at test time against the SDK's own
# CoreFoundation headers. Each writes one status line per group of checks,
# "<name> <group> ok ..." or "<name> <group> bad:<hex> ...", with the bits
# numbered in source order within the group, and then a last line, "<name> ok"
# or "<name> bad:<hex>", whose bits are the groups that failed, in the same
# order, so a failure names its group before its check. Progress notes go to
# stderr, as in the signal fixtures. The fixtures run under both engines, which
# must agree byte for byte, and are compared with cache mode, where the x86
# CoreFoundation out of the shared cache makes the same calls.
#
# These cases have an oracle no earlier case had. Native mode calls the arm64
# CoreFoundation and cache mode the x86 one, which is the same source built for
# the other architecture, so each fixture is also built for arm64 and run
# directly on the host, and its stdout must match the native run's as well. That
# run is made first, because an arm64 build failing its own checks means the
# fixture expects something CoreFoundation does not do, and then nothing about
# the bridge can be read from the native run at all. Every line is written so
# that both builds of CoreFoundation give the same answer: retain counts are
# compared and printed only as deltas, since a constant or tagged object's
# absolute count is whatever the framework says it is; hashes and pointers are
# compared only with each other; and a description is searched for the text the
# fixture put into it rather than printed, since it is full of addresses. Each
# case checks with nm that its fixture imports nothing but names the virtual
# CoreFoundation exports and libSystem functions the bridge implements, so that
# a failure is about CoreFoundation and not about an import the fixture had no
# business making. None of them is variadic, which rules out
# CFStringCreateWithFormat, and none takes a structure by value, which rules out
# every function taking a CFRange.
#
# Boolean and UniChar results are where the two conventions part. arm64 extends
# them to 32 bits and x86 leaves everything above AL or AX undefined, so the
# fixtures store every Boolean CoreFoundation returns in an int and compare it
# with true or false rather than test it for zero, where a result of 2 would
# pass, and read back characters above 0xff, where a result cut to a byte
# changes.
#
# cf_basic makes the calls that take no guest code, in eight groups. string
# creates a string from UTF-8 holding ASCII, Czech letters, a euro sign and a
# character outside the BMP, and must read back its twenty UTF-16 units one by
# one, its bytes through CFStringGetCString, and false from CFStringGetCString
# into a buffer too small and into ASCII; CFStringCreateWithBytes must build the
# same strings from UTF-16LE bytes and from a prefix of the UTF-8 ones, and must
# read the same four kCFStringEncodingUnicode bytes in host order when
# isExternalRepresentation is false and big-endian when it is true, which is the
# one place a Boolean argument shows whether it arrived. literal is about CFSTR
# literals themselves: an ASCII one and one the compiler lays down as UTF-16
# must be strings, equal to and hashing like the same text created at run time,
# with CFRetain handing the literal back unchanged, and the case checks that the
# fixture imports ___CFConstantStringClassReference, without which it would
# prove nothing about the isa. mutable appends to a mutable string and asks
# CFStringCompare, which must answer exactly -1 where a CFComparisonResult cut
# to 32 bits would not, CFStringHasPrefix, CFStringHasSuffix and the integer and
# double values of strings. split separates a string with an empty field and
# combines it back. array holds three objects in a CFArray with
# kCFTypeArrayCallBacks, each of which must come back as the very pointer that
# went in, the same pointer on every read, retained exactly once while it is
# held, and a mutable array must keep its order and its references through an
# append, an insert, a set, a remove and a remove-all. dict looks values up
# through keys created separately from the ones stored, checks that
# CFDictionaryGetKeysAndValues pairs every key with its own value, and uses
# kCFCopyStringDictionaryKeyCallBacks to show that a mutable key was copied on
# the way in. number round-trips SInt32, SInt64 and Float64 values through
# CFNumber, compares them and reads kCFBooleanTrue, kCFBooleanFalse and the
# special numbers, and data creates a CFData and watches its retain count move
# by one and back.
#
# cf_callbacks puts guest code where CoreFoundation calls it, in four groups.
# array gives a mutable array retain, release, copyDescription and equal
# callbacks that are all guest functions, and counts their calls through
# appends, a remove, a set, an insert, a CFEqual against an array of equal
# elements and against one differing at its fourth element, a CFCopyDescription
# and a mutable copy, down to the last release, when every element's own count
# must be back at zero. One element's retain callback hands back a stand-in
# instead of the element, and the array must hold the stand-in, which is the
# proof that a guest callback's pointer result reached CoreFoundation. Every
# array in the group is created with kCFAllocatorMalloc and every callback must
# be handed that allocator, because CFArrayCreate passes a retain callback the
# allocator its own caller passed while the mutable paths pass the array's, so a
# null allocator would reach the guest as two different pointers. mixed copies
# kCFTypeArrayCallBacks and replaces only equal with a guest function that
# compares case-insensitively through a bridged CFStringCompare, so one
# structure holds native retain, release and copyDescription words beside one
# guest word: the native words must still retain and release, CFEqual must call
# the guest word, and an array with the unmodified callbacks must not compare
# equal at all, since CoreFoundation compares the two equal pointers before it
# calls either. dict gives a dictionary guest hash and equal callbacks over
# C-string keys and looks every key up through a copy at another address, so
# equal has to run, and apply walks two dictionaries with
# CFDictionaryApplyFunction and a guest applier that checks its context and sums
# what it is handed, making bridged CFNumber calls from inside the applier for
# one of them.
#
# Two things in cf_callbacks are there to catch a trampoline rather than a
# CoreFoundation call. On x86_64 its three equal callbacks are declared to
# return 64 bits and return their Boolean in the low byte with bits set above
# it, which the x86 convention permits and a caller reading AL never sees, while
# the arm64 CoreFoundation reads all of w0, so a trampoline that hands back RAX
# without narrowing it turns every false into true. The arm64 build returns a
# plain Boolean, as its own convention requires; an arm64 build given the x86
# return instead fails every group. And the dict line prints a checksum of the
# order in which CFDictionaryGetKeysAndValues hands the keys back. That order
# follows the bucket each key's hash selects, so it comes out the same from
# either CoreFoundation given the same 64-bit hashes, and a hash cut to 32 bits
# on its way back from the guest moves keys between buckets: an arm64 build that
# makes that cut itself passes its own checks and prints a different order.
#
# cf_runloop runs the main thread's run loop with a guest timer, observer and
# version-0 source in the default mode, all with context retain and release
# callbacks that count. The timer is created far in the future and moved to
# 10 ms from now with CFRunLoopTimerSetNextFireDate, which
# CFRunLoopTimerGetNextFireDate must then report exactly, and it repeats every
# 10 ms; its first two callouts signal the source and wake the run loop, its
# third calls CFRunLoopStop, and CFRunLoopRunInMode, bounded at CF_RUN_SECS
# seconds, must return kCFRunLoopRunStopped. The source's perform callout must
# have run once between each pair of fires, which is where the run loop services
# a signalled source, and its schedule and cancel callouts once each, handed the
# main run loop and the default mode, when it is added and when it is removed,
# with the invalidation after the removal cancelling nothing more. The observer
# asks for kCFRunLoopEntry and kCFRunLoopBeforeWaiting and must see exactly one
# entry, first, and at least one wait. A wait before the first fire is certain,
# because until a callout has signalled the source nothing lets the run loop
# poll instead of sleeping. Later ones are not: after each of the first two
# fires the run loop polls for the signalled source, and a next fire that has
# already fallen due by then, which a thread preempted for longer than the
# interval would find, is taken without a wait between. On an idle machine there
# are three, but the count is checked only for that first wait and never
# printed. Once the run is over every object is removed, invalidated and
# released, and the context callbacks must balance. CoreFoundation retains a
# timer's info around every callout and releases the last of it when the timer
# is invalidated rather than when it is freed, so the timer's context sees four
# retains and four releases by the time CFRunLoopTimerInvalidate returns, and
# the observer's and the source's one of each.
#
# The cf runs, the arm64 ones included, are bounded at CF_TIMEOUT seconds, and a
# run that times out names the last progress note its fixture wrote: the group
# it was in, or for cf_runloop how far the run loop got. A fault inside a
# crossing is reported as the BRIDGE-FAULT it is, and in the two fixtures that
# hand CoreFoundation guest functions the case adds that native code jumping to
# a guest function pointer left unconverted in a structure looks exactly like
# that.
#
# M10 gives native mode Objective-C and Foundation, and the variadic functions
# nearly every program calls. A program linking Foundation binds its imports to
# two more virtual libraries, libobjc and Foundation, generated from the SDK
# like the others, whose class references are the host's own class objects, so
# the guest holds arm64 objects the way it holds CoreFoundation's. objc_msgSend
# has no signature a database could give it, since it takes whatever the method
# it reaches takes, so its stub goes to a handler that canonicalizes the
# selector the guest's image holds, looks the method up in the native runtime
# and takes the argument classes from the method's own signature there. A
# variadic function has no complete signature either, and Apple's arm64 passes
# every variadic argument on the stack where x86-64 passes the first ones in
# registers, so printf and its relatives in libSystem, CFStringCreateWithFormat
# and CFStringAppendFormat, and NSLog are format veneers: the handler walks the
# format string to learn the class of each argument that follows, gathers them
# from the x86 registers and stack in that order, and makes the native call with
# them where arm64 wants them. A variadic selector, stringWithFormat: or
# arrayWithObjects:, has the same problem inside a message send, and its
# arguments have to be moved the same way. Four fixtures cover it, compiled at
# test time. Three are Objective-C, built with -fobjc-arc against the SDK's
# Foundation, and define no class or category of their own, which is M11's. All
# four are shaped like the cf_* ones: one status line per group of checks,
# "<name> <group> ok ..." or "<name> <group> bad:<hex> ...", with the bits
# numbered in source order within the group, then "<name> ok" or
# "<name> bad:<hex>" with a bit per failed group, and progress notes on stderr.
#
# objc_foundation sends the messages that give Foundation no guest code to call,
# in six groups, all inside @autoreleasepool, so the pool's push and pop cross
# too. string creates a string holding Czech letters and a euro sign with
# stringWithUTF8String:, which must read back its fifteen UTF-16 units, three
# characters above 0xff and its UTF-8 bytes; it must be isEqual: to the @"..."
# literal of the same text in both directions and hash like it, and
# uppercaseString, substringWithRange:, which takes an NSRange by value, and
# rangeOfString:, which returns one, must give the right text and ranges, with a
# string that is not there found at exactly NSNotFound, which a location cut to
# 32 bits is not. tagged puts a five-letter ASCII string, which Foundation hands
# back as a tagged pointer with no isa to read, and one too long to be tagged
# through the same length, character, equality, hash and range messages, and
# appends one to the other. number reads back numberWithInt:, numberWithDouble:
# and numberWithLongLong:, and the @42 and @3.5 literals, which clang lays down
# as constant objects whose isa is Foundation's NSConstantIntegerNumber and
# NSConstantDoubleNumber, so their class must be the one NSClassFromString
# names; compare: must answer exactly -1, 0 or 1. collection builds @[...] and
# @{...} literals from objects created at run time, which clang compiles to
# arrayWithObjects:count: and dictionaryWithObjects:forKeys:count: over arrays
# on the guest's stack, and every element must come back as the very pointer
# that went in, the same pointer on every read, whether fetched by index, by
# subscript or by a key created separately at another address, and
# containsObject: must find an equal string at another address. describe takes
# the description of an array of strings and numbers, and the fixture puts its
# UTF8String after the summary line, so the one buffered write is last in
# program order. range crosses NSRange by value through Foundation: rangeValue
# on a valueWithRange: whose location lies above 32 bits, since NSMakeRange is
# inline and never crosses, and NSStringFromRange, NSRangeFromString,
# NSIntersectionRange and NSUnionRange, which are C functions taking or
# returning the structure; the line carries the text NSStringFromRange produced.
#
# objc_variadic makes the variadic calls, in four groups, and writes every
# string it builds on a line of its own after its group's status line, as well
# as comparing it with the text it must hold. format sends stringWithFormat: a
# format mixing %d, %@, %.2f, %s, %ld, %u, %lld, %x, %e, %g, %c, %zu, %hhd and
# %hd over sixteen integer-class arguments and nine doubles, so on x86-64 the
# first few of each class arrive in registers and the rest on the stack,
# interleaved there in argument order, and then sends initWithFormat: to an
# allocated string and stringByAppendingFormat: to the result. objects sends
# arrayWithObjects: with three objects and with seven before the nil, and
# dictionaryWithObjectsAndKeys: with three pairs and with one, and every element
# must come back as the very pointer passed. append sends appendFormat: to a
# mutable string three times, once with ten doubles. cfformat calls
# CFStringCreateWithFormat, which no cf_* fixture could, with a %@ among seven
# integer-class and ten double arguments, and CFStringAppendFormat with a %@ of
# its own.
#
# native_printf is plain C, two translation units linked together. The first is
# built with _FORTIFY_SOURCE=0, because the SDK turns sprintf into __sprintf_chk
# at any optimization level otherwise, and calls printf, fprintf to stdout and
# to stderr, snprintf into a buffer and into one too small, sprintf, asprintf,
# and dprintf to both descriptors, with %d %u %ld %lld %zu %x %c %s %p %f %e %g
# and * widths and precisions, and with more than six integer-class and more
# than eight double arguments in the calls that allow it. The second is built at
# -O2 with _FORTIFY_SOURCE=2 and makes the same kind of calls into buffers whose
# size the compiler knows, so it calls __sprintf_chk and __snprintf_chk. Both
# are built with -fno-builtin like the cf_* fixtures, which matters more than
# usual here: without it clang turns a bounded __snprintf_chk back into
# snprintf. Every return value goes on a status line, every buffer is written to
# stdout, where the comparisons see it, and all but the two _chk ones are also
# compared with the text they must hold; a %p is checked only for being 0x
# followed by something other than 0, since the address changes from run to run,
# while a null %p must read exactly 0x0. stdout is flushed before the dprintf to
# it, so its buffered lines come first in every mode. The two lines the fixture
# writes to stderr begin "native_printf stderr ", and the case compares those
# lines across runs the way it compares stdout, leaving out ocerz's own lines
# and the progress notes.
#
# native_nslog calls NSLog four times from one function: the x=%d y=%@ call, one
# with eight integer-class and ten double arguments, one with a non-ASCII %@ and
# a %C, and one with no arguments at all. Its %s arguments are ASCII, because
# NSLog reads a C string in the system encoding, which is a setting of the user
# rather than of either architecture. NSLog writes each message to stderr behind
# a prefix of the date, the time to the millisecond, the process name, the pid
# and the thread id, and none of that can agree between two runs, let alone the
# arm64 build, native mode and cache mode, which do not even share a process
# name. So the case normalizes before it compares: a stderr line counts as a
# message only if it begins with that prefix, the prefix is removed, and the
# text left over must be identical, line for line, in every run. That also
# leaves out ocerz's own lines and the progress notes, which carry no such
# prefix, and it is why every message is a single line: a continuation line has
# no prefix to find. The function making the calls holds six 64-bit values and a
# double across them, and their checksum must equal one computed without NSLog,
# since a veneer is code that could disturb a callee-saved register as easily as
# a trap could.
#
# native_printf and native_nslog each say on a status line how many message
# lines they wrote to stderr, and the arm64 run must show exactly that many
# after normalization; any other count means the host is not putting them where
# the case looks, and every stderr comparison would then pass on nothing.
#
# These cases take both of M8's oracles. Each fixture is also built for arm64
# and run directly on the host first, where a build failing its own checks means
# the fixture expects something the frameworks do not do, and then its stdout,
# and its normalized stderr, must match native mode's under both engines and
# cache mode's, where the x86 libobjc, Foundation, CoreFoundation and libSystem
# out of the shared cache make the same calls. Two things really do differ
# between the builds, and the fixtures are written so that neither reaches the
# output. A tagged pointer carries its tag in the top bit on arm64 and in the
# bottom bit on x86_64, and both runtimes scramble the payload, so the short
# string's pointer looks different to each build, and different again to the x86
# build in native mode, which holds the arm64 runtime's pointers. The fixture
# therefore asks only whether a pointer is tagged in either convention, which a
# heap object, aligned and below the top of the address space, never is. And
# BOOL is a signed char on x86_64 and a bool on arm64, so every BOOL a message
# returns is stored in an int and compared with YES or NO rather than tested for
# being non-zero. Nothing else the fixtures print differed: the array's
# description, the formatted numbers and every NSLog message came out byte for
# byte the same from the arm64 build and from cache mode. Hashes are still
# compared only with each other and never printed, since nothing obliges two
# builds of Foundation to hash alike.
#
# Each case checks with nm that its fixture imports nothing but names the
# virtual libobjc, Foundation and CoreFoundation export and libSystem functions
# the bridge implements or veneers, so that a failure is about Objective-C and
# formatting and not about an import the fixture had no business making, and
# that it imports the names without which it would prove nothing: objc_msgSend
# and the pool push in every Objective-C fixture,
# ___CFConstantStringClassReference and the two constant number classes in
# objc_foundation, the two CoreFoundation format functions in objc_variadic,
# NSLog in native_nslog, and sprintf, snprintf, __sprintf_chk and __snprintf_chk
# in native_printf.
#
# The M10 runs, the arm64 ones included, are bounded at OBJC_TIMEOUT seconds,
# and a run that times out names the last progress note its fixture wrote. The
# database and the binary are built separately, so a run stopped at a special
# record whose handler this build of ocerz does not have says that, rather than
# only that an export was unimplemented.
#
# M11 lets a guest define Objective-C classes, categories and protocols of its
# own and hand them to the host's frameworks. Everything M10 did ran one way:
# the guest sent messages to native objects. A class of the guest's runs the
# other way as well. Its metadata lies in the guest's image, compiled for
# x86_64, and has to become a class the native runtime knows by name, with a
# native superclass and a native root metaclass; its methods are x86 code that
# native Foundation and AppKit reach through their own objc_msgSend, so every
# implementation the runtime hands out has to enter the guest with the method's
# arguments moved from where arm64 passes them to where x86-64 does, and its
# result moved back; a category has to be attached to a native class that
# already exists; a protocol the image carries has to be replaced by the one the
# runtime registers under that name; and every +load method in the image has to
# have run before main. Three fixtures cover it, compiled at test time and
# shaped like the M10 ones: one status line per group, the bits in source
# order, then "<name> ok" or "<name> bad:<hex>" with a bit per failed group, and
# progress notes on stderr.
#
# Instance variables are where a class of the guest's is most easily wrong with
# nothing crashing. The compiler lays a class's ivars out after the superclass's
# size as the SDK's headers declare it and writes each offset into a variable of
# its own, which every access in the guest's code reads; the runtime lays them
# out again after the superclass's real size and rewrites those variables.
# NSObject's size is 8 either way, so an NSObject subclass slides by nothing.
# NSView's header declares no ivars, so the compiler starts a subclass's at
# offset 8, and the native NSView is 536 bytes on macOS 26, so the runtime
# slides every one of them by 528. A class realized without rewriting the
# guest's variables leaves guest code reading and writing NSView's own ivars
# while native code, key-value coding among it, uses the slid offsets. Both
# kinds of class are covered, and in both the fixture compares what guest code
# reads with what valueForKey: reads, since key-value coding finds an ivar that
# has no accessor through the native runtime and reads it at the native offset.
#
# objc_classes defines, against Foundation alone, a protocol OcerzNamed, a class
# OcerzShape : NSObject adopting it and NSCopying, a subclass OcerzSquare, and a
# category on NSString, and uses them in ten groups. load checks what happened
# before main: the +load methods of both classes and of the category ran once
# each, in the order the runtime promises, the superclass's before the
# subclass's although the subclass comes first in the image's list, and every
# class's before any category's; and neither class's +initialize ran until the
# first message to it, after which each ran exactly once, handed its own class.
# object makes instances through a class method factory and through the
# designated initializer, and reads the int, the double, the strong NSString and
# the weak id back through the properties and directly from the ivars, which
# must agree. The string's property is atomic, so its accessors are
# objc_getProperty and objc_setProperty_atomic, native functions handed the
# ivar's offset. The class must be the one NSClassFromString names, and
# valueForKey: and setValue:forKey: must reach the guest's own accessors through
# native Foundation, boxing and unboxing an int and a double on the way.
# describe formats an instance with %@, which must call the guest's -description
# exactly once, and takes -debugDescription and the description of an NSArray
# holding two instances, which call it from native code; the strings are written
# after the status line. equality gives the class -isEqual: and -hash and needs
# NSSet to collapse two distinct equal instances into one, and
# NSMutableDictionary to keep them as one key, which it copies through the
# guest's -copyWithZone:. -hash folds the sides in above bit 32, and the line
# prints a checksum of the order in which an NSMutableSet of twelve instances
# enumerates them. That order follows the buckets the hashes select, as the key
# order in cf_callbacks does, so a hash cut to 32 bits on its way back to native
# code moves it: an arm64 build whose -hash makes that cut itself prints
# bbfb12a4 where the fixture prints b206cbcc. sort sorts six instances with
# sortedArrayUsingSelector:@selector(compare:), where every result of the
# guest's -compare: is a 64-bit NSComparisonResult that must reach native code
# as -1 rather than as 4294967295. subclass checks that OcerzSquare's
# initializer reaches OcerzShape's through super, that the superclass's ivars
# keep their values when the subclass writes its own and the other way round,
# and that key-value coding reads and writes the subclass's accessor-less ivars
# exactly where guest code does; its -description and -isEqual: override
# OcerzShape's and call super. category sends the category's methods to an
# @"..." literal, a string created at run time, a tagged one, a mutable one and
# the class itself, one of them returning 64 bits, and asks respondsToSelector:
# and instancesRespondToSelector: about them. protocol asks conformsToProtocol:
# of the class, an instance and the subclass about OcerzNamed and about
# NSCopying, NSObject and NSCoding, of which the image carries copies of its
# own, and requires NSProtocolFromString to return the very object
# @protocol(...) refers to for OcerzNamed and for NSCopying. lifetime lets the
# last strong reference to an instance go inside an @autoreleasepool, after
# which a weak reference to it must read nil, the guest's -dealloc must have run
# once with the name still set, and a weak reference to that name must read nil
# too, which only the ARC-generated .cxx_destruct releasing the ivar brings
# about; a weak property must read nil once the object it held is gone, an
# OcerzSquare must run its own -dealloc and then OcerzShape's, and an instance
# held only by an NSMutableArray must live exactly until removeAllObjects.
# perform sends performSelector:, performSelector:withObject: and
# performSelector:withObject:withObject: to instances and to the class, so
# native NSObject makes the call into guest methods on the guest's behalf.
#
# objc_view_render defines OcerzTestView : NSView against AppKit, overriding
# -drawRect: and -isFlipped, and renders it with no window and no NSApplication:
# bitmapImageRepForCachingDisplayInRect: gives a bitmap for the view's bounds and
# cacheDisplayInRect:toBitmapImageRep: draws the view into it, and then into a
# second, 32 by 24 pixel device RGB bitmap the fixture creates. -drawRect: calls
# [super drawRect:], turns antialiasing off, fills the bounds and an integral
# rect with NSRectFill in solid device colors, fills another integral rect
# through CoreGraphics on the context's CGContext, and strokes an NSBezierPath
# along half-pixel coordinates, so every pixel is exactly one of four colors.
# view checks the class, the result of sending -isFlipped from guest code, and
# the frame and bounds, which x86_64 gets back through objc_msgSend_stret. render
# checks that AppKit called -drawRect: once per bitmap, on the view, with the
# bounds as its dirty rect by value and a flipped current context holding a
# CGContext, and that it called the -isFlipped override itself, and the line
# prints the number of each and the backing scale. pixels probes the device
# bitmap where every shape must be and where each would be if the view were not
# flipped, checks that the cached bitmap is red where the red rect is, and
# prints a checksum of each bitmap's bytes.
#
# The checksums are compared, so the bitmaps must come out the same on every
# run. The device bitmap has no color management between the colors and the
# bytes and no antialiased edge, and its probes ask for exact bytes. The cached
# bitmap is Generic RGB at the main screen's backing scale, so its bytes pass
# through a color conversion and its size depends on the display: 64 by 48 on a
# Retina Mac, and 32 by 24 when a sandbox profile denies the process the window
# server, where AppKit still draws. The scale is the same for every process in
# one run of this gate, and the arm64 build and cache mode, with x86 AppKit and
# CoreGraphics beneath it, wrote the same bytes into both bitmaps and counted the
# same ten -isFlipped calls. The case still runs the arm64 build VIEW_RUNS times
# and requires the same output every time before it compares anything, since a
# bitmap that varied by itself would fail every comparison for a reason that has
# nothing to do with native mode.
#
# objc_view_ivar is about ivars written in -initWithFrame: and read in
# -drawRect:. OcerzIvarView : NSView declares an int, a double, a strong NSColor,
# an NSRect and a trailing unsigned char, and -initWithFrame: sets all five
# after [super initWithFrame:]; main then overwrites them in a second view.
# layout checks that the first ivar lies at or past the native NSView's instance
# size as class_getInstanceSize reports it, that the class's own size covers the
# last, that the frame NSView keeps in ivars of its own survived the writes, and
# that valueForKey: and setValue:forKey: see each view's values where guest code
# put them. draw renders each view into a device RGB bitmap of its own and
# checks that -drawRect: ran on the right view and saw every value that view was
# given, and pixels finds each view's color filling its own box and nowhere
# else. Both view fixtures draw with antialiasing off in device colors and are
# held to VIEW_RUNS identical arm64 runs in the same way.
#
# A host where AppKit cannot draw offscreen at all is not a failure of native
# mode. The view fixtures say so with a line "<name> unavailable <why>" in place
# of their remaining status lines, and exit 2, when
# bitmapImageRepForCachingDisplayInRect: returns nil, when no device RGB bitmap
# can be made, or when cacheDisplayInRect:toBitmapImageRep: never calls
# -drawRect:. That line from the arm64 build, or an arm64 build that fails
# naming the window server on stderr, skips the case with the reason. The same
# line from native mode, where the arm64 build drew, fails like any other
# missing status line.
#
# Nothing these fixtures print differed between the arm64 build and cache mode,
# and they are written so that nothing should. A description carries the
# class's name and the instance's fields and no address; every hash is computed
# by the fixture from its own fields and never printed, so no build of
# Foundation hashes anything the output depends on; the NSSet order and the
# -isFlipped count, which depend on the framework rather than on the fixture,
# came out the same from both builds; and every BOOL is compared with YES or NO,
# as in M10.
#
# The M11 cases check their imports the way the M10 ones do, against the M10
# names and those the M11 fixtures add, AppKit's and the two CoreGraphics
# functions among them for the view fixtures, and require the names without
# which they would prove nothing: objc_msgSendSuper2, NSObject's metaclass and
# _objc_empty_cache, which every class the guest defines binds, objc_storeWeak
# and objc_loadWeakRetained, and objc_getProperty and objc_setProperty_atomic in
# objc_classes; NSView's class and metaclass, NSRectFill, CGContextFillRect and
# objc_msgSend_stret in objc_view_render; and NSView's metaclass and
# class_getInstanceSize in objc_view_ivar. objc_classes also imports
# _Unwind_Resume and __objc_personality_v0, because clang gives a function
# holding a __weak local a cleanup to run if an exception unwinds through it.
# The virtual libraries export both and nothing calls either, since the fixture
# throws nothing. The M11 runs, the arm64 ones included, are bounded at
# OBJC_TIMEOUT seconds, like the M10 ones.
#
# native_classic_bind pins the one import every older Intel binary makes. A
# program linked for a macOS before 12 uses classic lazy binding, whose
# __stub_helper entries jump to dyld_stub_binder, so it imports that symbol from
# libSystem even though the loader binds every lazy pointer before the guest
# runs and no helper is ever reached. Without an export of that name, spelled
# without the leading underscore every C symbol carries, such a program was
# refused with 71 before its first instruction. The case links a small program
# for 10.14, checks with otool and nm that it really has classic binds and
# really imports dyld_stub_binder, so a toolchain that stops producing either is
# reported as that, and then requires it to run and agree with cache mode.
#
# native_constructors pins the initializers a guest image carries. Native mode
# runs no libSystem initializer, and the initializer phase cache mode gates on it
# used to be the only thing that ran a guest's own constructors, so for a while a
# C constructor or a C++ static object was silently skipped. The fixture is a
# C++ program linked against a guest dylib: the dylib's constructor must run
# before the program's, the program's constructors must all run before main, and
# main must see what each wrote. The order among the main image's own
# constructors is the linker's, so the case accepts any of the orders it can
# choose and relies on cache mode to agree on the exact one.
#
# native_exit pins the three ways a process ends. Returning from main and
# calling exit must run the guest's atexit handlers, most recent first, after
# flushing nothing early, so stdout reads "buffered second first"; _exit must run
# no handler and flush nothing, so a line the guest buffered and never flushed
# is lost, exactly as it is on a real system. In native mode a guest's atexit and
# stdio are the host's, so this is the case that fails if main's return takes
# the raw exit syscall, if exit lets the VM wind down before the handlers run,
# since a callback into an exited VM is refused, or if _exit ends through
# ocerz's own exit and so flushes host stdio. Each way is compared with cache
# mode, status and output.
#
# app_bundle is the first case that runs an application rather than a program.
# Native Foundation decides which bundle is the main one, what the process is
# called and what its arguments were from the process itself, and the process is
# ocerz, so until the guest's identity reached the host's frameworks every
# NSBundle answer came from ocerz's directory and NSApplicationMain could not
# cross at all. The fixture is a real .app bundle built in the test's temporary
# directory: an Info.plist naming the bundle identifier, NSPrincipalClass
# NSApplication, LSUIElement, so no Dock icon appears, and a key of the test's
# own, a text file under Resources, and one executable holding an x86_64 and an
# arm64 slice, so the arm64 oracle, native mode and cache mode all run the same
# path inside the same bundle and print the same thing. The bundle is built under
# the temporary directory's real path, so that no run depends on how it resolves
# the symbolic links /var and /tmp are, since ocerz hands the guest the real path
# of its executable. There is no nib, since
# the Command Line Tools have no ibtool: main makes the shared application, gives
# it a delegate of the guest's own class and calls NSApplicationMain, which
# reads the Info.plist, starts the application and delivers
# applicationWillFinishLaunching: and applicationDidFinishLaunching: to the
# guest. The checks run there, in four groups. bundle asks NSBundle and
# CFBundle for the main bundle's path, identifier, Info.plist key, principal
# class and resource, whose text is printed. crt calls _NSGetExecutablePath with
# a buffer that fits, which must leave the size alone, and one that does not,
# which must return -1 and say how much it needs; requires _NSGetArgc and
# _NSGetArgv to answer the very argument vector main was handed, _NSGetEnviron
# the very environ variable, with a setenv visible through both, and
# _NSGetProgname the pointer getprogname returns; and requires
# _NSGetMachExecuteHeader to be the guest's own _mh_execute_header. process asks
# NSProcessInfo for the process name and the arguments, getprogname for the
# program name, NSRunningApplication for the bundle identifier LaunchServices
# registered, requires CFProcessPath to be absent from the guest's environment,
# and logs one NSLog line. app requires NSApp to be the application main made,
# with the guest's delegate, running, with the accessory activation policy that
# LSUIElement asks for, and the two launch notifications to have arrived once
# each in order. The delegate then writes the summary, buffers one more line
# with printf and sends -terminate:, after which applicationWillTerminate: and
# an atexit handler write a line each and exit must flush the buffered one, with
# status 0. NSLog's prefix carries the process name, which is the one thing the
# NSLog comparison of the M10 cases throws away, so this case reads it back and
# requires the application's name in every run. A host without a window server
# session has no application to run: the fixture asks CGSessionCopyCurrentDictionary
# first and writes "app_bundle unavailable" with status 2 when there is none,
# and that line from the arm64 build, or an arm64 build that fails naming the
# window server, skips the case. The native runs are bounded at OBJC_TIMEOUT
# seconds, and a run that stops names the last progress note, which says
# whether NSApplicationMain ever reached the delegate.
#
# The block_* cases are the proof that a block crosses in both directions.
# Each fixture is built for x86_64 and arm64 and runs under the arm64 oracle,
# native mode under the JIT and the interpreter, and cache mode, and all four
# must print the same lines, exactly as the M10 cases do. block_dispatch drives
# libdispatch's block entry points from plain C: dispatch_once with a global
# block run once across three calls and a capturing stack block, dispatch_sync
# to a global queue and a serial one with a dispatch_sync nested inside a block
# native code is running and a barrier, eight dispatch_async blocks counted home
# through a semaphore, ten dispatch_group_async blocks and a group wait and
# notify, a dispatch_after that must not have run when the call returns and must
# have run by the time its semaphore is signalled, a __block variable mutated by
# blocks native code runs on its own threads and read back through its
# forwarding pointer by the frame that owns it, and dispatch_apply over a
# global queue and DISPATCH_APPLY_AUTO, last, because async work submitted after
# a dispatch_apply never runs under cache mode's host workqueue bridge, which is
# the oracle. block_runtime calls the runtime directly: Block_copy of a stack
# block and of a heap one, which must hand back the same pointer, a block
# capturing another block, which runs _Block_object_assign from the guest's copy
# helper, two heap blocks sharing one __block variable the frame keeps writing
# to, a __block variable holding a block, whose keep helper the runtime runs,
# qsort_b and bsearch_b with block comparators, and dispatch_block_create, whose
# result is a native block the guest calls directly, hands back to
# dispatch_async and waits on, with a cancelled one that must not run.
# block_foundation is the Objective-C half: enumerateObjectsUsingBlock: with
# *stop, enumerateKeysAndObjectsUsingBlock:, indexesOfObjectsPassingTest:, which
# returns a BOOL from the block, a concurrent enumeration on libdispatch's
# workers, sortedArrayUsingComparator: both ways and sortUsingComparator:, an
# NSNotificationCenter observer block that holds a guest object alive until the
# observer is removed, a block capturing a guest object handed to dispatch_sync,
# an NSMutableArray and dispatch_group_async, after which the object must be
# gone, a guest class conforming to NSItemProviderWriting whose method native
# NSItemProvider calls with a completion block of its own, which the guest calls
# with the data, NSBlockOperation and NSOperationQueue with a completion
# block read back through the property's getter and called by the guest, and an
# in-process NSXPCConnection to an anonymous listener whose exported object is a
# guest class: NSXPCInterface has to find the extended method types of the
# guest's own protocol, the proxy's method, whose encoding says @ for the reply
# block, has to cross it as a block anyway, and the guest's exported method is
# handed NSXPC's own reply block and calls it. Like
# the other cases these skip without an x86_64 clang and fail when a fixture
# does not compile, and a stop names the group its last progress note names.
#
# Native mode loads code at run time the way dyld does, and dl_basic pins it
# against the host's own dyld. The fixture is an Objective-C program that
# registers an add-image callback before it loads anything, then dlopens a
# guest dylib it was never linked against. That dylib has a class with a +load
# method, a category on NSString, a C constructor that calls getpid so clang
# cannot fold it into static data, an exported function, an exported int and a
# thread-local variable, and it is reached by absolute path, @executable_path,
# @rpath and @loader_path, all of which must answer the same handle. The
# program then dlopens a bundle linked with -bundle_loader against the program
# itself, whose class subclasses a class the program defines and calls super
# through it, which is the plug-in shape: its import of the host class is bound
# to the main executable. Around those it checks dlsym on the handle, on
# RTLD_DEFAULT, RTLD_NEXT, RTLD_SELF and RTLD_MAIN_ONLY and on an RTLD_FIRST
# handle, dlsym(RTLD_DEFAULT, "strlen") called through the pointer it answers,
# RTLD_NOLOAD, RTLD_LOCAL and a later RTLD_GLOBAL, dlclose, dlopen_preflight,
# dladdr on the dylib's function and one byte into it, dlerror's text, its
# once-only answer and its being per thread, the image list, the callback's
# count and the images it saw, and the program's SDK and platform. Each group
# prints one status line with a bitmask, the way the M10 cases do, and the same
# source is compiled for arm64 and run directly on the host, with its dylib,
# bundle and second dylib built for arm64 beside it, so the host's dyld is the
# oracle: native mode's stdout under the JIT and under -no-jit must equal it
# byte for byte. Cache mode is compared only on the groups it answers the same
# way, images, load, sym, path, strlen, dladdr, plugin and version. Its dlopen
# expands no @executable_path, @loader_path or @rpath; its dlsym on a handle
# searches that image alone, finds none of the program's symbols through the
# handle dlopen(NULL) answers, finds the program's own through RTLD_NEXT from the
# program, and answers through a handle nothing issued, which dlclose accepts;
# its dlerror answers the same message twice and records none for a missed
# dlsym; it ignores RTLD_LOCAL; it calls no add-image callback; and a guest
# dylib it dlopens gets its classes but never its +load methods, which Rosetta
# runs. So the order, dlerror, rpath, handles, local and callbacks groups would
# fail there for reasons that belong to cache mode.
#
# dl_refusals is native mode's own: every dlopen it makes must fail, and fail
# with a message that says why, the image count unchanged afterwards and a
# second dlerror answering nothing. An arm64-only dylib on disk, a library the
# host's shared cache has but no API database describes, reached by path and by
# the bare name libz.dylib, a guest dylib whose dependency has been deleted,
# and one whose dependency no longer exports a symbol it imports, twice, with
# the same message both times, which is what a load that was not rolled back
# would change. After them a plain guest dylib still loads, libc.dylib answers
# the synthesized libSystem, whose strlen is the program's own, and
# CoreFoundation, which the program does not link, loads through its
# framework's symlink as a synthesized image under its Versions/A install name.
# The failure messages are printed and each is checked for its reason.
#
# The callback, attach, thread, tlv_*, signal_*, cf_*, M10, M11, app_bundle,
# block_*, dl_* and sys_* cases skip where there is no x86_64 clang, like the
# others, but a fixture of theirs that fails to compile where a trivial x86_64
# program compiles fine is a failure: skipping it would hide a broken fixture
# indefinitely. So is a cf_*, M10, M11, app_bundle, block_*, dl_basic or sys_*
# fixture whose arm64 build fails to compile where its x86_64 build did, since
# that leaves the case without its host oracle.
#
# The sys_* cases pin the libSystem calls native mode answers with ocerz's own
# implementations rather than a crossing (src/sysbridge.c). sys_files calls the
# variadic file and IPC functions whose optional argument is fixed -- open,
# openat and their $NOCANCEL forms with and without O_CREAT, fcntl across int
# and pointer commands including the record and open-file-description locks,
# ioctl on a pipe and on a pseudo-terminal, sem_open, shm_open, semctl and
# ulimit. sys_mmap maps anonymous and file-backed memory, private and shared,
# writes x86 code into a mapping, makes it executable and calls it, rewrites it
# and calls it again, which is only right if the JIT dropped what it translated
# the first time, catches a fault on a page protected to nothing with a handler
# that leaves by siglongjmp, and drives the Mach vm calls, including a
# protection change on a posix_memalign page and a vm_deallocate of the thread
# list task_threads returns, both memory ocerz never mapped. sys_jmp covers
# setjmp and longjmp in every spelling: the value, the mask each form does and
# does not restore, jumps out of signal handlers, out of a handler running on an
# alternate stack twice, which is only right if the jump took the thread off the
# alternate stack again, and a jump inside a qsort comparator. sys_proc forks,
# vforks, execs in all seven spellings, spawns with file actions, attributes,
# a PATH search and a #! script whose interpreter is the fixture, and runs
# commands through system and popen for reading, writing and both; the children
# are the fixture itself, and each reports what it was handed.
#
# Each is compiled for x86_64 and for arm64, and the arm64 build run directly
# against the host is the oracle: the native run under the JIT and under -no-jit
# must print what it prints, apart from sys_jmp's x86 line, which checks the
# MXCSR, the x87 control word and the direction flag that only an x86 longjmp
# restores, and which cache mode, whose longjmp is Apple's x86 routine
# translated, answers for instead. Cache mode must print the same as native
# mode. nm must show that each fixture imports the functions it exists to
# exercise, and nothing the libSystem database still stubs. sys_proc's children
# also write the mode they came up in and the executable the kernel says they
# are into a side log during the native run, and every one must say native and
# ocerz: a child that came up in cache mode, or ran under Rosetta, would print
# the same report, so the report alone could not tell.
#
# sys_jmp_refused and sys_fork_callback are the two refusals. The first longjmps
# from a qsort comparator to a setjmp taken before the qsort, which would leave
# qsort's native frames behind; both engines must refuse it by name with 72,
# and cache mode, where qsort is translated x86 code, performs it. The second
# forks inside the comparator. Under the JIT a translated block's frame lies
# beneath the comparator on the host stack, and the child would return into it,
# so the fork is refused by name with 72; the interpreter leaves no such frame,
# so there the fork is performed and must print what cache mode and the arm64
# build print.
#
# The cases that need a mappable shared cache are skipped, not failed, where
# there is none. The native cases still run there -- not needing a cache is the
# entire point of the mode -- but with nothing to compare against, the
# comparison halves of the native cases skip too, and what is left is the
# fixture's own self-check.

set -u
cd "$(dirname "$0")/.."
OCERZ=./ocerz
TMP="${TMPDIR:-/tmp}/ocerz_native.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

DYN=tests/guest/benchbin/xbench_dyn
STATIC=tests/guest/bin/exit42
KERNEL=depchain
KERNELS="depchain memcpy"
SCALE=1000
LIB=/usr/lib/libSystem.B.dylib
UNIMPL_SYM=_scanf
BRIDGE_RE='^ocerz: bridge: [^ ]+ [^ ]+ not implemented$'
NOBIND='ocerz: native: no bridge for '
M0_SUMMARY='unresolved imports, which no virtual library exports'
PROBE_VAR=OCERZ_BRIDGE_PROBE
PROBE_VAL=bridge-probe-42
PROBE_BIN=""
UNIMPL_BIN=""
BADPTR_BIN=""
AFTER_BIN=""
BAD_GUEST_ADDR=0x6000000000
BADPTR_SYM=_strlen
BADPTR_SIG='L(p)'
BADPTR_MARK='badptr enter'
BADPTR_PAST='badptr returned'
AFTER_MARK='after bridged'
AFTER_PAST='after returned'
BRIDGE_FAULT_RE='^ocerz: BRIDGE-FAULT\[[0-9]+\] (SIGSEGV|SIGBUS) inside a bridged call'
BRIDGE_FAULT_STATUS=139
GUEST_FAULT_STATUS=139
GUEST_CRASH='ocerz: guest crash['
WILD_RE='ocerz: (WILD-FAULT-AV|WILD-WORKER-TERMINATE|gs0x320 WORKER-TERMINATE)'
FAULTLOG_MARK="FAULT-MAP addr=$BAD_GUEST_ADDR"
SIGTRACE_MARK="deliver addr=$BAD_GUEST_ADDR"
BRIDGELOG_RE='^ocerz: BRIDGELOG\[[0-9]+\] [^ ]+ [^ ]+ [^ ]+$'
BRIDGELOG_KERNEL=memcpy
BRIDGELOG_SYM=_memcpy
BRIDGELOG_SIG='p(ppL)'
X86_CLANG=0
CB_QSORT_BIN=""
CB_NESTED_BIN=""
CB_BSEARCH_BIN=""
CB_RECURSION_BIN=""
CB_FAULT_BIN=""
CB_FAULT_MARK='cbfault enter'
CB_FAULT_PAST='cbfault returned'
AT_ASYNC_BIN=""
AT_APPLY_BIN=""
AT_NESTED_BIN=""
AT_SYNC_BIN=""
AT_FAULT_BIN=""
AT_FAULT_MARK='attfault enter'
AT_FAULT_WORKER='attfault worker'
AT_FAULT_SURVIVED='attfault survived'
AT_FAULT_PAST='attfault returned'
ATTACH_TIMEOUT=30
ATTACH_REFUSED='ocerz: abi: native code called guest function'
TH_CREATE_BIN=""
TH_MUTEX_BIN=""
TH_COND_BIN=""
TH_WORK_BIN=""
TH_FAULT_BIN=""
TH_FAULT_MARK='thrfault enter'
TH_FAULT_THREAD='thrfault thread'
TH_FAULT_SURVIVED='thrfault survived'
TH_FAULT_PAST='thrfault returned'
TH_FAULT_NOCREATE='thrfault create failed'
THREAD_TIMEOUT=30
STACK_GUARD_SYM=___stack_chk_guard
TLV_MAIN_BIN=""
TLV_BSS_BIN=""
TLV_LAYOUT_BIN=""
TLV_THREADS_BIN=""
TLV_CHURN_BIN=""
TLV_DYLIB_BIN=""
TLV_DYLIB_LIB=""
TLV_DEP_NAME=libtlvdep.dylib
TLV_TIMEOUT=30
TLV_CHURN_THREADS=320
TLV_CHURN_BASE=16
TLV_CHURN_BLOCK=2097152
TLV_CHURN_BUDGET=15
TLV_BOOTSTRAP_SYM=__tlv_bootstrap
TLV_BRIDGED='___stack_chk_fail _write _pthread_create _pthread_join _pthread_mutex_lock _pthread_mutex_unlock _pthread_cond_wait _pthread_cond_broadcast'
TLV_UNRESOLVED='ocerz: bridge: thread-local variable descriptor '
SIG_ACTION_BIN=""
SIG_SIGNAL_BIN=""
SIG_MASK_BIN=""
SIG_ALTSTACK_BIN=""
SIG_PTKILL_BIN=""
SIG_KILL_BIN=""
SIG_ERRORS_BIN=""
SIG_DEFAULT_BIN=""
SIG_HANDLER_BIN=""
SIGNAL_TIMEOUT=30
SIGNAL_WAIT_SECS=10
SIGNAL_BRIDGED='___stack_chk_fail ___error _write _strlen _strcmp _memcpy _malloc _free _getpid _time _pthread_create _pthread_join _pthread_self _signal _sigaction _raise _kill _sigprocmask _pthread_sigmask _sigaltstack _pthread_kill _sigemptyset _sigfillset _sigaddset _sigdelset _sigismember'
SIG_DEFAULT_WINCH='signal_default raising SIGWINCH'
SIG_DEFAULT_MARK='signal_default raising SIGTERM'
SIG_DEFAULT_PAST='signal_default survived'
SIG_DEFAULT_STATUS=143
CF_BASIC_BIN=""
CF_BASIC_ARM64=""
CF_CALLBACKS_BIN=""
CF_CALLBACKS_ARM64=""
CF_RUNLOOP_BIN=""
CF_RUNLOOP_ARM64=""
CF_TIMEOUT=30
CF_RUN_SECS=5
CF_FRAMEWORK=/System/Library/Frameworks/CoreFoundation.framework
CF_CLASS_SYM=___CFConstantStringClassReference
CF_STRUCT_RE='^ocerz: bridge: [^ ]+ (was handed a .* of version [0-9]+, which ocerz cannot convert|could not bind guest function )'
CF_BRIDGED='___stack_chk_fail ___error _write _puts _putchar _strlen _strcmp _memcmp _memcpy _memset _malloc _free _getpid _time'
CF_EXPORTS='_CFRetain _CFRelease _CFGetRetainCount _CFEqual _CFHash _CFGetTypeID _CFCopyDescription _CFGetAllocator'
CF_EXPORTS="$CF_EXPORTS _CFStringGetTypeID _CFArrayGetTypeID _CFDictionaryGetTypeID _CFNumberGetTypeID _CFBooleanGetTypeID _CFDataGetTypeID"
CF_EXPORTS="$CF_EXPORTS _CFStringCreateWithCString _CFStringCreateWithBytes _CFStringCreateCopy _CFStringCreateMutable _CFStringCreateMutableCopy"
CF_EXPORTS="$CF_EXPORTS _CFStringAppendCString _CFStringAppend _CFStringGetLength _CFStringGetCharacterAtIndex _CFStringGetCString _CFStringGetCStringPtr"
CF_EXPORTS="$CF_EXPORTS _CFStringGetMaximumSizeForEncoding _CFStringCompare _CFStringHasPrefix _CFStringHasSuffix _CFStringGetIntValue _CFStringGetDoubleValue"
CF_EXPORTS="$CF_EXPORTS _CFStringCreateArrayBySeparatingStrings _CFStringCreateByCombiningStrings ___CFStringMakeConstantString"
CF_EXPORTS="$CF_EXPORTS _CFArrayCreate _CFArrayCreateMutable _CFArrayCreateCopy _CFArrayCreateMutableCopy _CFArrayGetCount _CFArrayGetValueAtIndex"
CF_EXPORTS="$CF_EXPORTS _CFArrayAppendValue _CFArrayInsertValueAtIndex _CFArraySetValueAtIndex _CFArrayRemoveValueAtIndex _CFArrayRemoveAllValues"
CF_EXPORTS="$CF_EXPORTS _CFDictionaryCreate _CFDictionaryCreateMutable _CFDictionaryCreateCopy _CFDictionaryCreateMutableCopy _CFDictionaryGetCount"
CF_EXPORTS="$CF_EXPORTS _CFDictionaryGetValue _CFDictionaryGetValueIfPresent _CFDictionaryContainsKey _CFDictionaryAddValue _CFDictionarySetValue"
CF_EXPORTS="$CF_EXPORTS _CFDictionaryRemoveValue _CFDictionaryGetKeysAndValues _CFDictionaryApplyFunction"
CF_EXPORTS="$CF_EXPORTS _CFNumberCreate _CFNumberGetValue _CFNumberGetType _CFNumberCompare _CFBooleanGetValue _CFDataCreate _CFDataGetLength _CFDataGetBytePtr"
CF_EXPORTS="$CF_EXPORTS _CFAbsoluteTimeGetCurrent _CFRunLoopGetCurrent _CFRunLoopGetMain _CFRunLoopRun _CFRunLoopRunInMode _CFRunLoopStop _CFRunLoopWakeUp"
CF_EXPORTS="$CF_EXPORTS _CFRunLoopAddTimer _CFRunLoopRemoveTimer _CFRunLoopTimerCreate _CFRunLoopTimerInvalidate _CFRunLoopTimerIsValid"
CF_EXPORTS="$CF_EXPORTS _CFRunLoopTimerGetNextFireDate _CFRunLoopTimerSetNextFireDate _CFRunLoopObserverCreate _CFRunLoopAddObserver"
CF_EXPORTS="$CF_EXPORTS _CFRunLoopRemoveObserver _CFRunLoopObserverInvalidate _CFRunLoopSourceCreate _CFRunLoopAddSource _CFRunLoopRemoveSource"
CF_EXPORTS="$CF_EXPORTS _CFRunLoopSourceSignal _CFRunLoopSourceInvalidate"
CF_EXPORTS="$CF_EXPORTS ___CFConstantStringClassReference _kCFAllocatorDefault _kCFAllocatorSystemDefault _kCFAllocatorMalloc _kCFAllocatorNull"
CF_EXPORTS="$CF_EXPORTS _kCFTypeArrayCallBacks _kCFTypeDictionaryKeyCallBacks _kCFTypeDictionaryValueCallBacks _kCFCopyStringDictionaryKeyCallBacks"
CF_EXPORTS="$CF_EXPORTS _kCFBooleanTrue _kCFBooleanFalse _kCFNull _kCFRunLoopDefaultMode _kCFRunLoopCommonModes"
CF_EXPORTS="$CF_EXPORTS _kCFNumberPositiveInfinity _kCFNumberNegativeInfinity _kCFNumberNaN"
OBJC_FOUNDATION_BIN=""
OBJC_FOUNDATION_ARM64=""
OBJC_VARIADIC_BIN=""
OBJC_VARIADIC_ARM64=""
NATIVE_PRINTF_BIN=""
NATIVE_PRINTF_ARM64=""
NATIVE_NSLOG_BIN=""
NATIVE_NSLOG_ARM64=""
OBJC_TIMEOUT=30
OBJC_LIB=/usr/lib/libobjc.A.dylib
FOUNDATION_FRAMEWORK=/System/Library/Frameworks/Foundation.framework
OBJC_HANDLER_RE='^ocerz: bridge: [^ ]+ asks for the handler [^ ]+, which ocerz does not have$'
NSLOG_PREFIX_RE='^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} [^[]+\[[0-9]+:[0-9a-fx]+\] '
OBJC_BRIDGED='_printf _fprintf _sprintf _snprintf _asprintf _dprintf ___sprintf_chk ___snprintf_chk _fflush ___stdoutp ___stderrp'
OBJC_EXPORTS='_objc_msgSend _objc_alloc _objc_alloc_init _objc_opt_new _objc_opt_class _objc_opt_self _objc_opt_isKindOfClass _objc_opt_respondsToSelector'
OBJC_EXPORTS="$OBJC_EXPORTS _objc_retain _objc_release _objc_autorelease _objc_retainAutorelease _objc_storeStrong"
OBJC_EXPORTS="$OBJC_EXPORTS _objc_retainAutoreleasedReturnValue _objc_claimAutoreleasedReturnValue _objc_autoreleaseReturnValue _objc_retainAutoreleaseReturnValue"
OBJC_EXPORTS="$OBJC_EXPORTS _objc_autoreleasePoolPush _objc_autoreleasePoolPop"
OBJC_EXPORTS="$OBJC_EXPORTS _NSLog _NSClassFromString _NSStringFromRange _NSRangeFromString _NSIntersectionRange _NSUnionRange"
OBJC_EXPORTS="$OBJC_EXPORTS _OBJC_CLASS_\$_NSString _OBJC_CLASS_\$_NSMutableString _OBJC_CLASS_\$_NSNumber _OBJC_CLASS_\$_NSValue"
OBJC_EXPORTS="$OBJC_EXPORTS _OBJC_CLASS_\$_NSConstantIntegerNumber _OBJC_CLASS_\$_NSConstantDoubleNumber _OBJC_CLASS_\$_NSArray _OBJC_CLASS_\$_NSDictionary"
OBJC_EXPORTS="$OBJC_EXPORTS _CFStringCreateWithFormat _CFStringAppendFormat"
OBJC_CLASSES_BIN=""
OBJC_CLASSES_ARM64=""
OBJC_VIEW_RENDER_BIN=""
OBJC_VIEW_RENDER_ARM64=""
OBJC_VIEW_IVAR_BIN=""
OBJC_VIEW_IVAR_ARM64=""
VIEW_RUNS=3
APPKIT_FRAMEWORK=/System/Library/Frameworks/AppKit.framework
CG_FRAMEWORK=/System/Library/Frameworks/CoreGraphics.framework
VIEW_NO_SERVER_RE='WindowServer|window server|CGSConnection|CGS_REQUIRE_INIT'
OBJC_CLASS_EXPORTS='_objc_msgSendSuper2 _objc_msgSend_stret __objc_empty_cache _objc_enumerationMutation _class_getInstanceSize'
OBJC_CLASS_EXPORTS="$OBJC_CLASS_EXPORTS _OBJC_CLASS_\$_NSObject _OBJC_METACLASS_\$_NSObject"
OBJC_CLASS_EXPORTS="$OBJC_CLASS_EXPORTS _objc_storeWeak _objc_initWeak _objc_loadWeakRetained _objc_destroyWeak _objc_getProperty _objc_setProperty_atomic"
OBJC_CLASS_EXPORTS="$OBJC_CLASS_EXPORTS __Unwind_Resume ___objc_personality_v0"
OBJC_CLASS_EXPORTS="$OBJC_CLASS_EXPORTS _NSStringFromClass _NSStringFromProtocol _NSProtocolFromString _NSSelectorFromString _NSStringFromRect"
OBJC_CLASS_EXPORTS="$OBJC_CLASS_EXPORTS _OBJC_CLASS_\$_NSSet _OBJC_CLASS_\$_NSMutableSet _OBJC_CLASS_\$_NSMutableArray _OBJC_CLASS_\$_NSMutableDictionary"
APPKIT_EXPORTS='_NSRectFill _NSDeviceRGBColorSpace _CGContextSetRGBFillColor _CGContextFillRect'
APPKIT_EXPORTS="$APPKIT_EXPORTS _OBJC_CLASS_\$_NSView _OBJC_METACLASS_\$_NSView _OBJC_CLASS_\$_NSColor _OBJC_CLASS_\$_NSBezierPath"
APPKIT_EXPORTS="$APPKIT_EXPORTS _OBJC_CLASS_\$_NSBitmapImageRep _OBJC_CLASS_\$_NSGraphicsContext"
APP_BUNDLE_BIN=""
APP_BUNDLE_ARM64=""
APP_BUNDLE_EXE=""
APP_NAME=OcerzApp
APP_ID=org.aarchx.ocerz.appbundle
APP_ARG=extra-argument
APP_KEY=OcerzPlanted
APP_VALUE=from-info-plist
APP_TEXT=read-through-nsbundle
APP_EXPORTS='_NSApplicationMain _NSApp _CGSessionCopyCurrentDictionary _CFBundleGetMainBundle _CFBundleGetIdentifier _CFBundleGetValueForInfoDictionaryKey'
APP_EXPORTS="$APP_EXPORTS _OBJC_CLASS_\$_NSApplication _OBJC_CLASS_\$_NSBundle _OBJC_CLASS_\$_NSProcessInfo _OBJC_CLASS_\$_NSRunningApplication"
APP_EXPORTS="$APP_EXPORTS __NSGetArgc __NSGetArgv __NSGetEnviron __NSGetExecutablePath __NSGetMachExecuteHeader __NSGetProgname"
APP_EXPORTS="$APP_EXPORTS _getprogname _getenv _setenv _atexit _environ _strrchr _objc_unsafeClaimAutoreleasedReturnValue"
DL_BASIC_BIN=""
DL_BASIC_ARM64=""
DL_REFUSALS_BIN=""
DL_TIMEOUT=60
DL_EXPORTS='_dlopen _dlsym _dladdr _dlclose _dlerror _dlopen_preflight __dyld_image_count __dyld_get_image_header __dyld_get_image_name'
DL_EXPORTS="$DL_EXPORTS __dyld_get_image_vmaddr_slide __dyld_get_image_header_containing_address __dyld_get_prog_image_header __dyld_register_func_for_add_image"
DL_EXPORTS="$DL_EXPORTS __dyld_is_memory_immutable __dyld_shared_cache_contains_path _dyld_image_path_containing_address _dyld_get_active_platform"
DL_EXPORTS="$DL_EXPORTS _dyld_get_program_min_os_version _dyld_get_program_sdk_version _dyld_program_sdk_at_least"
DL_BRIDGED='___stack_chk_fail ___snprintf_chk ____chkstk_darwin _free _malloc _pthread_create _pthread_join _strcmp _strlen _strrchr _strstr _write dyld_stub_binder'
DL_NEED='_dlopen _dlsym _dladdr _dlclose _dlerror __dyld_register_func_for_add_image _objc_msgSend'
DL_CACHE_GROUPS='images|load|sym|path|strlen|dladdr|plugin|version'
DL_REFUSED='native library without an API database'
MEASURE_BIN=/usr/bin/time
SYS_FILES_BIN=""
SYS_FILES_ARM64=""
SYS_MMAP_BIN=""
SYS_MMAP_ARM64=""
SYS_JMP_BIN=""
SYS_JMP_ARM64=""
SYS_PROC_BIN=""
SYS_PROC_ARM64=""
SYS_REFUSE_BIN=""
SYS_FORKCB_BIN=""
SYS_FORKCB_ARM64=""
SYS_TIMEOUT=120
SYS_WORK="$TMP/sysw"
SYS_FILES_NEED='_open _open$NOCANCEL _openat _openat$NOCANCEL _fcntl _fcntl$NOCANCEL _ioctl _sem_open _shm_open _semctl _ulimit'
SYS_MMAP_NEED='_mmap _munmap _mprotect _madvise _mach_vm_allocate _mach_vm_deallocate _mach_vm_protect _vm_allocate _vm_deallocate _vm_protect _sigsetjmp _siglongjmp'
SYS_JMP_NEED='_setjmp __setjmp _sigsetjmp _longjmp __longjmp _siglongjmp'
SYS_PROC_NEED='_fork _vfork _execv _execve _execvp _execvP _execl _execle _execlp _posix_spawn _posix_spawnp _system _popen _pclose'
SYS_PROC_KINDS='spawn spawn-attr spawnp sys_proc_script execv execve execvp execvP execl execle execlp many system popen'
SYS_REFUSE_MSG='ocerz: bridge: /usr/lib/libSystem.B.dylib _longjmp from inside a callback _qsort made, to a setjmp taken outside that call, would skip the native frames of _qsort; refused'
SYS_FORKCB_MSG='ocerz: bridge: /usr/lib/libSystem.B.dylib _fork was called inside a callback _qsort made from translated code, and the child would return into a translation it does not inherit; refused'
SYS_REFUSED_STATUS=72

unset OCERZ_MODE
unset OCERZ_BRIDGE_PROBE_UNSET
unset OCERZ_BRIDGELOG
unset CFProcessPath

if [ ! -x "$OCERZ" ]; then
    echo "error: ocerz binary not found or not executable at $OCERZ" >&2
    echo "build it first with: make ocerz" >&2
    exit 2
fi
if [ ! -x "$DYN" ] || [ ! -x "$STATIC" ]; then
    echo "run_native_tests: SKIP (guest fixtures missing; run: make -C tests/guest)"
    exit 0
fi

pass=0
fail=0

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi
NATIVE_TIMEOUT=60

run_bounded() {
    local out_file="$1" err_file="$2"
    shift 2
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" "${NATIVE_TIMEOUT}s" "$@" >"$out_file" 2>"$err_file" </dev/null
        return $?
    fi
    "$@" >"$out_file" 2>"$err_file" </dev/null &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$NATIVE_TIMEOUT" ]; then
            kill -TERM "$pid" 2>/dev/null
            sleep 1
            kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid"
    return $?
}

record() {
    local name="$1" reason="$2" detail="${3:-}"
    if [ -z "$reason" ]; then
        echo "PASS $name${detail:+ ($detail)}"; pass=$((pass+1))
    else
        echo "FAIL $name ($reason)"; fail=$((fail+1))
    fi
}

cache_line_seen() {
    grep -q 'shared cache mapped' "$@"
}

bridge_sym() {
    grep -hE "$BRIDGE_RE" "$@" 2>/dev/null | head -1 | awk '{print $4}'
}

# empty on success, otherwise why this native run did not reach a clean exit 0
native_run_reason() {
    local rc="$1"
    shift
    if grep -Fq "$NOBIND" "$@" 2>/dev/null; then
        echo "an import went unresolved: $(grep -hF "$NOBIND" "$@" | head -1)"
    elif grep -qE "$BRIDGE_RE" "$@" 2>/dev/null; then
        echo "stopped at an unbridged export: $(grep -hE "$BRIDGE_RE" "$@" | head -1)"
    elif cache_line_seen "$@"; then
        echo "native mode mapped the shared cache"
    elif [ "$rc" -ne 0 ]; then
        echo "exit $rc, want 0"
    else
        echo ""
    fi
}

CACHE_OK=1
run_bounded "$TMP/probe.out" "$TMP/probe.err" "$OCERZ" "$DYN" "$KERNEL" 1
case $? in
    65|70) CACHE_OK=0 ;;
esac

NOUT="$TMP/native.$KERNEL.jit.out"
NERR="$TMP/native.$KERNEL.jit.err"
CACHE_OUT="$TMP/cache.$KERNEL.jit.out"

build_fixtures() {
    local src="$TMP/bridge_probe.c" bin="$TMP/bridge_probe"
    local usrc="$TMP/unimpl.c" ubin="$TMP/unimpl"
    local bsrc="$TMP/badptr.c" bbin="$TMP/badptr"
    local asrc="$TMP/after.c" abin="$TMP/after"

    cat > "$src" <<'EOC'
typedef __SIZE_TYPE__ bp_size;

void *malloc(bp_size);
void *calloc(bp_size, bp_size);
void *realloc(void *, bp_size);
void free(void *);
bp_size strlen(const char *);
bp_size strnlen(const char *, bp_size);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, bp_size);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, bp_size);
char *strcat(char *, const char *);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strstr(const char *, const char *);
char *strdup(const char *);
void *memcpy(void *, const void *, bp_size);
void *memmove(void *, const void *, bp_size);
void *memset(void *, int, bp_size);
int memcmp(const void *, const void *, bp_size);
void *memchr(const void *, int, bp_size);
char *getenv(const char *);
long write(int, const void *, bp_size);
long read(int, void *, bp_size);
int close(int);
int puts(const char *);
int putchar(int);
int *__error(void);
int abs(int);
long labs(long);
int atoi(const char *);
long atol(const char *);
int isatty(int);
int getpid(void);
long time(long *);
unsigned long clock(void);

#define CK(cond) do { if (!(cond)) m |= bit; bit <<= 1; } while (0)

static int g_io_ok = 1;
static int g_bad;

static int sgn(int v)
{
    return v < 0 ? -1 : (v > 0 ? 1 : 0);
}

static void emit(const char *s, bp_size n)
{
    if (write(1, s, n) != (long)n)
        g_io_ok = 0;
}

static bp_size put_str(char *p, const char *s)
{
    bp_size i = 0;
    while (s[i]) {
        p[i] = s[i];
        i++;
    }
    return i;
}

static bp_size put_hex(char *p, unsigned v)
{
    const char *d = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++)
        p[i] = d[(v >> (28 - 4 * i)) & 15];
    return 8;
}

static void line_status(const char *name, unsigned mask)
{
    char b[64];
    bp_size n = put_str(b, name);

    b[n++] = ' ';
    if (mask == 0) {
        n += put_str(b + n, "ok");
    } else {
        n += put_str(b + n, "bad:");
        n += put_hex(b + n, mask);
        g_bad = 1;
    }
    b[n++] = '\n';
    emit(b, n);
}

static const char s_a[] = "ocerz bridge probe";
static const char s_b[] = "ocerz bridge probes";

static unsigned group_str(void)
{
    unsigned m = 0, bit = 1;
    char buf[64], cp[16];
    char *d;

    CK(strlen(s_a) == 18);
    CK(strnlen(s_a, 5) == 5);
    CK(strnlen(s_a, 64) == 18);
    CK(sgn(strcmp(s_a, s_a)) == 0);
    CK(sgn(strcmp(s_a, s_b)) == -1);
    CK(sgn(strcmp(s_b, s_a)) == 1);
    CK(sgn(strncmp(s_a, s_b, 18)) == 0);
    CK(sgn(strncmp(s_a, s_b, 19)) == -1);
    CK(strcpy(buf, "ocerz") == buf);
    CK(strlen(buf) == 5);
    CK(strcat(buf, "-probe") == buf);
    CK(sgn(strcmp(buf, "ocerz-probe")) == 0);
    CK(strlen(buf) == 11);
    CK(strchr(buf, '-') == buf + 5);
    CK(strchr(buf, 'z') == buf + 4);
    CK(strchr(buf, 'q') == 0);
    CK(strrchr(buf, 'e') == buf + 10);
    CK(strstr(buf, "probe") == buf + 6);
    CK(strstr(buf, "zzz") == 0);
    memset(cp, 'x', sizeof cp);
    CK(strncpy(cp, s_a, 5) == cp);
    cp[5] = 0;
    CK(sgn(strcmp(cp, "ocerz")) == 0);
    d = strdup(s_a);
    CK(d != 0 && d != s_a && sgn(strcmp(d, s_a)) == 0);
    free(d);
    return m;
}

static unsigned char m_buf[512];

static unsigned group_mem(void)
{
    unsigned m = 0, bit = 1;
    unsigned char ref[64];
    int i;

    CK(memset(m_buf, 0xa5, sizeof m_buf) == m_buf);
    CK(m_buf[0] == 0xa5 && m_buf[511] == 0xa5);
    CK(memset(m_buf, 0, 64) == m_buf);
    for (i = 0; i < 64; i++)
        ref[i] = 0;
    CK(sgn(memcmp(m_buf, ref, 64)) == 0);
    CK(m_buf[64] == 0xa5);

    for (i = 0; i < 64; i++) {
        m_buf[i] = (unsigned char)(i * 7 + 3);
        ref[i] = (unsigned char)(i * 7 + 3);
    }
    CK(memcpy(m_buf + 256, m_buf, 64) == m_buf + 256);
    CK(sgn(memcmp(m_buf + 256, ref, 64)) == 0);
    ref[10] = (unsigned char)(ref[10] ^ 0xff);
    CK(sgn(memcmp(m_buf + 256, ref, 64)) != 0);
    ref[10] = (unsigned char)(ref[10] ^ 0xff);

    CK(memmove(m_buf + 8, m_buf, 64) == m_buf + 8);
    CK(sgn(memcmp(m_buf + 8, ref, 64)) == 0);
    CK(memmove(m_buf, m_buf + 8, 64) == m_buf);
    CK(sgn(memcmp(m_buf, ref, 64)) == 0);

    CK(memchr(m_buf, ref[20], 64) == m_buf + 20);
    CK(memchr(m_buf, 0, 64) == 0);
    return m;
}

static unsigned group_heap(void)
{
    unsigned m = 0, bit = 1;
    const bp_size big = 4u << 20;
    unsigned char *p, *q, *z;
    int i, ok;

    p = malloc(64);
    ok = p != 0;
    if (ok) {
        for (i = 0; i < 64; i++)
            p[i] = (unsigned char)(i ^ 0x5a);
        for (i = 0; i < 64; i++)
            if (p[i] != (unsigned char)(i ^ 0x5a))
                ok = 0;
    }
    CK(ok);
    free(p);

    q = malloc(big);
    ok = q != 0;
    if (ok) {
        q[0] = 0x11;
        q[big / 2] = 0x22;
        q[big - 1] = 0x33;
        memset(q + 4096, 0x7e, 4096);
        if (q[0] != 0x11 || q[big / 2] != 0x22 || q[big - 1] != 0x33)
            ok = 0;
        if (q[4096] != 0x7e || q[8191] != 0x7e)
            ok = 0;
    }
    CK(ok);
    free(q);

    z = calloc(128, 8);
    ok = z != 0;
    if (ok)
        for (i = 0; i < 1024; i++)
            if (z[i] != 0)
                ok = 0;
    CK(ok);
    free(z);

    p = malloc(32);
    ok = p != 0;
    if (ok) {
        for (i = 0; i < 32; i++)
            p[i] = (unsigned char)(i + 1);
        p = realloc(p, 8192);
        ok = p != 0;
        if (ok)
            for (i = 0; i < 32; i++)
                if (p[i] != (unsigned char)(i + 1))
                    ok = 0;
    }
    CK(ok);
    free(p);
    return m;
}

static unsigned group_env(void)
{
    unsigned m = 0, bit = 1;
    const char *v = getenv("OCERZ_BRIDGE_PROBE");

    CK(v != 0);
    CK(v != 0 && strlen(v) == 15);
    CK(v != 0 && sgn(strcmp(v, "bridge-probe-42")) == 0);
    CK(getenv("OCERZ_BRIDGE_PROBE_UNSET") == 0);
    return m;
}

static unsigned group_misc(void)
{
    unsigned m = 0, bit = 1;
    char rb[8];
    int *e;

    CK(abs(-7) == 7);
    CK(labs(-1234567890L) == 1234567890L);
    CK(atoi("-42") == -42);
    CK(atoi("  17xyz") == 17);
    CK(atol("1234567890") == 1234567890L);
    CK(getpid() > 0);
    CK(time(0) > 1600000000L);
    CK(clock() != (unsigned long)-1);
    CK(isatty(-1) == 0);
    CK(read(0, rb, sizeof rb) == 0);
    CK(close(-1) == -1);
    e = __error();
    CK(e != 0 && *e == 9);
    return m;
}

int main(void)
{
    line_status("str", group_str());
    line_status("mem", group_mem());
    line_status("heap", group_heap());
    line_status("env", group_env());
    line_status("misc", group_misc());
    putchar('i');
    putchar('o');
    puts(g_io_ok ? " ok" : " bad");
    return (g_bad || !g_io_ok) ? 1 : 0;
}
EOC

    cat > "$usrc" <<'EOC'
int scanf(const char *, ...);
int main(void)
{
    int v = 0;
    return scanf("%d", &v) == 1 ? v : 0;
}
EOC

    cat > "$bsrc" <<EOC
typedef __SIZE_TYPE__ bp_size;

bp_size strlen(const char *);
long write(int, const void *, bp_size);

static const char *volatile bp_bad = (const char *)${BAD_GUEST_ADDR}ull;
static volatile bp_size bp_len;

int main(void)
{
    write(1, "badptr enter\n", 13);
    bp_len = strlen(bp_bad);
    write(1, "badptr returned\n", 16);
    return bp_len != 0;
}
EOC

    cat > "$asrc" <<EOC
typedef __SIZE_TYPE__ bp_size;

bp_size strlen(const char *);
long write(int, const void *, bp_size);

static const char *volatile ap_good = "after";
static const unsigned char *volatile ap_bad = (const unsigned char *)${BAD_GUEST_ADDR}ull;
static volatile bp_size ap_len;
static volatile unsigned char ap_got;

int main(void)
{
    write(1, "after enter\n", 12);
    ap_len = strlen(ap_good);
    if (ap_len == 5)
        write(1, "after bridged\n", 14);
    ap_got = *ap_bad;
    write(1, "after returned\n", 15);
    return ap_got != 0;
}
EOC

    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
            -o "$bin" "$src" >/dev/null 2>&1; then
        PROBE_BIN="$bin"
        printf 'str ok\nmem ok\nheap ok\nenv ok\nmisc ok\nio ok\n' > "$TMP/probe.want"
    fi
    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
            -o "$ubin" "$usrc" >/dev/null 2>&1; then
        UNIMPL_BIN="$ubin"
    fi
    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
            -o "$bbin" "$bsrc" >/dev/null 2>&1; then
        BADPTR_BIN="$bbin"
    fi
    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
            -o "$abin" "$asrc" >/dev/null 2>&1; then
        AFTER_BIN="$abin"
    fi
}

build_callback_fixtures() {
    local name

    printf 'int main(void) { return 0; }\n' > "$TMP/cb_trivial.c"
    if clang -arch x86_64 -o "$TMP/cb_trivial" "$TMP/cb_trivial.c" >/dev/null 2>&1; then
        X86_CLANG=1
    fi

    cat > "$TMP/cb_common.h" <<'EOC'
typedef __SIZE_TYPE__ cb_size;
typedef __UINTPTR_TYPE__ cb_uptr;

long write(int, const void *, cb_size);
int puts(const char *);
int strcmp(const char *, const char *);
void qsort(void *, cb_size, cb_size, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, cb_size, cb_size,
              int (*)(const void *, const void *));

#define CK(cond) do { if (!(cond)) m |= bit; bit <<= 1; } while (0)
#define CB_SKEWED() (((cb_uptr)__builtin_frame_address(0) & 15) != 0)

static char cb_buf[160];
static cb_size cb_len;

static void cb_str(const char *s)
{
    while (*s && cb_len < sizeof cb_buf - 1)
        cb_buf[cb_len++] = *s++;
}

static void cb_hex(unsigned v)
{
    const char *d = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) {
        char c[2] = { d[(v >> (28 - 4 * i)) & 15], 0 };
        cb_str(c);
    }
}

static void cb_dec(unsigned v)
{
    char t[11];
    int n = 10;
    t[10] = 0;
    do {
        t[--n] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    cb_str(t + n);
}

static void cb_begin(const char *name, unsigned mask)
{
    cb_len = 0;
    cb_str(name);
    if (mask == 0) {
        cb_str(" ok");
    } else {
        cb_str(" bad:");
        cb_hex(mask);
    }
}

static void cb_field(const char *key, unsigned v, int hex)
{
    cb_str(" ");
    cb_str(key);
    cb_str("=");
    if (hex)
        cb_hex(v);
    else
        cb_dec(v);
}

static void cb_end(void)
{
    cb_buf[cb_len++] = '\n';
    write(1, cb_buf, cb_len);
}
EOC

    cat > "$TMP/callback_qsort.c" <<'EOC'
#include "cb_common.h"

#define N 300

static int g_v[N];
static unsigned g_up, g_down, g_skew;

static int cmp_up(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    g_up++;
    if (CB_SKEWED())
        g_skew++;
    return x < y ? -65536 : (x > y ? 65536 : 0);
}

static int cmp_down(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    g_down++;
    if (CB_SKEWED())
        g_skew++;
    return x > y ? -65536 : (x < y ? 65536 : 0);
}

static int ascending(void)
{
    int i;
    for (i = 0; i < N; i++)
        if (g_v[i] != i - N / 2)
            return 0;
    return 1;
}

int main(void)
{
    unsigned m = 0, bit = 1, sum = 0;
    int i, ok;

    for (i = 0; i < N; i++)
        g_v[i] = (int)((unsigned)i * 7919u % N) - N / 2;

    qsort(g_v, N, sizeof g_v[0], cmp_up);
    CK(ascending());
    qsort(g_v, N, sizeof g_v[0], cmp_down);
    ok = 1;
    for (i = 0; i < N; i++)
        if (g_v[i] != N / 2 - 1 - i)
            ok = 0;
    CK(ok);
    qsort(g_v, N, sizeof g_v[0], cmp_up);
    CK(ascending());
    CK(g_up != 0 && g_down != 0);
    CK(g_skew == 0);

    for (i = 0; i < N; i++)
        sum = sum * 31u + (unsigned)g_v[i];
    cb_begin("qsort", m);
    cb_field("n", N, 0);
    cb_field("sum", sum, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/callback_nested_bridge.c" <<'EOC'
#include "cb_common.h"

static const char *const g_words[] = {
    "qsort", "strcmp", "bridge", "Bridge", "callback", "call", "", "zebra",
    "alpha", "alphabet", "alp", "~tilde", "0zero", "ocerz", "call", "guest",
    "x86", "arm64", "AArchX", "trampoline", "slot", "bank", "\xc3\xa9t\xc3\xa9",
    "comparator", "nested", "crossing", "frame", "Zulu", "zulu", "b", "a", "aa",
};
#define NW ((int)(sizeof g_words / sizeof g_words[0]))

static const char *g_sorted[NW];
static unsigned g_calls;

static int cmp_str(const void *a, const void *b)
{
    g_calls++;
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int own_cmp(const char *x, const char *y)
{
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return (int)(unsigned char)*x - (int)(unsigned char)*y;
}

int main(void)
{
    unsigned m = 0, bit = 1;
    char seen[NW], line[512];
    cb_size n = 0;
    int i, j, ok;

    for (i = 0; i < NW; i++) {
        g_sorted[i] = g_words[i];
        seen[i] = 0;
    }
    qsort(g_sorted, NW, sizeof g_sorted[0], cmp_str);

    ok = 1;
    for (i = 1; i < NW; i++)
        if (own_cmp(g_sorted[i - 1], g_sorted[i]) > 0)
            ok = 0;
    CK(ok);
    ok = 1;
    for (i = 0; i < NW; i++) {
        for (j = 0; j < NW; j++)
            if (!seen[j] && g_words[j] == g_sorted[i])
                break;
        if (j == NW)
            ok = 0;
        else
            seen[j] = 1;
    }
    CK(ok);
    CK(g_calls != 0);

    cb_begin("nested", m);
    cb_field("words", (unsigned)NW, 0);
    cb_end();
    for (i = 0; i < NW; i++) {
        if (i)
            line[n++] = ',';
        for (j = 0; g_sorted[i][j]; j++)
            line[n++] = g_sorted[i][j];
    }
    line[n] = 0;
    puts(line);
    return m != 0;
}
EOC

    cat > "$TMP/callback_bsearch.c" <<'EOC'
#include "cb_common.h"

#define N 200

struct rec {
    int key;
    int val;
};

struct probe {
    char tag[4];
    int key;
};

static struct rec g_recs[N];
static struct probe g_probe;
static unsigned g_calls, g_bad_key, g_bad_elem, g_skew;

static int cmp_key(const void *k, const void *e)
{
    const struct probe *p = k;
    const struct rec *r = e;
    cb_uptr lo = (cb_uptr)g_recs, off = (cb_uptr)e - lo;

    g_calls++;
    if (p != &g_probe)
        g_bad_key++;
    if ((cb_uptr)e < lo || off >= sizeof g_recs || off % sizeof g_recs[0] != 0)
        g_bad_elem++;
    if (CB_SKEWED())
        g_skew++;
    return p->key < r->key ? -65536 : (p->key > r->key ? 65536 : 0);
}

static const struct rec *find(int key, cb_size n)
{
    g_probe.key = key;
    return bsearch(&g_probe, g_recs, n, sizeof g_recs[0], cmp_key);
}

int main(void)
{
    static const int kOutside[] = { -1000000, -251, 348, 1000000 };
    unsigned m = 0, bit = 1, found = 0, absent = 0;
    int i, ok;

    g_probe.tag[0] = 'k';
    g_probe.tag[1] = 'e';
    g_probe.tag[2] = 'y';
    for (i = 0; i < N; i++) {
        g_recs[i].key = 3 * i - 250;
        g_recs[i].val = i * 7 + 1;
    }

    ok = 1;
    for (i = 0; i < N; i++) {
        const struct rec *r = find(3 * i - 250, N);
        if (r == &g_recs[i] && r->val == i * 7 + 1)
            found++;
        else
            ok = 0;
    }
    CK(ok);

    ok = 1;
    for (i = 0; i < N; i++) {
        if (find(3 * i - 249, N) == 0)
            absent++;
        else
            ok = 0;
    }
    for (i = 0; i < 4; i++) {
        if (find(kOutside[i], N) == 0)
            absent++;
        else
            ok = 0;
    }
    CK(ok);
    CK(find(-250, 0) == 0);
    CK(g_calls != 0);
    CK(g_bad_key == 0);
    CK(g_bad_elem == 0);
    CK(g_skew == 0);

    cb_begin("bsearch", m);
    cb_field("found", found, 0);
    cb_field("absent", absent, 0);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/callback_recursion.c" <<'EOC'
#include "cb_common.h"

#define DEPTH 24

static int g_level;
static unsigned g_enter[DEPTH + 1], g_leave[DEPTH + 1];
static unsigned g_deepest, g_wrong_cmp, g_moved, g_unsorted, g_skew;
static unsigned g_trail = 17;

static int cmp_up(const void *a, const void *b);
static int cmp_down(const void *a, const void *b);

static void descend(int d)
{
    int v[3];
    int up = d & 1;

    v[0] = 3 * d + 2;
    v[1] = 3 * d;
    v[2] = 3 * d + 1;
    g_level = d;
    qsort(v, 3, sizeof v[0], up ? cmp_up : cmp_down);
    if (up ? !(v[0] < v[1] && v[1] < v[2]) : !(v[0] > v[1] && v[1] > v[2]))
        g_unsorted++;
    g_leave[d]++;
    g_trail = g_trail * 37u + (unsigned)d;
}

static int step(const void *a, const void *b, int up)
{
    int d = g_level;
    int x = *(const int *)a, y = *(const int *)b;

    if ((d & 1) != up)
        g_wrong_cmp++;
    if (CB_SKEWED())
        g_skew++;
    if (d >= 1 && d <= DEPTH && g_enter[d] == 0) {
        g_enter[d]++;
        g_trail = g_trail * 31u + (unsigned)d;
        if ((unsigned)d > g_deepest)
            g_deepest = (unsigned)d;
        if (d < DEPTH) {
            descend(d + 1);
            g_level = d;
            if (*(const int *)a != x || *(const int *)b != y)
                g_moved++;
        }
    }
    if (up)
        return x < y ? -65536 : (x > y ? 65536 : 0);
    return x > y ? -65536 : (x < y ? 65536 : 0);
}

static int cmp_up(const void *a, const void *b)
{
    return step(a, b, 1);
}

static int cmp_down(const void *a, const void *b)
{
    return step(a, b, 0);
}

int main(void)
{
    unsigned m = 0, bit = 1, want = 17;
    int d, ok;

    descend(1);

    ok = 1;
    for (d = 1; d <= DEPTH; d++)
        if (g_enter[d] != 1)
            ok = 0;
    CK(ok);
    ok = 1;
    for (d = 1; d <= DEPTH; d++)
        if (g_leave[d] != 1)
            ok = 0;
    CK(ok);
    CK(g_deepest == DEPTH);
    CK(g_wrong_cmp == 0);
    CK(g_moved == 0);
    CK(g_unsorted == 0);
    CK(g_skew == 0);
    for (d = 1; d <= DEPTH; d++)
        want = want * 31u + (unsigned)d;
    for (d = DEPTH; d >= 1; d--)
        want = want * 37u + (unsigned)d;
    CK(g_trail == want);

    cb_begin("recursion", m);
    cb_field("depth", g_deepest, 0);
    cb_field("trail", g_trail, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/callback_guest_fault.c" <<EOC
#include "cb_common.h"

static const int *volatile g_bad = (const int *)${BAD_GUEST_ADDR}ull;
static volatile int g_sink;
static int g_v[8] = { 5, 3, 7, 1, 8, 2, 6, 4 };

static int cmp_fault(const void *a, const void *b)
{
    g_sink = *g_bad;
    return *(const int *)a - *(const int *)b;
}

int main(void)
{
    write(1, "cbfault enter\n", 14);
    qsort(g_v, 8, sizeof g_v[0], cmp_fault);
    write(1, "cbfault returned\n", 17);
    return 0;
}
EOC

    for name in callback_qsort callback_nested_bridge callback_bsearch \
                callback_recursion callback_guest_fault; do
        clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
                -o "$TMP/$name" "$TMP/$name.c" >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            callback_qsort) CB_QSORT_BIN="$TMP/$name" ;;
            callback_nested_bridge) CB_NESTED_BIN="$TMP/$name" ;;
            callback_bsearch) CB_BSEARCH_BIN="$TMP/$name" ;;
            callback_recursion) CB_RECURSION_BIN="$TMP/$name" ;;
            callback_guest_fault) CB_FAULT_BIN="$TMP/$name" ;;
        esac
    done
}

build_attach_fixtures() {
    local name

    cat > "$TMP/attach_common.h" <<'EOC'
#include "cb_common.h"

typedef void *at_queue;
typedef void *at_sema;

int *__error(void);
cb_size strlen(const char *);
at_queue dispatch_get_global_queue(long, unsigned long);
void dispatch_async_f(at_queue, void *, void (*)(void *));
void dispatch_sync_f(at_queue, void *, void (*)(void *));
void dispatch_apply_f(cb_size, at_queue, void *, void (*)(void *, cb_size));
at_sema dispatch_semaphore_create(long);
long dispatch_semaphore_wait(at_sema, unsigned long long);
long dispatch_semaphore_signal(at_sema);
void dispatch_release(void *);

#define AT_FOREVER (~0ull)

static void at_note(const char *tag, const char *what)
{
    char b[96];
    cb_size n = 0;

    while (*tag && n < 40)
        b[n++] = *tag++;
    b[n++] = ':';
    b[n++] = ' ';
    while (*what && n < sizeof b - 1)
        b[n++] = *what++;
    b[n++] = '\n';
    write(2, b, n);
}

static unsigned at_mix(unsigned v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}
EOC

    cat > "$TMP/attach_async.c" <<'EOC'
#include "attach_common.h"

#define ROUNDS 2
#define TAG "attach_async"

struct job {
    unsigned in;
    unsigned out;
    unsigned runs;
    int *err;
};

static struct job g_job[ROUNDS];
static at_sema g_done;
static unsigned g_bad_ctx, g_skew;

static void work(void *ctx)
{
    struct job *j = ctx;
    int r;

    at_note(TAG, "work entered");
    if (CB_SKEWED())
        g_skew++;
    for (r = 0; r < ROUNDS; r++)
        if (ctx == &g_job[r])
            break;
    if (r == ROUNDS) {
        g_bad_ctx++;
    } else {
        j->runs++;
        j->err = __error();
        j->out = at_mix(j->in);
    }
    at_note(TAG, "signalling");
    dispatch_semaphore_signal(g_done);
}

int main(void)
{
    unsigned m = 0, bit = 1, value = 0;
    at_queue q = dispatch_get_global_queue(0, 0);
    int *mine = __error();
    int r, ok, waited = 1, runs_ok = 1, values_ok = 1, off_thread = 1;

    g_done = dispatch_semaphore_create(0);
    CK(q != 0 && g_done != 0);
    if (q == 0 || g_done == 0) {
        cb_begin(TAG, m);
        cb_end();
        return 1;
    }
    for (r = 0; r < ROUNDS; r++) {
        g_job[r].in = 0x41545441u + (unsigned)r;
        at_note(TAG, "calling dispatch_async_f");
        dispatch_async_f(q, &g_job[r], work);
        at_note(TAG, "returned from dispatch_async_f");
        ok = dispatch_semaphore_wait(g_done, AT_FOREVER) == 0;
        at_note(TAG, "wait returned");
        if (!ok)
            waited = 0;
        if (g_job[r].runs != 1)
            runs_ok = 0;
        if (g_job[r].out != at_mix(g_job[r].in))
            values_ok = 0;
        if (g_job[r].err == 0 || g_job[r].err == mine)
            off_thread = 0;
        value = value * 31u + g_job[r].out;
    }
    CK(waited);
    CK(runs_ok);
    CK(g_bad_ctx == 0);
    CK(values_ok);
    CK(off_thread);
    CK(g_skew == 0);
    dispatch_release(g_done);

    cb_begin(TAG, m);
    cb_field("rounds", ROUNDS, 0);
    cb_field("value", value, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/attach_apply.c" <<'EOC'
#include "attach_common.h"

#define N 384
#define SPIN 1000
#define TAG "attach_apply"

static unsigned g_hits[N];
static unsigned g_val[N];
static int *g_err[N];
static unsigned g_bad_ctx, g_bad_index, g_skew;
static const char g_ctx[] = TAG;

static unsigned spin(unsigned i)
{
    unsigned acc = i, k;
    for (k = 0; k < SPIN; k++)
        acc = at_mix(acc + k);
    return acc;
}

static void iteration(void *ctx, cb_size i)
{
    if (ctx != g_ctx)
        g_bad_ctx++;
    if (CB_SKEWED())
        g_skew++;
    if (i >= N) {
        g_bad_index++;
        return;
    }
    g_val[i] = spin((unsigned)i);
    g_err[i] = __error();
    g_hits[i]++;
}

int main(void)
{
    unsigned m = 0, bit = 1, sum = 0, want = 0, threads = 0, here = 0, s;
    at_queue q = dispatch_get_global_queue(0, 0);
    int *mine = __error();
    int *seen[N];
    int i, k, none = 0, twice = 0, wrong = 0;
    char note[48];
    cb_size n;

    CK(q != 0);
    if (q == 0) {
        cb_begin(TAG, m);
        cb_end();
        return 1;
    }
    at_note(TAG, "calling dispatch_apply_f");
    dispatch_apply_f(N, q, (void *)g_ctx, iteration);
    at_note(TAG, "returned from dispatch_apply_f");

    for (i = 0; i < N; i++) {
        if (g_hits[i] == 0)
            none++;
        else if (g_hits[i] > 1)
            twice++;
        s = spin((unsigned)i);
        if (g_hits[i] != 0 && g_val[i] != s)
            wrong++;
        sum = sum * 31u + g_val[i];
        want = want * 31u + s;
        if (g_err[i] == mine)
            here++;
        for (k = 0; k < (int)threads; k++)
            if (seen[k] == g_err[i])
                break;
        if (k == (int)threads && g_err[i] != 0)
            seen[threads++] = g_err[i];
    }
    CK(none == 0);
    CK(twice == 0);
    CK(wrong == 0 && sum == want);
    CK(g_bad_ctx == 0);
    CK(g_bad_index == 0);
    CK(g_skew == 0);
    CK(threads > (here ? 1u : 0u));

    cb_len = 0;
    cb_str("threads=");
    cb_dec(threads);
    cb_str(" calling=");
    cb_dec(here);
    for (n = 0; n < cb_len && n < sizeof note - 1; n++)
        note[n] = cb_buf[n];
    note[n] = 0;
    at_note(TAG, note);

    cb_begin(TAG, m);
    cb_field("n", N, 0);
    cb_field("sum", sum, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/attach_nested_bridge.c" <<'EOC'
#include "attach_common.h"

#define TAG "attach_nested"

static const char *const g_words[] = {
    "worker", "attach", "attached", "personality", "libdispatch", "guest",
    "thread", "Thread", "callback", "crossing", "", "zeta", "alpha", "alp",
    "nested", "queue", "semaphore", "strcmp", "strlen", "qsort", "x86", "arm64",
};
#define NW ((int)(sizeof g_words / sizeof g_words[0]))

static const char *g_sorted[NW];
static at_sema g_done;
static const char g_token[] = TAG;
static int *g_work_err;
static unsigned g_runs, g_bad_ctx, g_skew, g_calls, g_wrong_thread;
static unsigned g_len_sum, g_strcmp_bad;

static int cmp_str(const void *a, const void *b)
{
    g_calls++;
    if (CB_SKEWED())
        g_skew++;
    if (__error() != g_work_err)
        g_wrong_thread++;
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void work(void *ctx)
{
    int i;

    at_note(TAG, "work entered");
    g_runs++;
    if (ctx != g_token)
        g_bad_ctx++;
    if (CB_SKEWED())
        g_skew++;
    g_work_err = __error();
    for (i = 0; i < NW; i++) {
        g_len_sum += (unsigned)strlen(g_words[i]);
        g_sorted[i] = g_words[i];
    }
    if (!(strcmp("attach", "attached") < 0))
        g_strcmp_bad |= 1;
    if (!(strcmp("worker", "thread") > 0))
        g_strcmp_bad |= 2;
    if (strcmp(g_words[3], g_words[3]) != 0)
        g_strcmp_bad |= 4;
    qsort(g_sorted, NW, sizeof g_sorted[0], cmp_str);
    at_note(TAG, "signalling");
    dispatch_semaphore_signal(g_done);
}

static int own_cmp(const char *x, const char *y)
{
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return (int)(unsigned char)*x - (int)(unsigned char)*y;
}

int main(void)
{
    unsigned m = 0, bit = 1, len = 0, order = 17;
    at_queue q = dispatch_get_global_queue(0, 0);
    int *mine = __error();
    char seen[NW];
    int i, j, ok, waited;

    g_done = dispatch_semaphore_create(0);
    CK(q != 0 && g_done != 0);
    if (q == 0 || g_done == 0) {
        cb_begin(TAG, m);
        cb_end();
        return 1;
    }
    at_note(TAG, "calling dispatch_async_f");
    dispatch_async_f(q, (void *)g_token, work);
    at_note(TAG, "returned from dispatch_async_f");
    waited = dispatch_semaphore_wait(g_done, AT_FOREVER) == 0;
    at_note(TAG, "wait returned");

    for (i = 0; i < NW; i++) {
        for (j = 0; g_words[i][j]; j++)
            len++;
        seen[i] = 0;
    }
    CK(waited && g_runs == 1 && g_bad_ctx == 0);
    CK(g_work_err != 0 && g_work_err != mine);
    CK(g_len_sum == len);
    CK(g_strcmp_bad == 0);
    ok = 1;
    for (i = 1; i < NW; i++)
        if (g_sorted[i - 1] == 0 || g_sorted[i] == 0 || own_cmp(g_sorted[i - 1], g_sorted[i]) > 0)
            ok = 0;
    CK(ok);
    ok = 1;
    for (i = 0; i < NW; i++) {
        for (j = 0; j < NW; j++)
            if (!seen[j] && g_words[j] == g_sorted[i])
                break;
        if (j == NW)
            ok = 0;
        else
            seen[j] = 1;
    }
    CK(ok);
    CK(g_calls != 0);
    CK(g_wrong_thread == 0);
    CK(g_skew == 0);
    dispatch_release(g_done);

    for (i = 0; i < NW; i++)
        for (j = 0; g_sorted[i] && g_sorted[i][j]; j++)
            order = order * 31u + (unsigned char)g_sorted[i][j];

    cb_begin(TAG, m);
    cb_field("words", (unsigned)NW, 0);
    cb_field("len", len, 0);
    cb_field("order", order, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/attach_sync.c" <<'EOC'
#include "attach_common.h"

#define CALLS 3
#define NEAR 0x40000u
#define TAG "attach_sync"

static const char g_token[] = TAG;
static int *g_work_err[CALLS];
static cb_uptr g_work_frame[CALLS], g_call_frame[CALLS];
static unsigned g_runs, g_bad_ctx, g_skew, g_value;
static int g_call;

static void work(void *ctx)
{
    at_note(TAG, "work entered");
    if (ctx != g_token)
        g_bad_ctx++;
    if (CB_SKEWED())
        g_skew++;
    if (g_call >= 0 && g_call < CALLS) {
        g_work_err[g_call] = __error();
        g_work_frame[g_call] = (cb_uptr)__builtin_frame_address(0);
    }
    g_runs++;
    g_value = at_mix(g_value + (unsigned)g_call);
}

static __attribute__((noinline)) unsigned run_sync(at_queue q, int c, unsigned carry)
{
    unsigned before = at_mix(carry ^ 0x53594e43u);

    g_call = c;
    g_call_frame[c] = (cb_uptr)__builtin_frame_address(0);
    at_note(TAG, "calling dispatch_sync_f");
    dispatch_sync_f(q, (void *)g_token, work);
    at_note(TAG, "returned from dispatch_sync_f");
    return before ^ g_value;
}

int main(void)
{
    unsigned m = 0, bit = 1, carry = 7, want = 7, value = 0;
    at_queue q = dispatch_get_global_queue(0, 0);
    int *mine = __error();
    int c, same_thread = 1, near = 1, runs_ok = 1;

    CK(q != 0);
    if (q == 0) {
        cb_begin(TAG, m);
        cb_end();
        return 1;
    }
    for (c = 0; c < CALLS; c++) {
        unsigned runs = g_runs;
        carry = run_sync(q, c, carry);
        if (g_runs != runs + 1)
            runs_ok = 0;
        if (g_work_err[c] != mine)
            same_thread = 0;
        if (!(g_work_frame[c] < g_call_frame[c] && g_call_frame[c] - g_work_frame[c] < NEAR))
            near = 0;
    }
    for (c = 0; c < CALLS; c++) {
        value = at_mix(value + (unsigned)c);
        want = at_mix(want ^ 0x53594e43u) ^ value;
    }
    CK(runs_ok);
    CK(g_bad_ctx == 0);
    CK(same_thread);
    CK(near);
    CK(carry == want);
    CK(g_skew == 0);

    cb_begin(TAG, m);
    cb_field("calls", CALLS, 0);
    cb_field("value", g_value, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/attach_guest_fault.c" <<EOC
#include "attach_common.h"

static const int *volatile g_bad = (const int *)${BAD_GUEST_ADDR}ull;
static volatile int g_sink;
static at_sema g_done;

static void work(void *ctx)
{
    write(1, "attfault worker\n", 16);
    g_sink = *g_bad;
    write(1, "attfault survived\n", 18);
    dispatch_semaphore_signal(g_done);
}

int main(void)
{
    at_queue q = dispatch_get_global_queue(0, 0);

    g_done = dispatch_semaphore_create(0);
    write(1, "attfault enter\n", 15);
    dispatch_async_f(q, 0, work);
    dispatch_semaphore_wait(g_done, AT_FOREVER);
    write(1, "attfault returned\n", 18);
    return 0;
}
EOC

    for name in attach_async attach_apply attach_nested_bridge attach_sync \
                attach_guest_fault; do
        clang -arch x86_64 -std=c11 -O1 -fno-builtin -fno-stack-protector \
                -o "$TMP/$name" "$TMP/$name.c" >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            attach_async) AT_ASYNC_BIN="$TMP/$name" ;;
            attach_apply) AT_APPLY_BIN="$TMP/$name" ;;
            attach_nested_bridge) AT_NESTED_BIN="$TMP/$name" ;;
            attach_sync) AT_SYNC_BIN="$TMP/$name" ;;
            attach_guest_fault) AT_FAULT_BIN="$TMP/$name" ;;
        esac
    done
}

build_thread_fixtures() {
    local name

    cat > "$TMP/thread_common.h" <<'EOC'
#include <pthread.h>
#include "attach_common.h"

void *memcpy(void *, const void *, cb_size);
void *malloc(cb_size);
void free(void *);

#define TH_IN(p, arr) ((cb_uptr)(p) - (cb_uptr)(arr) < sizeof (arr) && ((cb_uptr)(p) - (cb_uptr)(arr)) % sizeof (arr)[0] == 0)
EOC

    cat > "$TMP/thread_create_join.c" <<'EOC'
#include "thread_common.h"

#define WAVES 2
#define PER_WAVE 8
#define TOTAL (WAVES * PER_WAVE)
#define TAG "thread_create_join"

struct slot {
    unsigned runs;
    unsigned skew;
    pthread_t self;
    pthread_t self_again;
};

static struct slot g_slot[TOTAL];
static unsigned g_bad_arg;

static cb_uptr expect(unsigned k)
{
    return ((cb_uptr)(k + 1) << 36) | at_mix(k ^ 0x54485244u);
}

static void *start(void *arg)
{
    struct slot *s = arg;
    unsigned k;

    at_note(TAG, "thread entered");
    if (!TH_IN(s, g_slot)) {
        g_bad_arg++;
        at_note(TAG, "thread leaving");
        return 0;
    }
    k = (unsigned)(s - g_slot);
    s->runs++;
    if (CB_SKEWED())
        s->skew++;
    s->self = pthread_self();
    s->self_again = pthread_self();
    at_note(TAG, "thread leaving");
    return (void *)expect(k);
}

int main(void)
{
    unsigned m = 0, bit = 1, value = 17;
    pthread_t main_self = pthread_self();
    pthread_t tid[PER_WAVE];
    int ok_tid[PER_WAVE];
    void *ret;
    int w, i, j, k;
    int created = 1, joined = 1, ran = 1, results = 1, match = 1, not_main = 1;
    int stable = 1, distinct = 1, skew = 0;

    for (w = 0; w < WAVES; w++) {
        for (i = 0; i < PER_WAVE; i++) {
            at_note(TAG, "calling pthread_create");
            ok_tid[i] = pthread_create(&tid[i], 0, start, &g_slot[w * PER_WAVE + i]) == 0;
            at_note(TAG, "returned from pthread_create");
            if (!ok_tid[i])
                created = 0;
        }
        for (i = 0; i < PER_WAVE; i++)
            for (j = 0; j < i; j++)
                if (ok_tid[i] && ok_tid[j] && tid[i] == tid[j])
                    distinct = 0;
        for (i = 0; i < PER_WAVE; i++) {
            k = w * PER_WAVE + i;
            ret = 0;
            if (ok_tid[i]) {
                at_note(TAG, "calling pthread_join");
                if (pthread_join(tid[i], &ret) != 0)
                    joined = 0;
                at_note(TAG, "returned from pthread_join");
            }
            if ((cb_uptr)ret != expect((unsigned)k))
                results = 0;
            if (g_slot[k].runs != 1)
                ran = 0;
            if (!ok_tid[i] || g_slot[k].self != tid[i])
                match = 0;
            if (g_slot[k].self == main_self)
                not_main = 0;
            if (g_slot[k].self_again != g_slot[k].self)
                stable = 0;
            if (g_slot[k].skew)
                skew = 1;
            value = value * 31u + (unsigned)((cb_uptr)ret >> 32);
            value = value * 31u + (unsigned)(cb_uptr)ret;
        }
    }
    if (pthread_self() != main_self)
        stable = 0;
    CK(created);
    CK(joined);
    CK(ran && g_bad_arg == 0);
    CK(results);
    CK(match);
    CK(not_main);
    CK(stable);
    CK(distinct);
    CK(!skew);

    cb_begin(TAG, m);
    cb_field("threads", TOTAL, 0);
    cb_field("value", value, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/thread_mutex.c" <<'EOC'
#include "thread_common.h"

#define THREADS 8
#define ROUNDS 25000
#define TAG "thread_mutex"

struct worker {
    unsigned long done;
    unsigned lock_err;
    unsigned skew;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tally_lock;
static struct worker g_w[THREADS];
static volatile unsigned long g_count;
static volatile int g_inside;
static unsigned g_overlap, g_bad_arg, g_tally;

static void *start(void *arg)
{
    struct worker *w = arg;
    unsigned long v, r;

    at_note(TAG, "thread entered");
    if (!TH_IN(w, g_w)) {
        g_bad_arg++;
        at_note(TAG, "thread leaving");
        return 0;
    }
    if (CB_SKEWED())
        w->skew++;
    for (r = 0; r < ROUNDS; r++) {
        if (pthread_mutex_lock(&g_lock) != 0) {
            w->lock_err++;
            continue;
        }
        if (g_inside)
            g_overlap++;
        g_inside = 1;
        v = g_count;
        v += 1;
        g_count = v;
        g_inside = 0;
        if (pthread_mutex_unlock(&g_lock) != 0)
            w->lock_err++;
        w->done++;
    }
    if (pthread_mutex_lock(&g_tally_lock) != 0)
        w->lock_err++;
    g_tally += (unsigned)(w - g_w) + 1u;
    if (pthread_mutex_unlock(&g_tally_lock) != 0)
        w->lock_err++;
    at_note(TAG, "thread leaving");
    return w;
}

int main(void)
{
    unsigned m = 0, bit = 1, want_tally = 0;
    pthread_t tid[THREADS];
    int ok_tid[THREADS];
    void *ret;
    int i, created = 1, joined = 1, results = 1, done = 1, lock_err = 0, skew = 0;
    int init_ok, gate_ok, destroy_ok;

    init_ok = pthread_mutex_init(&g_tally_lock, 0) == 0;
    gate_ok = pthread_mutex_lock(&g_lock) == 0;
    for (i = 0; i < THREADS; i++) {
        at_note(TAG, "calling pthread_create");
        ok_tid[i] = pthread_create(&tid[i], 0, start, &g_w[i]) == 0;
        at_note(TAG, "returned from pthread_create");
        if (!ok_tid[i])
            created = 0;
    }
    if (pthread_mutex_unlock(&g_lock) != 0)
        gate_ok = 0;
    for (i = 0; i < THREADS; i++) {
        ret = 0;
        if (ok_tid[i]) {
            at_note(TAG, "calling pthread_join");
            if (pthread_join(tid[i], &ret) != 0)
                joined = 0;
            at_note(TAG, "returned from pthread_join");
        }
        if (ret != &g_w[i])
            results = 0;
        if (g_w[i].done != ROUNDS)
            done = 0;
        if (g_w[i].lock_err)
            lock_err = 1;
        if (g_w[i].skew)
            skew = 1;
        want_tally += (unsigned)i + 1u;
    }
    destroy_ok = pthread_mutex_destroy(&g_lock) == 0 && pthread_mutex_destroy(&g_tally_lock) == 0;
    CK(created && joined && results && g_bad_arg == 0);
    CK(init_ok && gate_ok && !lock_err && destroy_ok);
    CK(done);
    CK(g_count == (unsigned long)THREADS * ROUNDS);
    CK(g_overlap == 0);
    CK(g_tally == want_tally);
    CK(!skew);

    cb_begin(TAG, m);
    cb_field("threads", THREADS, 0);
    cb_field("count", (unsigned)g_count, 0);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/thread_cond.c" <<'EOC'
#include "thread_common.h"

#define VALUES 1000
#define TAG "thread_cond"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_filled = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_asked;
static unsigned g_requested, g_finished, g_full, g_holder;
static unsigned long long g_slot;
static unsigned g_prod_err, g_cons_err, g_main_err, g_excl, g_skew;
static unsigned g_cons_waits, g_received, g_dup, g_order, g_payload, g_range;
static unsigned g_sum = 17;
static unsigned char g_seen[VALUES];
static const char g_prod_token[] = "producer";
static const char g_cons_token[] = "consumer";

static unsigned long long value_of(unsigned k)
{
    return ((unsigned long long)k << 32) | at_mix(k + 0x434f4e44u);
}

static void hold(unsigned who)
{
    if (g_holder != 0)
        g_excl++;
    g_holder = who;
}

static void *producer(void *arg)
{
    unsigned k = 0;

    at_note(TAG, "thread entered");
    if (CB_SKEWED())
        g_skew++;
    if (arg != g_prod_token)
        g_prod_err++;
    if (pthread_mutex_lock(&g_lock) != 0)
        g_prod_err++;
    hold(1);
    for (;;) {
        while (g_requested == k && !g_finished) {
            g_holder = 0;
            if (pthread_cond_wait(&g_asked, &g_lock) != 0)
                g_prod_err++;
            hold(1);
        }
        if (g_requested == k)
            break;
        g_slot = value_of(k);
        g_full = 1;
        k++;
        if (pthread_cond_signal(&g_filled) != 0)
            g_prod_err++;
    }
    g_holder = 0;
    if (pthread_mutex_unlock(&g_lock) != 0)
        g_prod_err++;
    at_note(TAG, "thread leaving");
    return arg;
}

static void *consumer(void *arg)
{
    unsigned long long v;
    unsigned k, got, next = 0;

    at_note(TAG, "thread entered");
    if (CB_SKEWED())
        g_skew++;
    if (arg != g_cons_token)
        g_cons_err++;
    for (k = 0; k < VALUES; k++) {
        if (pthread_mutex_lock(&g_lock) != 0)
            g_cons_err++;
        hold(2);
        g_requested = k + 1;
        if (pthread_cond_broadcast(&g_asked) != 0)
            g_cons_err++;
        while (!g_full) {
            g_cons_waits++;
            g_holder = 0;
            if (pthread_cond_wait(&g_filled, &g_lock) != 0)
                g_cons_err++;
            hold(2);
        }
        v = g_slot;
        g_full = 0;
        g_holder = 0;
        if (pthread_mutex_unlock(&g_lock) != 0)
            g_cons_err++;
        got = (unsigned)(v >> 32);
        if (got >= VALUES) {
            g_range++;
            continue;
        }
        if (g_seen[got]++)
            g_dup++;
        if (got != next)
            g_order++;
        next = got + 1;
        if ((unsigned)v != at_mix(got + 0x434f4e44u))
            g_payload++;
        g_received++;
        g_sum = g_sum * 31u + (unsigned)v;
    }
    if (pthread_mutex_lock(&g_lock) != 0)
        g_cons_err++;
    hold(2);
    g_finished = 1;
    if (pthread_cond_broadcast(&g_asked) != 0)
        g_cons_err++;
    g_holder = 0;
    if (pthread_mutex_unlock(&g_lock) != 0)
        g_cons_err++;
    at_note(TAG, "thread leaving");
    return arg;
}

int main(void)
{
    unsigned m = 0, bit = 1, k, missing = 0;
    pthread_t prod, cons;
    int prod_ok, cons_ok, joined = 1;
    void *ret = 0;

    if (pthread_cond_init(&g_asked, 0) != 0)
        g_main_err++;
    at_note(TAG, "calling pthread_create");
    cons_ok = pthread_create(&cons, 0, consumer, (void *)g_cons_token) == 0;
    at_note(TAG, "returned from pthread_create");
    at_note(TAG, "calling pthread_create");
    prod_ok = pthread_create(&prod, 0, producer, (void *)g_prod_token) == 0;
    at_note(TAG, "returned from pthread_create");

    if (cons_ok) {
        if (pthread_mutex_lock(&g_lock) != 0)
            g_main_err++;
        hold(3);
        at_note(TAG, "waiting");
        while (!g_finished) {
            g_holder = 0;
            if (pthread_cond_wait(&g_asked, &g_lock) != 0)
                g_main_err++;
            hold(3);
        }
        at_note(TAG, "wait returned");
        g_holder = 0;
        if (pthread_mutex_unlock(&g_lock) != 0)
            g_main_err++;
    }
    if (prod_ok) {
        at_note(TAG, "calling pthread_join");
        if (pthread_join(prod, 0) != 0)
            joined = 0;
        at_note(TAG, "returned from pthread_join");
    }
    if (cons_ok) {
        at_note(TAG, "calling pthread_join");
        if (pthread_join(cons, &ret) != 0)
            joined = 0;
        at_note(TAG, "returned from pthread_join");
    }
    for (k = 0; k < VALUES; k++)
        if (g_seen[k] == 0)
            missing++;
    if (pthread_mutex_destroy(&g_lock) != 0)
        g_main_err++;

    CK(prod_ok && cons_ok && joined && ret == g_cons_token);
    CK(g_prod_err == 0 && g_cons_err == 0 && g_main_err == 0);
    CK(g_received == VALUES && missing == 0 && g_range == 0);
    CK(g_dup == 0);
    CK(g_order == 0);
    CK(g_payload == 0);
    CK(g_excl == 0);
    CK(g_cons_waits >= VALUES);
    CK(g_skew == 0);

    cb_begin(TAG, m);
    cb_field("values", g_received, 0);
    cb_field("sum", g_sum, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/thread_bridged_work.c" <<'EOC'
#include "thread_common.h"

#define THREADS 6
#define ROUNDS 12
#define N 96
#define TAG "thread_bridged_work"

static const char *const g_words[] = {
    "pthread_create", "start routine", "trampoline", "personality", "guest",
    "native", "strlen", "memcpy", "qsort", "comparator", "", "x86_64", "arm64",
    "join", "mutex", "condition variable", "AArchX", "ocerz",
};
#define NW ((unsigned)(sizeof g_words / sizeof g_words[0]))

struct job;

struct item {
    int key;
    struct job *owner;
};

struct job {
    pthread_t self;
    unsigned runs;
    unsigned calls;
    unsigned wrong_thread;
    unsigned skew;
    unsigned copy_bad;
    unsigned heap_bad;
    unsigned sort_bad;
    unsigned sum;
};

static struct job g_job[THREADS];
static unsigned g_bad_arg, g_bad_owner;

static int cmp_key(const void *a, const void *b)
{
    const struct item *x = a, *y = b;
    struct job *j = x->owner;

    if (!TH_IN(j, g_job)) {
        g_bad_owner++;
        return 0;
    }
    j->calls++;
    if (y->owner != j || pthread_self() != j->self)
        j->wrong_thread++;
    if (CB_SKEWED())
        j->skew++;
    return x->key < y->key ? -65536 : (x->key > y->key ? 65536 : 0);
}

static unsigned own_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static unsigned expect_sum(unsigned t)
{
    unsigned h = 17, r, i, n, c;
    const char *w;

    for (r = 0; r < ROUNDS; r++) {
        for (i = 0; i < NW; i++) {
            w = g_words[(i + t + r) % NW];
            n = own_len(w);
            h = h * 31u + n;
            for (c = 0; c < n; c++)
                h = h * 31u + (unsigned char)w[c];
        }
        for (i = 0; i < N; i++)
            h = h * 31u + (unsigned)((int)i - N / 2);
    }
    return h;
}

static void *start(void *arg)
{
    struct job *j = arg;
    struct item items[N];
    char buf[48];
    char *heap;
    const char *w;
    unsigned h = 17, r, i, c, t;
    cb_size n;

    at_note(TAG, "thread entered");
    if (!TH_IN(j, g_job)) {
        g_bad_arg++;
        at_note(TAG, "thread leaving");
        return 0;
    }
    t = (unsigned)(j - g_job);
    j->runs++;
    j->self = pthread_self();
    if (CB_SKEWED())
        j->skew++;
    for (r = 0; r < ROUNDS; r++) {
        heap = malloc(64);
        if (heap == 0)
            j->heap_bad++;
        for (i = 0; i < NW; i++) {
            w = g_words[(i + t + r) % NW];
            n = strlen(w);
            if (n != own_len(w) || n >= sizeof buf)
                j->copy_bad++;
            if (n >= sizeof buf)
                continue;
            buf[n] = 'x';
            if (memcpy(buf, w, n + 1) != buf)
                j->copy_bad++;
            for (c = 0; c <= n; c++)
                if (buf[c] != w[c])
                    j->copy_bad++;
            if (heap != 0) {
                if (memcpy(heap, buf, n + 1) != heap)
                    j->heap_bad++;
                if (strlen(heap) != n)
                    j->heap_bad++;
            }
            h = h * 31u + (unsigned)n;
            for (c = 0; c < n; c++)
                h = h * 31u + (unsigned char)buf[c];
        }
        free(heap);
        for (i = 0; i < N; i++) {
            items[i].key = (int)((i * 37u + t * 11u + r * 5u) % N) - N / 2;
            items[i].owner = j;
        }
        qsort(items, N, sizeof items[0], cmp_key);
        for (i = 0; i < N; i++) {
            if (items[i].key != (int)i - N / 2 || items[i].owner != j)
                j->sort_bad++;
            h = h * 31u + (unsigned)items[i].key;
        }
    }
    j->sum = h;
    at_note(TAG, "thread leaving");
    return j;
}

int main(void)
{
    unsigned m = 0, bit = 1, total = 17;
    pthread_t tid[THREADS];
    int ok_tid[THREADS];
    void *ret;
    int i, created = 1, joined = 1, results = 1, ran = 1, sums = 1;
    int calls = 1, wrong = 0, skew = 0, copy = 0, heap = 0, sorted = 1;

    for (i = 0; i < THREADS; i++) {
        at_note(TAG, "calling pthread_create");
        ok_tid[i] = pthread_create(&tid[i], 0, start, &g_job[i]) == 0;
        at_note(TAG, "returned from pthread_create");
        if (!ok_tid[i])
            created = 0;
    }
    for (i = 0; i < THREADS; i++) {
        ret = 0;
        if (ok_tid[i]) {
            at_note(TAG, "calling pthread_join");
            if (pthread_join(tid[i], &ret) != 0)
                joined = 0;
            at_note(TAG, "returned from pthread_join");
        }
        if (ret != &g_job[i])
            results = 0;
        if (g_job[i].runs != 1)
            ran = 0;
        if (g_job[i].sum != expect_sum((unsigned)i))
            sums = 0;
        if (g_job[i].calls == 0)
            calls = 0;
        if (g_job[i].wrong_thread)
            wrong = 1;
        if (g_job[i].skew)
            skew = 1;
        if (g_job[i].copy_bad)
            copy = 1;
        if (g_job[i].heap_bad)
            heap = 1;
        if (g_job[i].sort_bad)
            sorted = 0;
        total = total * 31u + g_job[i].sum;
    }
    CK(created && joined && results && ran && g_bad_arg == 0);
    CK(!copy);
    CK(!heap);
    CK(sorted);
    CK(calls && g_bad_owner == 0);
    CK(!wrong);
    CK(sums);
    CK(!skew);

    cb_begin(TAG, m);
    cb_field("threads", THREADS, 0);
    cb_field("sum", total, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/thread_guest_fault.c" <<EOC
#include "thread_common.h"

static const int *volatile g_bad = (const int *)${BAD_GUEST_ADDR}ull;
static volatile int g_sink;
static const char g_token[] = "thrfault";

static void *start(void *arg)
{
    char mark[] = "thrfault thread\n";

    write(1, mark, sizeof mark - 1);
    g_sink = *g_bad;
    write(1, "thrfault survived\n", 18);
    return arg;
}

int main(void)
{
    pthread_t t;
    void *ret = 0;

    write(1, "thrfault enter\n", 15);
    if (pthread_create(&t, 0, start, (void *)g_token) != 0) {
        write(1, "thrfault create failed\n", 23);
        return 2;
    }
    pthread_join(t, &ret);
    write(1, "thrfault returned\n", 18);
    return ret != g_token;
}
EOC

    for name in thread_create_join thread_mutex thread_cond thread_bridged_work \
                thread_guest_fault; do
        clang -arch x86_64 -std=c11 -O1 -fno-builtin \
                -o "$TMP/$name" "$TMP/$name.c" >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            thread_create_join) TH_CREATE_BIN="$TMP/$name" ;;
            thread_mutex) TH_MUTEX_BIN="$TMP/$name" ;;
            thread_cond) TH_COND_BIN="$TMP/$name" ;;
            thread_bridged_work) TH_WORK_BIN="$TMP/$name" ;;
            thread_guest_fault) TH_FAULT_BIN="$TMP/$name" ;;
        esac
    done
}

build_tlv_fixtures() {
    local name

    cat > "$TMP/tlv_common.h" <<'EOC'
#include "thread_common.h"

#define TLV_NOINL __attribute__((noinline))

static unsigned tlv_nonzero(const unsigned char *p, cb_size n, cb_size step)
{
    unsigned bad = 0;
    cb_size i;

    for (i = 0; i < n; i += step)
        if (p[i] != 0)
            bad++;
    if (n != 0 && p[n - 1] != 0)
        bad++;
    return bad;
}

static int tlv_same(const unsigned char *a, const unsigned char *b, cb_size n)
{
    cb_size i;

    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
EOC

    cat > "$TMP/tlv_main.c" <<'EOC'
#include "tlv_common.h"

#define TAG "tlv_main"
#define ROUNDS 48
#define INT0 0x5eed1234
#define DBL0 (-1234.0625)
#define A1 0x0123456789abcdefull
#define A2 0xfedcba9876543210ull
#define A3 0x1111222233334444ull
#define A4 0x8000000000000001ull
#define A5 0x00000000ffffffffull
#define A6 0x7fffffff00000000ull

__thread int tv_int = INT0;
__thread double tv_dbl = DBL0;

TLV_NOINL int *int_addr(void)
{
    return &tv_int;
}

TLV_NOINL double *dbl_addr(void)
{
    return &tv_dbl;
}

TLV_NOINL int read_int(void)
{
    return tv_int;
}

TLV_NOINL double read_dbl(void)
{
    return tv_dbl;
}

TLV_NOINL void step(int by)
{
    tv_int += by;
    tv_dbl += 0.5 * (double)by;
}

TLV_NOINL cb_uptr live_args(cb_uptr a, cb_uptr b, cb_uptr c, cb_uptr d, cb_uptr e, cb_uptr f)
{
    tv_int += 1;
    return a * 3u + b * 5u + c * 7u + d * 11u + e * 13u + f * 17u + (cb_uptr)(unsigned)tv_int;
}

int main(void)
{
    unsigned m = 0, bit = 1, value = 17;
    int *ia;
    double *da;
    int i, first_int, sum = 0, stable = 1, ints = 1, dbls = 1, direct = 1;
    double first_dbl;
    cb_uptr got, want;

    at_note(TAG, "first touch");
    first_int = read_int();
    first_dbl = read_dbl();
    ia = int_addr();
    da = dbl_addr();
    at_note(TAG, "stepping");
    for (i = 0; i < ROUNDS; i++) {
        step(i + 1);
        sum += i + 1;
        if (int_addr() != ia || dbl_addr() != da || &tv_int != ia || &tv_dbl != da)
            stable = 0;
        if (read_int() != INT0 + sum || tv_int != INT0 + sum)
            ints = 0;
        if (read_dbl() != DBL0 + 0.5 * (double)sum || tv_dbl != DBL0 + 0.5 * (double)sum)
            dbls = 0;
        value = value * 31u + (unsigned)read_int();
    }
    *ia = 0x0badf00d;
    *da = 0.125;
    if (read_int() != 0x0badf00d || tv_int != 0x0badf00d || read_dbl() != 0.125 || tv_dbl != 0.125)
        direct = 0;
    tv_int = -99;
    tv_dbl = 1e300;
    if (*ia != -99 || read_int() != -99 || *da != 1e300 || read_dbl() != 1e300)
        direct = 0;
    got = live_args(A1, A2, A3, A4, A5, A6);
    want = A1 * 3u + A2 * 5u + A3 * 7u + A4 * 11u + A5 * 13u + A6 * 17u + (cb_uptr)(unsigned)read_int();
    value = value * 31u + (unsigned)(got >> 32);
    value = value * 31u + (unsigned)got;

    CK(first_int == INT0);
    CK(first_dbl == DBL0);
    CK(stable);
    CK(ints);
    CK(dbls);
    CK(direct);
    CK(read_int() == -98 && got == want);
    CK((cb_uptr)ia != (cb_uptr)da && ((cb_uptr)ia & 3) == 0 && ((cb_uptr)da & 7) == 0);
    at_note(TAG, "checked");

    cb_begin(TAG, m);
    cb_field("rounds", ROUNDS, 0);
    cb_field("value", value, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/tlv_bss.c" <<'EOC'
#include "tlv_common.h"

#define TAG "tlv_bss"
#define BIG (1u << 20)
#define HEAD "tlv_bss initialized head bytes"
#define PLAIN "ordinary initialized data bytes"

__thread unsigned char tv_head[32] = HEAD;
__thread unsigned char tv_big[BIG];
__thread unsigned tv_tail;
unsigned char g_plain[BIG];
unsigned char g_plain_data[32] = PLAIN;

static const unsigned char g_head0[32] = HEAD;
static const unsigned char g_plain0[32] = PLAIN;

TLV_NOINL unsigned char *big_addr(void)
{
    return tv_big;
}

TLV_NOINL unsigned char *head_addr(void)
{
    return tv_head;
}

TLV_NOINL unsigned *tail_addr(void)
{
    return &tv_tail;
}

static unsigned char pattern(cb_size i)
{
    return (unsigned char)((i * 131u + 7u) ^ (i >> 12));
}

static int apart(cb_uptr a, cb_size an, cb_uptr b, cb_size bn)
{
    return a + an <= b || b + bn <= a;
}

int main(void)
{
    unsigned m = 0, bit = 1, sum = 2166136261u, nonzero = 0, direct_nonzero = 0, back_bad = 0, plain_bad = 0;
    unsigned char *big, *head;
    unsigned *tail;
    cb_size i;
    int tail_zero, head_ok, kept, stable, separate;

    at_note(TAG, "writing ordinary globals");
    for (i = 0; i < BIG; i++)
        g_plain[i] = 0xa5;
    for (i = 0; i < sizeof g_plain_data; i++)
        g_plain_data[i] = (unsigned char)(g_plain_data[i] ^ 0x5a);

    at_note(TAG, "first touch");
    big = big_addr();
    for (i = 0; i < BIG; i++)
        if (big[i] != 0)
            nonzero++;
    for (i = 0; i < BIG; i += 4096)
        if (tv_big[i + 4095] != 0)
            direct_nonzero++;
    tail = tail_addr();
    tail_zero = *tail == 0 && tv_tail == 0;
    head = head_addr();
    head_ok = tlv_same(head, g_head0, sizeof g_head0);

    at_note(TAG, "writing thread-local array");
    for (i = 0; i < BIG; i++)
        big[i] = pattern(i);
    *tail = 0xfeedfaceu;
    for (i = 0; i < BIG; i++) {
        if (big[i] != pattern(i))
            back_bad++;
        sum = (sum ^ big[i]) * 16777619u;
    }
    for (i = 0; i < BIG; i += 4096)
        if (tv_big[i + 17] != pattern(i + 17))
            back_bad++;
    kept = tlv_same(head_addr(), g_head0, sizeof g_head0) && *tail_addr() == 0xfeedfaceu &&
           tv_tail == 0xfeedfaceu;
    stable = big_addr() == big && head_addr() == head && tail_addr() == tail;
    separate = apart((cb_uptr)big, BIG, (cb_uptr)head, sizeof tv_head) &&
               apart((cb_uptr)big, BIG, (cb_uptr)tail, sizeof tv_tail) &&
               apart((cb_uptr)head, sizeof tv_head, (cb_uptr)tail, sizeof tv_tail) &&
               apart((cb_uptr)big, BIG, (cb_uptr)g_plain, BIG) &&
               apart((cb_uptr)big, BIG, (cb_uptr)g_plain_data, sizeof g_plain_data);
    for (i = 0; i < BIG; i++)
        if (g_plain[i] != 0xa5)
            plain_bad++;
    for (i = 0; i < sizeof g_plain_data; i++)
        if (g_plain_data[i] != (unsigned char)(g_plain0[i] ^ 0x5a))
            plain_bad++;

    CK(nonzero == 0 && direct_nonzero == 0);
    CK(tail_zero);
    CK(head_ok);
    CK(back_bad == 0);
    CK(kept);
    CK(stable);
    CK(separate);
    CK(plain_bad == 0);
    at_note(TAG, "checked");

    cb_begin(TAG, m);
    cb_field("bytes", BIG, 0);
    cb_field("nonzero", nonzero, 0);
    cb_field("sum", sum, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/tlv_layout.c" <<'EOC'
#include "tlv_common.h"

#define TAG "tlv_layout"
#define NV 15

struct small {
    char c;
    double d;
    short s;
};

struct var {
    cb_uptr at;
    cb_size size;
    cb_size align;
};

const char g_text[] = "thread-local pointer target";
const char g_other[] = "another pointer target";

__thread char tv_c = 'Q';
__thread unsigned char tv_z1;
__thread short tv_s = -12345;
__thread int tv_i = 0x7a5b3c1d;
__thread signed char tv_c2 = -7;
__thread long long tv_ll = 0x0123456789abcdefLL;
__thread short tv_z2[5];
__thread double tv_d = 6.02214076e23;
__thread float tv_f = -3.75f;
__thread struct small tv_st = { 'z', -0.1875, 0x2bcd };
__thread int tv_arr[7] = { 2, 3, 5, 7, 11, 13, 17 };
__thread const char *tv_p = g_text;
__thread double tv_z3;
_Alignas(16) __thread unsigned char tv_a16[3] = { 0xa1, 0x16, 0x03 };
__thread unsigned char tv_tail = 0xee;

static struct var g_var[NV];

TLV_NOINL void take_addresses(struct var *v)
{
    v[0] = (struct var){ (cb_uptr)&tv_c, sizeof tv_c, _Alignof(char) };
    v[1] = (struct var){ (cb_uptr)&tv_z1, sizeof tv_z1, _Alignof(unsigned char) };
    v[2] = (struct var){ (cb_uptr)&tv_s, sizeof tv_s, _Alignof(short) };
    v[3] = (struct var){ (cb_uptr)&tv_i, sizeof tv_i, _Alignof(int) };
    v[4] = (struct var){ (cb_uptr)&tv_c2, sizeof tv_c2, _Alignof(signed char) };
    v[5] = (struct var){ (cb_uptr)&tv_ll, sizeof tv_ll, _Alignof(long long) };
    v[6] = (struct var){ (cb_uptr)&tv_z2, sizeof tv_z2, _Alignof(short) };
    v[7] = (struct var){ (cb_uptr)&tv_d, sizeof tv_d, _Alignof(double) };
    v[8] = (struct var){ (cb_uptr)&tv_f, sizeof tv_f, _Alignof(float) };
    v[9] = (struct var){ (cb_uptr)&tv_st, sizeof tv_st, _Alignof(struct small) };
    v[10] = (struct var){ (cb_uptr)&tv_arr, sizeof tv_arr, _Alignof(int) };
    v[11] = (struct var){ (cb_uptr)&tv_p, sizeof tv_p, _Alignof(const char *) };
    v[12] = (struct var){ (cb_uptr)&tv_z3, sizeof tv_z3, _Alignof(double) };
    v[13] = (struct var){ (cb_uptr)&tv_a16, sizeof tv_a16, 16 };
    v[14] = (struct var){ (cb_uptr)&tv_tail, sizeof tv_tail, 1 };
}

TLV_NOINL void write_new(void)
{
    int k;

    tv_c = 'r';
    tv_z1 = 0x9b;
    tv_s = 31000;
    tv_i = -0x13572468;
    tv_c2 = 99;
    tv_ll = -0x0fedcba987654321LL;
    for (k = 0; k < 5; k++)
        tv_z2[k] = (short)(-1000 - k);
    tv_d = -2.5e-10;
    tv_f = 1024.5f;
    tv_st.c = 'y';
    tv_st.d = 12345.75;
    tv_st.s = -0x1bcd;
    for (k = 0; k < 7; k++)
        tv_arr[k] = 1000 * (k + 1) + 1;
    tv_p = g_other;
    tv_z3 = 0.0078125;
    tv_a16[0] = 0x5c;
    tv_a16[1] = 0x6d;
    tv_a16[2] = 0x7e;
    tv_tail = 0x11;
}

TLV_NOINL int read_new(void)
{
    int k, ok = 1;

    ok &= tv_c == 'r' && tv_z1 == 0x9b && tv_s == 31000 && tv_i == -0x13572468 && tv_c2 == 99;
    ok &= tv_ll == -0x0fedcba987654321LL;
    for (k = 0; k < 5; k++)
        ok &= tv_z2[k] == (short)(-1000 - k);
    ok &= tv_d == -2.5e-10 && tv_f == 1024.5f;
    ok &= tv_st.c == 'y' && tv_st.d == 12345.75 && tv_st.s == -0x1bcd;
    for (k = 0; k < 7; k++)
        ok &= tv_arr[k] == 1000 * (k + 1) + 1;
    ok &= tv_p == g_other && tv_z3 == 0.0078125;
    ok &= tv_a16[0] == 0x5c && tv_a16[1] == 0x6d && tv_a16[2] == 0x7e && tv_tail == 0x11;
    return ok;
}

int main(void)
{
    static const int primes[7] = { 2, 3, 5, 7, 11, 13, 17 };
    unsigned m = 0, bit = 1, value = 17;
    struct var again[NV];
    cb_uptr lo = ~(cb_uptr)0, hi = 0;
    int i, j, zeros = 1, arr = 1, aligned = 1, disjoint = 1, stable = 1;

    at_note(TAG, "first touch");
    take_addresses(g_var);
    for (i = 0; i < 5; i++)
        if (tv_z2[i] != 0)
            zeros = 0;
    if (tv_z1 != 0 || tv_z3 != 0.0)
        zeros = 0;
    for (i = 0; i < 7; i++)
        if (tv_arr[i] != primes[i])
            arr = 0;
    CK(tv_c == 'Q');
    CK(tv_s == -12345);
    CK(tv_i == 0x7a5b3c1d);
    CK(tv_c2 == -7);
    CK(tv_ll == 0x0123456789abcdefLL);
    CK(tv_d == 6.02214076e23);
    CK(tv_f == -3.75f);
    CK(tv_st.c == 'z' && tv_st.d == -0.1875 && tv_st.s == 0x2bcd);
    CK(arr);
    CK(tv_p == g_text && tv_p[0] == 't');
    CK(tv_a16[0] == 0xa1 && tv_a16[1] == 0x16 && tv_a16[2] == 0x03);
    CK(tv_tail == 0xee);
    CK(zeros);

    value = value * 31u + (unsigned)tv_c;
    value = value * 31u + (unsigned)tv_s;
    value = value * 31u + (unsigned)tv_i;
    value = value * 31u + (unsigned)tv_c2;
    value = value * 31u + (unsigned)(tv_ll >> 32);
    value = value * 31u + (unsigned)tv_ll;
    value = value * 31u + (unsigned)(int)(tv_d / 1e18);
    value = value * 31u + (unsigned)(int)(tv_f * 4.0f);
    value = value * 31u + (unsigned)tv_st.s;
    for (i = 0; i < 7; i++)
        value = value * 31u + (unsigned)tv_arr[i];
    value = value * 31u + tv_a16[0] + tv_a16[1] + tv_a16[2] + tv_tail;

    at_note(TAG, "checking addresses");
    for (i = 0; i < NV; i++) {
        if (g_var[i].at % g_var[i].align != 0)
            aligned = 0;
        if (g_var[i].at < lo)
            lo = g_var[i].at;
        if (g_var[i].at + g_var[i].size > hi)
            hi = g_var[i].at + g_var[i].size;
        for (j = 0; j < i; j++)
            if (g_var[i].at < g_var[j].at + g_var[j].size && g_var[j].at < g_var[i].at + g_var[i].size)
                disjoint = 0;
    }
    CK(aligned);
    CK(disjoint && hi - lo <= 4096);

    at_note(TAG, "writing every variable");
    write_new();
    CK(read_new());
    take_addresses(again);
    for (i = 0; i < NV; i++)
        if (again[i].at != g_var[i].at)
            stable = 0;
    CK(stable);
    at_note(TAG, "checked");

    cb_begin(TAG, m);
    cb_field("vars", NV, 0);
    cb_field("value", value, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/tlv_threads.c" <<'EOC'
#include "tlv_common.h"

#define THREADS 6
#define SEATS (THREADS + 1)
#define BSS 8192
#define TAG "tlv_threads"
#define INT0 0x7157a11
#define DBL0 3.140625
#define NAME0 "tlv template"

struct seat {
    int *int_at;
    double *dbl_at;
    unsigned char *bss_at;
    unsigned runs;
    unsigned fresh_bad;
    unsigned bss_bad;
    unsigned own_bad;
    unsigned kept_bad;
    unsigned skew;
    unsigned sum;
};

__thread int tv_int = INT0;
__thread double tv_dbl = DBL0;
__thread unsigned char tv_name[16] = NAME0;
__thread unsigned char tv_bss[BSS];

static const unsigned char g_name0[16] = NAME0;
static struct seat g_seat[SEATS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static unsigned g_ready, g_release, g_bad_arg, g_lock_err;

static int want_int(unsigned k)
{
    return (int)(0x10000000u + k * 0x01010101u);
}

static double want_dbl(unsigned k)
{
    return -0.5 - 2.25 * (double)k;
}

static unsigned char want_byte(unsigned k, unsigned i)
{
    return (unsigned char)(k * 37u + i * 11u + 1u);
}

TLV_NOINL int *int_addr(void)
{
    return &tv_int;
}

TLV_NOINL double *dbl_addr(void)
{
    return &tv_dbl;
}

TLV_NOINL unsigned char *bss_addr(void)
{
    return tv_bss;
}

TLV_NOINL int fresh(void)
{
    return tv_int == INT0 && tv_dbl == DBL0 && tlv_same(tv_name, g_name0, sizeof g_name0);
}

TLV_NOINL void make_own(unsigned k)
{
    unsigned i;

    tv_int = want_int(k);
    tv_dbl = want_dbl(k);
    for (i = 0; i < sizeof tv_name; i++)
        tv_name[i] = want_byte(k, i);
    for (i = 0; i < BSS; i++)
        tv_bss[i] = want_byte(k, i + 16);
}

TLV_NOINL int still_own(unsigned k, struct seat *s)
{
    unsigned i;
    int ok = int_addr() == s->int_at && dbl_addr() == s->dbl_at && bss_addr() == s->bss_at;

    ok &= tv_int == want_int(k) && tv_dbl == want_dbl(k);
    for (i = 0; i < sizeof tv_name; i++)
        ok &= tv_name[i] == want_byte(k, i);
    for (i = 0; i < BSS; i++)
        ok &= s->bss_at[i] == want_byte(k, i + 16);
    return ok;
}

static void enter(unsigned k, struct seat *s)
{
    s->runs++;
    if (!fresh())
        s->fresh_bad++;
    s->bss_bad = tlv_nonzero(bss_addr(), BSS, 1);
    s->int_at = int_addr();
    s->dbl_at = dbl_addr();
    s->bss_at = bss_addr();
    make_own(k);
    if (!still_own(k, s))
        s->own_bad++;
    s->sum = (unsigned)tv_int * 31u + (unsigned)tv_name[3] + s->bss_at[BSS - 1];
}

static void *start(void *arg)
{
    struct seat *s = arg;
    unsigned k;

    at_note(TAG, "thread entered");
    if (!TH_IN(s, g_seat) || s == &g_seat[THREADS]) {
        g_bad_arg++;
        at_note(TAG, "thread leaving");
        return 0;
    }
    k = (unsigned)(s - g_seat);
    if (CB_SKEWED())
        s->skew++;
    enter(k, s);
    if (pthread_mutex_lock(&g_lock) != 0)
        g_lock_err++;
    g_ready++;
    if (pthread_cond_broadcast(&g_cond) != 0)
        g_lock_err++;
    while (!g_release)
        if (pthread_cond_wait(&g_cond, &g_lock) != 0)
            g_lock_err++;
    if (pthread_mutex_unlock(&g_lock) != 0)
        g_lock_err++;
    if (!still_own(k, s))
        s->kept_bad++;
    at_note(TAG, "thread leaving");
    return s;
}

int main(void)
{
    unsigned m = 0, bit = 1, value = 17;
    struct seat *main_seat = &g_seat[THREADS];
    pthread_t tid[THREADS];
    int ok_tid[THREADS];
    void *ret;
    int i, j, created = 1, joined = 1, results = 1, runs = 1, fresh_ok = 1, bss_ok = 1;
    int own_ok = 1, kept_ok = 1, distinct = 1, skew = 0;

    at_note(TAG, "first touch");
    enter(THREADS, main_seat);
    for (i = 0; i < THREADS; i++) {
        at_note(TAG, "calling pthread_create");
        ok_tid[i] = pthread_create(&tid[i], 0, start, &g_seat[i]) == 0;
        at_note(TAG, "returned from pthread_create");
        if (!ok_tid[i]) {
            created = 0;
            continue;
        }
        if (pthread_mutex_lock(&g_lock) != 0)
            g_lock_err++;
        at_note(TAG, "waiting");
        while (g_ready < (unsigned)i + 1u)
            if (pthread_cond_wait(&g_cond, &g_lock) != 0)
                g_lock_err++;
        at_note(TAG, "wait returned");
        if (pthread_mutex_unlock(&g_lock) != 0)
            g_lock_err++;
    }
    if (pthread_mutex_lock(&g_lock) != 0)
        g_lock_err++;
    g_release = 1;
    if (pthread_cond_broadcast(&g_cond) != 0)
        g_lock_err++;
    if (pthread_mutex_unlock(&g_lock) != 0)
        g_lock_err++;
    for (i = 0; i < THREADS; i++) {
        ret = 0;
        if (ok_tid[i]) {
            at_note(TAG, "calling pthread_join");
            if (pthread_join(tid[i], &ret) != 0)
                joined = 0;
            at_note(TAG, "returned from pthread_join");
        }
        if (ret != &g_seat[i])
            results = 0;
    }
    for (i = 0; i < SEATS; i++) {
        if (g_seat[i].runs != 1)
            runs = 0;
        if (i < THREADS && g_seat[i].fresh_bad)
            fresh_ok = 0;
        if (i < THREADS && g_seat[i].bss_bad)
            bss_ok = 0;
        if (g_seat[i].own_bad)
            own_ok = 0;
        if (g_seat[i].kept_bad)
            kept_ok = 0;
        if (g_seat[i].skew)
            skew = 1;
        for (j = 0; j < i; j++)
            if (g_seat[i].int_at == g_seat[j].int_at || g_seat[i].dbl_at == g_seat[j].dbl_at ||
                g_seat[i].bss_at == g_seat[j].bss_at)
                distinct = 0;
        value = value * 31u + g_seat[i].sum;
    }

    CK(created && joined && results && runs && g_bad_arg == 0);
    CK(main_seat->fresh_bad == 0 && main_seat->bss_bad == 0);
    CK(fresh_ok);
    CK(bss_ok);
    CK(own_ok);
    CK(kept_ok);
    CK(distinct);
    CK(still_own(THREADS, main_seat));
    CK(g_lock_err == 0);
    CK(!skew);

    cb_begin(TAG, m);
    cb_field("threads", THREADS, 0);
    cb_field("value", value, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/tlv_thread_churn.c" <<EOC
#include "tlv_common.h"

#define THREADS ${TLV_CHURN_THREADS}
#define MAX_THREADS 1024
#define BIG ${TLV_CHURN_BLOCK}u
#define PAGE 4096u
#define TAG "tlv_thread_churn"
#define SEQ0 0xc4012e5u
#define DBL0 0.0625
#define DATA0 "churn template bytes, 32 long.."

struct run {
    unsigned runs;
    unsigned fresh_bad;
    unsigned zero_bad;
    unsigned own_bad;
    unsigned skew;
    unsigned sum;
};

__thread unsigned tv_seq = SEQ0;
__thread double tv_dbl = DBL0;
__thread unsigned char tv_data[32] = DATA0;
__thread unsigned char tv_big[BIG];

static const unsigned char g_data0[32] = DATA0;
static struct run g_run[MAX_THREADS + 1];
static unsigned g_bad_arg;

static unsigned count_arg(int argc, char **argv)
{
    unsigned n = 0;
    const char *s;

    if (argc < 2)
        return THREADS;
    for (s = argv[1]; *s >= '0' && *s <= '9' && n <= MAX_THREADS; s++)
        n = n * 10u + (unsigned)(*s - '0');
    return (n == 0 || n > MAX_THREADS || *s != 0) ? THREADS : n;
}

static unsigned char mark(unsigned k, unsigned page)
{
    return (unsigned char)((k * 13u + page) | 1u);
}

TLV_NOINL unsigned char *big_addr(void)
{
    return tv_big;
}

TLV_NOINL int fresh(void)
{
    return tv_seq == SEQ0 && tv_dbl == DBL0 && tlv_same(tv_data, g_data0, sizeof g_data0);
}

TLV_NOINL void make_own(unsigned k)
{
    unsigned char *big = big_addr();
    unsigned i;

    tv_seq = SEQ0 ^ (k * 0x9e3779b9u);
    tv_dbl = (double)k + 0.25;
    for (i = 0; i < sizeof tv_data; i++)
        tv_data[i] = (unsigned char)(k + i);
    for (i = 0; i < BIG; i += PAGE)
        big[i] = mark(k, i / PAGE);
    big[BIG - 1] = mark(k, BIG / PAGE);
}

TLV_NOINL int still_own(unsigned k, unsigned char *at)
{
    unsigned char *big = big_addr();
    unsigned i;
    int ok = big == at && tv_seq == (SEQ0 ^ (k * 0x9e3779b9u)) && tv_dbl == (double)k + 0.25;

    for (i = 0; i < sizeof tv_data; i++)
        ok &= tv_data[i] == (unsigned char)(k + i);
    for (i = 0; i < BIG; i += PAGE)
        ok &= big[i] == mark(k, i / PAGE);
    ok &= big[BIG - 1] == mark(k, BIG / PAGE);
    return ok;
}

static void touch(unsigned k, struct run *r)
{
    unsigned char *at;

    r->runs++;
    if (!fresh())
        r->fresh_bad++;
    at = big_addr();
    r->zero_bad = tlv_nonzero(at, BIG, PAGE);
    make_own(k);
    if (!still_own(k, at))
        r->own_bad++;
    r->sum = tv_seq * 31u + (unsigned)tv_data[k % 32u] + tv_big[(k * PAGE) % BIG];
}

static void *start(void *arg)
{
    struct run *r = arg;

    at_note(TAG, "thread entered");
    if ((cb_uptr)r - (cb_uptr)g_run >= MAX_THREADS * sizeof g_run[0] ||
        ((cb_uptr)r - (cb_uptr)g_run) % sizeof g_run[0] != 0) {
        g_bad_arg++;
        at_note(TAG, "thread leaving");
        return 0;
    }
    if (CB_SKEWED())
        r->skew++;
    touch((unsigned)(r - g_run), r);
    at_note(TAG, "thread leaving");
    return r;
}

int main(int argc, char **argv)
{
    unsigned m = 0, bit = 1, value = 17, n = count_arg(argc, argv), k;
    unsigned fresh_bad = 0, zero_bad = 0, own_bad = 0;
    struct run *mine = &g_run[MAX_THREADS];
    unsigned char *main_at;
    pthread_t t;
    void *ret;
    int created = 1, joined = 1, results = 1, runs = 1, skew = 0;

    at_note(TAG, "first touch");
    touch(MAX_THREADS, mine);
    main_at = big_addr();
    for (k = 0; k < n; k++) {
        at_note(TAG, "calling pthread_create");
        if (pthread_create(&t, 0, start, &g_run[k]) != 0) {
            at_note(TAG, "returned from pthread_create");
            created = 0;
            break;
        }
        at_note(TAG, "returned from pthread_create");
        ret = 0;
        at_note(TAG, "calling pthread_join");
        if (pthread_join(t, &ret) != 0)
            joined = 0;
        at_note(TAG, "returned from pthread_join");
        if (ret != &g_run[k])
            results = 0;
        if (g_run[k].runs != 1)
            runs = 0;
        if (g_run[k].fresh_bad)
            fresh_bad++;
        if (g_run[k].zero_bad)
            zero_bad++;
        if (g_run[k].own_bad)
            own_bad++;
        if (g_run[k].skew)
            skew = 1;
        value = value * 31u + g_run[k].sum;
    }

    CK(created && joined && results && runs && g_bad_arg == 0);
    CK(mine->fresh_bad == 0 && mine->zero_bad == 0 && mine->own_bad == 0);
    CK(fresh_bad == 0);
    CK(zero_bad == 0);
    CK(own_bad == 0);
    CK(still_own(MAX_THREADS, main_at));
    CK(!skew);

    cb_begin(TAG, m);
    cb_field("threads", n, 0);
    cb_field("stale", fresh_bad, 0);
    cb_field("value", value, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/tlvdep.c" <<'EOC'
typedef __SIZE_TYPE__ dep_size;

long write(int, const void *, dep_size);

#define DEP_BSS 4096

__thread int tlvdep_counter = 0x0d11b0a7;
__thread double tlvdep_scale = 0.75;
__thread unsigned char tlvdep_bss[DEP_BSS];

int *tlvdep_counter_addr(void)
{
    return &tlvdep_counter;
}

double *tlvdep_scale_addr(void)
{
    return &tlvdep_scale;
}

int tlvdep_counter_get(void)
{
    return tlvdep_counter;
}

int tlvdep_bump(int by)
{
    tlvdep_counter += by;
    tlvdep_scale *= 2.0;
    return tlvdep_counter;
}

unsigned tlvdep_bss_nonzero(void)
{
    unsigned n = 0;
    int i;

    for (i = 0; i < DEP_BSS; i++)
        if (tlvdep_bss[i] != 0)
            n++;
    return n;
}

void tlvdep_bss_fill(unsigned char v)
{
    int i;

    for (i = 0; i < DEP_BSS; i++)
        tlvdep_bss[i] = (unsigned char)(v + i);
}

int tlvdep_bss_holds(unsigned char v)
{
    int i;

    for (i = 0; i < DEP_BSS; i++)
        if (tlvdep_bss[i] != (unsigned char)(v + i))
            return 0;
    return 1;
}

void tlvdep_note(const char *what)
{
    char b[64];
    dep_size n = 0;

    for (; n < 8; n++)
        b[n] = "tlvdep: "[n];
    while (*what && n < sizeof b - 1)
        b[n++] = *what++;
    b[n++] = '\n';
    write(2, b, n);
}
EOC

    cat > "$TMP/tlv_dylib.c" <<'EOC'
#include "tlv_common.h"

#define THREADS 4
#define SEATS (THREADS + 1)
#define TAG "tlv_dylib"
#define DEP0 0x0d11b0a7
#define MAIN0 0x3a1a3a1a

extern __thread int tlvdep_counter;
int *tlvdep_counter_addr(void);
double *tlvdep_scale_addr(void);
int tlvdep_counter_get(void);
int tlvdep_bump(int);
unsigned tlvdep_bss_nonzero(void);
void tlvdep_bss_fill(unsigned char);
int tlvdep_bss_holds(unsigned char);
void tlvdep_note(const char *);

struct seat {
    int *dep_at;
    int *main_at;
    unsigned runs;
    unsigned fresh_bad;
    unsigned bss_bad;
    unsigned cross_bad;
    unsigned apart_bad;
    unsigned own_bad;
    unsigned kept_bad;
    unsigned skew;
    unsigned sum;
};

__thread int tv_main = MAIN0;

static struct seat g_seat[SEATS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static unsigned g_ready, g_release, g_bad_arg, g_lock_err;

static int dep_want(unsigned k)
{
    return DEP0 + 100 * (int)k + 7;
}

static int main_want(unsigned k)
{
    return (int)(0x60000000u + k * 0x00110011u);
}

TLV_NOINL int *main_addr(void)
{
    return &tv_main;
}

TLV_NOINL int *dep_addr_here(void)
{
    return &tlvdep_counter;
}

static int own(unsigned k, struct seat *s)
{
    return tlvdep_counter_addr() == s->dep_at && dep_addr_here() == s->dep_at &&
           main_addr() == s->main_at && tlvdep_counter_get() == dep_want(k) &&
           tlvdep_counter == dep_want(k) && *tlvdep_scale_addr() == 1.5 &&
           tv_main == main_want(k) && tlvdep_bss_holds((unsigned char)(k * 29u + 3u));
}

static void enter(unsigned k, struct seat *s)
{
    s->runs++;
    s->dep_at = tlvdep_counter_addr();
    s->main_at = main_addr();
    if (tlvdep_counter_get() != DEP0 || *tlvdep_scale_addr() != 0.75 || tv_main != MAIN0)
        s->fresh_bad++;
    s->bss_bad = tlvdep_bss_nonzero();
    if (dep_addr_here() != s->dep_at || tlvdep_counter != DEP0)
        s->cross_bad++;
    if ((cb_uptr)s->dep_at == (cb_uptr)s->main_at)
        s->apart_bad++;
    tv_main = main_want(k);
    if (tlvdep_bump(100 * (int)k + 7) != dep_want(k) || tv_main != main_want(k))
        s->apart_bad++;
    tlvdep_bss_fill((unsigned char)(k * 29u + 3u));
    if (!own(k, s))
        s->own_bad++;
    s->sum = (unsigned)tlvdep_counter * 31u + (unsigned)tv_main;
}

static void *start(void *arg)
{
    struct seat *s = arg;
    unsigned k;

    at_note(TAG, "thread entered");
    if (!TH_IN(s, g_seat) || s == &g_seat[THREADS]) {
        g_bad_arg++;
        at_note(TAG, "thread leaving");
        return 0;
    }
    k = (unsigned)(s - g_seat);
    if (CB_SKEWED())
        s->skew++;
    enter(k, s);
    if (pthread_mutex_lock(&g_lock) != 0)
        g_lock_err++;
    g_ready++;
    if (pthread_cond_broadcast(&g_cond) != 0)
        g_lock_err++;
    while (!g_release)
        if (pthread_cond_wait(&g_cond, &g_lock) != 0)
            g_lock_err++;
    if (pthread_mutex_unlock(&g_lock) != 0)
        g_lock_err++;
    if (!own(k, s))
        s->kept_bad++;
    at_note(TAG, "thread leaving");
    return s;
}

int main(void)
{
    unsigned m = 0, bit = 1, value = 17;
    struct seat *main_seat = &g_seat[THREADS];
    pthread_t tid[THREADS];
    int ok_tid[THREADS];
    void *ret;
    int i, j, created = 1, joined = 1, results = 1, runs = 1, fresh_ok = 1, bss_ok = 1;
    int cross_ok = 1, apart_ok = 1, own_ok = 1, kept_ok = 1, distinct = 1, skew = 0;

    tlvdep_note("dylib code reached");
    at_note(TAG, "first touch");
    enter(THREADS, main_seat);
    for (i = 0; i < THREADS; i++) {
        at_note(TAG, "calling pthread_create");
        ok_tid[i] = pthread_create(&tid[i], 0, start, &g_seat[i]) == 0;
        at_note(TAG, "returned from pthread_create");
        if (!ok_tid[i]) {
            created = 0;
            continue;
        }
        if (pthread_mutex_lock(&g_lock) != 0)
            g_lock_err++;
        at_note(TAG, "waiting");
        while (g_ready < (unsigned)i + 1u)
            if (pthread_cond_wait(&g_cond, &g_lock) != 0)
                g_lock_err++;
        at_note(TAG, "wait returned");
        if (pthread_mutex_unlock(&g_lock) != 0)
            g_lock_err++;
    }
    if (pthread_mutex_lock(&g_lock) != 0)
        g_lock_err++;
    g_release = 1;
    if (pthread_cond_broadcast(&g_cond) != 0)
        g_lock_err++;
    if (pthread_mutex_unlock(&g_lock) != 0)
        g_lock_err++;
    for (i = 0; i < THREADS; i++) {
        ret = 0;
        if (ok_tid[i]) {
            at_note(TAG, "calling pthread_join");
            if (pthread_join(tid[i], &ret) != 0)
                joined = 0;
            at_note(TAG, "returned from pthread_join");
        }
        if (ret != &g_seat[i])
            results = 0;
    }
    for (i = 0; i < SEATS; i++) {
        if (g_seat[i].runs != 1)
            runs = 0;
        if (i < THREADS && g_seat[i].fresh_bad)
            fresh_ok = 0;
        if (i < THREADS && g_seat[i].bss_bad)
            bss_ok = 0;
        if (g_seat[i].cross_bad)
            cross_ok = 0;
        if (g_seat[i].apart_bad)
            apart_ok = 0;
        if (g_seat[i].own_bad)
            own_ok = 0;
        if (g_seat[i].kept_bad)
            kept_ok = 0;
        if (g_seat[i].skew)
            skew = 1;
        for (j = 0; j < i; j++)
            if (g_seat[i].dep_at == g_seat[j].dep_at || g_seat[i].main_at == g_seat[j].main_at)
                distinct = 0;
        value = value * 31u + g_seat[i].sum;
    }

    CK(created && joined && results && runs && g_bad_arg == 0);
    CK(main_seat->fresh_bad == 0 && main_seat->bss_bad == 0);
    CK(fresh_ok);
    CK(bss_ok);
    CK(cross_ok);
    CK(apart_ok);
    CK(own_ok);
    CK(kept_ok);
    CK(distinct);
    CK(own(THREADS, main_seat));
    CK(g_lock_err == 0);
    CK(!skew);

    cb_begin(TAG, m);
    cb_field("threads", THREADS, 0);
    cb_field("value", value, 1);
    cb_end();
    return m != 0;
}
EOC

    for name in tlv_main tlv_bss tlv_layout tlv_threads tlv_thread_churn; do
        clang -arch x86_64 -std=c11 -O1 -fno-builtin \
                -o "$TMP/$name" "$TMP/$name.c" >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            tlv_main) TLV_MAIN_BIN="$TMP/$name" ;;
            tlv_bss) TLV_BSS_BIN="$TMP/$name" ;;
            tlv_layout) TLV_LAYOUT_BIN="$TMP/$name" ;;
            tlv_threads) TLV_THREADS_BIN="$TMP/$name" ;;
            tlv_thread_churn) TLV_CHURN_BIN="$TMP/$name" ;;
        esac
    done
    if clang -arch x86_64 -std=c11 -O1 -fno-builtin -dynamiclib \
            -install_name "@executable_path/$TLV_DEP_NAME" \
            -o "$TMP/$TLV_DEP_NAME" "$TMP/tlvdep.c" >"$TMP/tlv_dylib.cc.log" 2>&1 &&
       clang -arch x86_64 -std=c11 -O1 -fno-builtin \
            -o "$TMP/tlv_dylib" "$TMP/tlv_dylib.c" "$TMP/$TLV_DEP_NAME" >>"$TMP/tlv_dylib.cc.log" 2>&1; then
        TLV_DYLIB_BIN="$TMP/tlv_dylib"
        TLV_DYLIB_LIB="$TMP/$TLV_DEP_NAME"
    fi
}

build_signal_fixtures() {
    local name

    cat > "$TMP/sig_common.h" <<EOC
#include <errno.h>
#include <signal.h>
#include "thread_common.h"

pid_t getpid(void);

#define SIG_WAIT_SECS ${SIGNAL_WAIT_SECS}

static int sig_wait(const volatile sig_atomic_t *flag)
{
    time_t end = time(0) + SIG_WAIT_SECS;
    unsigned long n = 0;

    while (!*flag) {
        getpid();
        if ((++n & 1023) == 0 && time(0) > end)
            return 0;
    }
    return 1;
}

static int sig_inside(cb_uptr p, cb_uptr base, cb_size size)
{
    return base != 0 && p - base < size;
}

static void sig_int(const char *key, int v)
{
    cb_str(" ");
    cb_str(key);
    cb_str("=");
    if (v < 0) {
        cb_str("-");
        cb_dec((unsigned)-(long)v);
    } else {
        cb_dec((unsigned)v);
    }
}
EOC

    cat > "$TMP/signal_sigaction.c" <<'EOC'
#include "sig_common.h"

#define TAG "signal_sigaction"
#define PAD 48

static volatile unsigned long long g_src[6];
static volatile double g_srcd;
static volatile sig_atomic_t g_info_hits, g_plain_hits;
static volatile unsigned g_sig_bad, g_info_bad, g_uc_bad, g_plain_bad, g_skew;

static void on_info(int sig, siginfo_t *info, void *uc)
{
    g_info_hits++;
    if (sig != SIGUSR1)
        g_sig_bad++;
    if (info == 0 || info->si_signo != SIGUSR1)
        g_info_bad++;
    if (uc == 0)
        g_uc_bad++;
    if (CB_SKEWED())
        g_skew++;
}

static void on_plain(int sig)
{
    g_plain_hits++;
    if (sig != SIGUSR1)
        g_plain_bad++;
    if (CB_SKEWED())
        g_skew++;
}

static unsigned fold(unsigned h, unsigned long long v)
{
    return (h * 31u + (unsigned)(v >> 32)) * 31u + (unsigned)v;
}

static unsigned combine(unsigned h, unsigned long long a, unsigned long long b,
                        unsigned long long c, unsigned long long d,
                        unsigned long long e, unsigned long long f, double g, int hits)
{
    h = fold(h, a + (unsigned)hits);
    h = fold(h, b ^ (unsigned)hits);
    h = fold(h, c - (unsigned)hits);
    h = fold(h, d * (unsigned)(hits + 2));
    h = fold(h, e | (unsigned)hits);
    h = fold(h, f + 3u * (unsigned)hits);
    return fold(h, (unsigned long long)(g * 8.0) + (unsigned)hits);
}

__attribute__((noinline)) static unsigned held(int *rc, int *seen, unsigned *pad_bad)
{
    unsigned long long a = g_src[0], b = g_src[1], c = g_src[2];
    unsigned long long d = g_src[3], e = g_src[4], f = g_src[5];
    double g = g_srcd;
    unsigned char pad[PAD];
    unsigned before = 17, i;

    for (i = 0; i < PAD; i++)
        pad[i] = (unsigned char)(a >> (i % 56)) ^ (unsigned char)i;
    before = combine(before, a, b, c, d, e, f, g, 0);
    at_note(TAG, "raising SIGUSR1");
    *rc = raise(SIGUSR1);
    at_note(TAG, "raise returned");
    *seen = g_info_hits;
    for (i = 0; i < PAD; i++)
        if (pad[i] != ((unsigned char)(a >> (i % 56)) ^ (unsigned char)i))
            (*pad_bad)++;
    return combine(before, a, b, c, d, e, f, g, g_info_hits);
}

static unsigned want_held(void)
{
    unsigned h = combine(17, g_src[0], g_src[1], g_src[2], g_src[3], g_src[4], g_src[5], g_srcd, 0);

    return combine(h, g_src[0], g_src[1], g_src[2], g_src[3], g_src[4], g_src[5], g_srcd, 1);
}

int main(void)
{
    unsigned m = 0, bit = 1, sum, pad_bad = 0;
    struct sigaction act, again, old, query;
    sigset_t want_mask;
    int r_install, r_again, r_query, r_raise, r_plain, seen, i;

    for (i = 0; i < 6; i++)
        g_src[i] = ((unsigned long long)at_mix((unsigned)i + 1u) << 32) | at_mix((unsigned)i + 0x5349u);
    g_srcd = 1234.625;

    act.sa_sigaction = on_info;
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, SIGUSR2);
    act.sa_flags = SA_SIGINFO;
    want_mask = act.sa_mask;
    r_install = sigaction(SIGUSR1, &act, 0);

    r_raise = -7;
    seen = -7;
    sum = held(&r_raise, &seen, &pad_bad);

    again.sa_handler = on_plain;
    sigemptyset(&again.sa_mask);
    again.sa_flags = 0;
    old.sa_handler = 0;
    old.sa_mask = 0;
    old.sa_flags = 0;
    r_again = sigaction(SIGUSR1, &again, &old);
    query.sa_handler = 0;
    query.sa_mask = 1;
    query.sa_flags = SA_SIGINFO;
    r_query = sigaction(SIGUSR1, 0, &query);
    at_note(TAG, "raising SIGUSR1 again");
    r_plain = raise(SIGUSR1);
    at_note(TAG, "raise returned");

    CK(r_install == 0 && r_again == 0 && r_query == 0);
    CK(r_raise == 0);
    CK(seen == 1 && g_info_hits == 1);
    CK(g_sig_bad == 0);
    CK(g_info_bad == 0);
    CK(g_uc_bad == 0);
    CK(sum == want_held());
    CK(pad_bad == 0);
    CK(old.sa_sigaction == on_info);
    CK((old.sa_flags & SA_SIGINFO) != 0);
    CK(old.sa_mask == want_mask);
    CK(query.sa_handler == on_plain && (query.sa_flags & SA_SIGINFO) == 0 && query.sa_mask == 0);
    CK(r_plain == 0 && g_plain_hits == 1 && g_plain_bad == 0 && g_info_hits == 1);
    CK(g_skew == 0);

    cb_begin(TAG, m);
    cb_field("hits", (unsigned)(g_info_hits + g_plain_hits), 0);
    cb_field("sum", sum, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/signal_signal.c" <<'EOC'
#include "sig_common.h"

#define TAG "signal_signal"

typedef void (*sig_fn)(int);

static volatile sig_atomic_t g_hits;
static volatile unsigned g_bad, g_skew;

static void on_usr2(int sig)
{
    g_hits++;
    if (sig != SIGUSR2)
        g_bad++;
    if (CB_SKEWED())
        g_skew++;
}

int main(void)
{
    unsigned m = 0, bit = 1;
    sig_fn first, second, ignored, restored, last;
    int r1, r2, r3, hits1, hits2;

    first = signal(SIGUSR2, on_usr2);
    second = signal(SIGUSR2, on_usr2);
    at_note(TAG, "raising SIGUSR2");
    r1 = raise(SIGUSR2);
    hits1 = g_hits;
    at_note(TAG, "raise returned");
    at_note(TAG, "raising SIGUSR2 again");
    r2 = raise(SIGUSR2);
    hits2 = g_hits;
    at_note(TAG, "raise returned");
    ignored = signal(SIGUSR1, SIG_IGN);
    at_note(TAG, "raising ignored SIGUSR1");
    r3 = raise(SIGUSR1);
    at_note(TAG, "raise returned");
    restored = signal(SIGUSR1, SIG_DFL);
    last = signal(SIGUSR2, SIG_DFL);

    CK(first == SIG_DFL);
    CK(second == on_usr2);
    CK(r1 == 0 && hits1 == 1);
    CK(r2 == 0 && hits2 == 2);
    CK(g_bad == 0);
    CK(ignored == SIG_DFL && r3 == 0 && g_hits == 2);
    CK(restored == SIG_IGN && last == on_usr2);
    CK(g_skew == 0);

    cb_begin(TAG, m);
    cb_field("hits", (unsigned)g_hits, 0);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/signal_mask.c" <<'EOC'
#include "sig_common.h"

#define TAG "signal_mask"
#define ROUNDS 3

struct round {
    int block_rc, raise_rc, query_rc, unblock_rc, after_rc;
    int open_before, blocked_now, open_after;
    int hits_blocked, hits_queried, hits_unblocked;
};

static volatile sig_atomic_t g_hits;
static volatile unsigned g_bad, g_own_open, g_extra_open, g_query_bad, g_skew;

static void on_usr1(int sig)
{
    sigset_t in = 0;

    g_hits++;
    if (sig != SIGUSR1)
        g_bad++;
    if (pthread_sigmask(SIG_BLOCK, 0, &in) != 0) {
        g_query_bad++;
    } else {
        if (!sigismember(&in, SIGUSR1))
            g_own_open++;
        if (!sigismember(&in, SIGUSR2))
            g_extra_open++;
    }
    if (CB_SKEWED())
        g_skew++;
}

static int mask_call(int api, int how, const sigset_t *set, sigset_t *old)
{
    if (api == 0)
        return sigprocmask(how, set, old);
    return pthread_sigmask(how, set, old);
}

static void run_round(int api, struct round *r)
{
    sigset_t one, old = ~(sigset_t)0, now = 0, after = ~(sigset_t)0;
    int base = g_hits;

    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);
    at_note(TAG, "blocking SIGUSR1");
    r->block_rc = mask_call(api == 2 ? 0 : api, SIG_BLOCK, &one, &old);
    r->open_before = !sigismember(&old, SIGUSR1) && !sigismember(&old, SIGUSR2);
    at_note(TAG, "raising blocked SIGUSR1");
    r->raise_rc = raise(SIGUSR1);
    r->hits_blocked = g_hits - base;
    at_note(TAG, "raise returned");
    r->query_rc = mask_call(api, SIG_BLOCK, 0, &now);
    r->blocked_now = sigismember(&now, SIGUSR1);
    r->hits_queried = g_hits - base;
    at_note(TAG, "unblocking SIGUSR1");
    r->unblock_rc = mask_call(api, SIG_UNBLOCK, &one, 0);
    r->hits_unblocked = g_hits - base;
    at_note(TAG, "unblock returned");
    r->after_rc = mask_call(api, 0, 0, &after);
    r->open_after = !sigismember(&after, SIGUSR1) && !sigismember(&after, SIGUSR2);
}

int main(void)
{
    unsigned m = 0, bit = 1, sum = 17;
    struct sigaction act;
    struct round r[ROUNDS];
    sigset_t fn_set = ~(sigset_t)0, mac_set = 0, full = 0;
    int i, install_rc, fn_rc, fn_bad_rc, member_one, member_two, member_full;
    int rc_ok = 1, open_ok = 1, raise_ok = 1, held = 1, blocked_ok = 1, delivered = 1;

    fn_rc = (sigemptyset)(&fn_set);
    fn_rc |= (sigaddset)(&fn_set, SIGUSR1);
    fn_rc |= (sigaddset)(&fn_set, SIGUSR2);
    fn_rc |= (sigdelset)(&fn_set, SIGUSR2);
    fn_rc |= (sigfillset)(&full);
    fn_bad_rc = (sigaddset)(&mac_set, 99);
    member_one = (sigismember)(&fn_set, SIGUSR1);
    member_two = (sigismember)(&fn_set, SIGUSR2);
    member_full = (sigismember)(&full, SIGTERM);
    sigemptyset(&mac_set);
    sigaddset(&mac_set, SIGUSR1);

    act.sa_handler = on_usr1;
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, SIGUSR2);
    act.sa_flags = 0;
    install_rc = sigaction(SIGUSR1, &act, 0);

    for (i = 0; i < ROUNDS; i++) {
        run_round(i, &r[i]);
        if (r[i].block_rc || r[i].query_rc || r[i].unblock_rc || r[i].after_rc)
            rc_ok = 0;
        if (!r[i].open_before || !r[i].open_after)
            open_ok = 0;
        if (r[i].raise_rc)
            raise_ok = 0;
        if (r[i].hits_blocked != 0 || r[i].hits_queried != 0)
            held = 0;
        if (!r[i].blocked_now)
            blocked_ok = 0;
        if (r[i].hits_unblocked != 1)
            delivered = 0;
        sum = sum * 31u + (unsigned)r[i].hits_blocked;
        sum = sum * 31u + (unsigned)r[i].hits_unblocked;
    }

    CK(fn_rc == 0 && fn_set == mac_set && member_one == 1 && member_two == 0 && member_full == 1);
    CK(fn_bad_rc == -1);
    CK(install_rc == 0 && rc_ok);
    CK(raise_ok);
    CK(held);
    CK(blocked_ok);
    CK(delivered && g_hits == ROUNDS);
    CK(open_ok);
    CK(g_query_bad == 0 && g_own_open == 0 && g_extra_open == 0);
    CK(g_bad == 0 && g_skew == 0);

    cb_begin(TAG, m);
    cb_field("hits", (unsigned)g_hits, 0);
    cb_field("sum", sum, 1);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/signal_altstack.c" <<'EOC'
#include "sig_common.h"

#define TAG "signal_altstack"
#define ALT_SIZE ((cb_size)SIGSTKSZ * 4)

static cb_uptr g_alt;
static volatile sig_atomic_t g_on_hits, g_off_hits;
static volatile cb_uptr g_on_at, g_off_at;
static volatile unsigned g_skew;

static void on_alt(int sig)
{
    char here = (char)sig;

    g_on_at = (cb_uptr)&here;
    if (CB_SKEWED())
        g_skew++;
    g_on_hits++;
}

static void on_thread_stack(int sig)
{
    char here = (char)sig;

    g_off_at = (cb_uptr)&here;
    if (CB_SKEWED())
        g_skew++;
    g_off_hits++;
}

int main(void)
{
    unsigned m = 0, bit = 1;
    stack_t first, alt, back, after, off, gone;
    struct sigaction on, plain;
    int r_first, r_set, r_back, r_install, r_raise_on, r_raise_off, r_after, r_off, r_gone, r_raise_gone;
    int on_inside, off_inside, gone_inside;

    first.ss_flags = 0;
    r_first = sigaltstack(0, &first);

    g_alt = (cb_uptr)malloc(ALT_SIZE);
    alt.ss_sp = (void *)g_alt;
    alt.ss_size = ALT_SIZE;
    alt.ss_flags = 0;
    r_set = sigaltstack(&alt, 0);
    back.ss_sp = 0;
    back.ss_size = 0;
    back.ss_flags = -1;
    r_back = sigaltstack(0, &back);

    on.sa_handler = on_alt;
    sigemptyset(&on.sa_mask);
    on.sa_flags = SA_ONSTACK;
    plain.sa_handler = on_thread_stack;
    sigemptyset(&plain.sa_mask);
    plain.sa_flags = 0;
    r_install = sigaction(SIGUSR1, &on, 0) | sigaction(SIGUSR2, &plain, 0);

    at_note(TAG, "raising SIGUSR1 with SA_ONSTACK");
    r_raise_on = raise(SIGUSR1);
    at_note(TAG, "raise returned");
    on_inside = sig_inside(g_on_at, g_alt, ALT_SIZE);
    at_note(TAG, "raising SIGUSR2 without SA_ONSTACK");
    r_raise_off = raise(SIGUSR2);
    at_note(TAG, "raise returned");
    off_inside = sig_inside(g_off_at, g_alt, ALT_SIZE);

    after.ss_flags = -1;
    r_after = sigaltstack(0, &after);
    off.ss_sp = (void *)g_alt;
    off.ss_size = ALT_SIZE;
    off.ss_flags = SS_DISABLE;
    r_off = sigaltstack(&off, 0);
    gone.ss_flags = 0;
    r_gone = sigaltstack(0, &gone);
    at_note(TAG, "raising SIGUSR1 with the alternate stack disabled");
    r_raise_gone = raise(SIGUSR1);
    at_note(TAG, "raise returned");
    gone_inside = sig_inside(g_on_at, g_alt, ALT_SIZE);

    CK(r_first == 0 && (first.ss_flags & SS_DISABLE) != 0 && (first.ss_flags & SS_ONSTACK) == 0);
    CK(g_alt != 0 && r_set == 0 && r_back == 0);
    CK((cb_uptr)back.ss_sp == g_alt && back.ss_size == ALT_SIZE);
    CK((back.ss_flags & (SS_DISABLE | SS_ONSTACK)) == 0);
    CK(r_install == 0 && r_raise_on == 0 && r_raise_off == 0 && r_raise_gone == 0);
    CK(g_on_hits == 2 && g_off_hits == 1);
    CK(on_inside);
    CK(!off_inside);
    CK(r_after == 0 && (after.ss_flags & SS_ONSTACK) == 0);
    CK(r_off == 0 && r_gone == 0 && (gone.ss_flags & SS_DISABLE) != 0);
    CK(!gone_inside);
    CK(g_skew == 0);
    free((void *)g_alt);

    cb_begin(TAG, m);
    cb_field("hits", (unsigned)(g_on_hits + g_off_hits), 0);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/signal_pthread_kill.c" <<'EOC'
#include "sig_common.h"

#define TAG "signal_pthread_kill"

static const char g_token[] = "worker";
static volatile sig_atomic_t g_ready, g_hits;
static volatile int g_worker_waited = -1;
static pthread_t g_worker_self, g_handler_self;
static volatile unsigned g_bad, g_skew;

static void on_usr1(int sig)
{
    g_handler_self = pthread_self();
    if (sig != SIGUSR1)
        g_bad++;
    if (CB_SKEWED())
        g_skew++;
    g_hits++;
}

static void *worker(void *arg)
{
    at_note(TAG, "thread entered");
    g_worker_self = pthread_self();
    g_ready = 1;
    at_note(TAG, "worker waiting for the handler");
    g_worker_waited = sig_wait(&g_hits);
    at_note(TAG, "worker wait returned");
    at_note(TAG, "thread leaving");
    return arg;
}

int main(void)
{
    unsigned m = 0, bit = 1;
    struct sigaction act;
    pthread_t main_self = pthread_self(), main_handler, t;
    void *ret = 0;
    int install_rc, self_rc, self_hits, created, ready = 0, kill_rc = -7, join_rc = -7;

    act.sa_handler = on_usr1;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    install_rc = sigaction(SIGUSR1, &act, 0);

    at_note(TAG, "calling pthread_kill on the main thread");
    self_rc = pthread_kill(main_self, SIGUSR1);
    self_hits = g_hits;
    main_handler = g_handler_self;
    at_note(TAG, "returned from pthread_kill");
    g_hits = 0;
    g_handler_self = 0;

    at_note(TAG, "calling pthread_create");
    created = pthread_create(&t, 0, worker, (void *)g_token) == 0;
    at_note(TAG, "returned from pthread_create");
    if (created) {
        at_note(TAG, "waiting for the worker");
        ready = sig_wait(&g_ready);
        at_note(TAG, "wait returned");
        at_note(TAG, "calling pthread_kill on the worker");
        kill_rc = pthread_kill(t, SIGUSR1);
        at_note(TAG, "returned from pthread_kill");
        at_note(TAG, "calling pthread_join");
        join_rc = pthread_join(t, &ret);
        at_note(TAG, "returned from pthread_join");
    }

    CK(install_rc == 0);
    CK(self_rc == 0 && self_hits == 1 && main_handler == main_self);
    CK(created && ready && join_rc == 0 && ret == g_token);
    CK(kill_rc == 0);
    CK(g_worker_waited == 1 && g_hits == 1);
    CK(created && g_worker_self == t && g_handler_self == t);
    CK(g_handler_self != main_self);
    CK(g_bad == 0 && g_skew == 0);

    cb_begin(TAG, m);
    cb_field("hits", (unsigned)(self_hits + g_hits), 0);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/signal_kill.c" <<'EOC'
#include "sig_common.h"

#define TAG "signal_kill"

static volatile sig_atomic_t g_hits;
static pthread_t g_handler_self;
static volatile unsigned g_bad, g_skew;

static void on_usr1(int sig)
{
    g_handler_self = pthread_self();
    if (sig != SIGUSR1)
        g_bad++;
    if (CB_SKEWED())
        g_skew++;
    g_hits++;
}

int main(void)
{
    unsigned m = 0, bit = 1;
    struct sigaction act;
    pthread_t main_self = pthread_self();
    int install_rc, probe_rc, probe_hits, kill_rc, waited, settled;

    act.sa_handler = on_usr1;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    install_rc = sigaction(SIGUSR1, &act, 0);

    probe_rc = kill(getpid(), 0);
    probe_hits = g_hits;
    at_note(TAG, "calling kill");
    kill_rc = kill(getpid(), SIGUSR1);
    at_note(TAG, "returned from kill");
    at_note(TAG, "waiting for the handler");
    waited = sig_wait(&g_hits);
    at_note(TAG, "wait returned");
    getpid();
    getpid();
    settled = g_hits;

    CK(install_rc == 0);
    CK(probe_rc == 0 && probe_hits == 0);
    CK(kill_rc == 0);
    CK(waited && settled == 1);
    CK(g_handler_self == main_self);
    CK(g_bad == 0 && g_skew == 0);

    cb_begin(TAG, m);
    cb_field("hits", (unsigned)settled, 0);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/signal_errors.c" <<'EOC'
#include "sig_common.h"

#define TAG "signal_errors"
#define NACT 6
#define NRAISE 4

struct attempt {
    const char *key;
    int sig;
    int rc;
    int err;
};

static volatile sig_atomic_t g_hits;
static char g_small[MINSIGSTKSZ];

static void on_any(int sig)
{
    (void)sig;
    g_hits++;
}

static void sig_result(const struct attempt *a, int with_errno)
{
    sig_int(a->key, a->rc);
    if (with_errno) {
        cb_str("/");
        cb_dec((unsigned)a->err);
    }
}

int main(void)
{
    unsigned m = 0, bit = 1;
    struct sigaction act, old;
    stack_t small;
    struct attempt sa[NACT] = {
        { "kill", SIGKILL, 0, 0 },
        { "stop", SIGSTOP, 0, 0 },
        { "zero", 0, 0, 0 },
        { "big", 200, 0, 0 },
        { "nsig", NSIG, 0, 0 },
        { "query_kill", SIGKILL, 0, 0 },
    };
    struct attempt st = { "sigaltstack_small", 0, 0, 0 };
    struct attempt ra[NRAISE] = {
        { "raise_big", 200, 0, 0 },
        { "kill_big", 200, 0, 0 },
        { "raise_zero", 0, 0, 0 },
        { "kill_zero", 0, 0, 0 },
    };
    struct attempt pk[3] = {
        { "pthread_kill_big", 200, 0, 0 },
        { "pthread_kill_nsig", NSIG, 0, 0 },
        { "pthread_kill_zero", 0, 0, 0 },
    };
    int i;

    act.sa_handler = on_any;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    for (i = 0; i < NACT; i++) {
        *__error() = 0;
        if (i == NACT - 1)
            sa[i].rc = sigaction(sa[i].sig, 0, &old);
        else
            sa[i].rc = sigaction(sa[i].sig, &act, 0);
        sa[i].err = *__error();
    }
    small.ss_sp = g_small;
    small.ss_size = MINSIGSTKSZ - 1;
    small.ss_flags = 0;
    *__error() = 0;
    st.rc = sigaltstack(&small, 0);
    st.err = *__error();
    for (i = 0; i < NRAISE; i++) {
        *__error() = 0;
        if (i % 2 == 0)
            ra[i].rc = raise(ra[i].sig);
        else
            ra[i].rc = kill(getpid(), ra[i].sig);
        ra[i].err = *__error();
    }
    for (i = 0; i < 3; i++)
        pk[i].rc = pthread_kill(pthread_self(), pk[i].sig);

    for (i = 0; i < NACT; i++)
        CK(sa[i].rc == -1 && sa[i].err == EINVAL);
    CK(st.rc == -1 && st.err == ENOMEM);
    CK(ra[0].rc == -1 && ra[0].err == EINVAL && ra[1].rc == -1 && ra[1].err == EINVAL);
    CK(ra[2].rc == 0 && ra[3].rc == 0);
    CK(pk[0].rc == EINVAL && pk[1].rc == EINVAL);
    CK(pk[2].rc == 0);
    CK(g_hits == 0);

    cb_begin(TAG, m);
    for (i = 0; i < NACT; i++)
        sig_result(&sa[i], 1);
    sig_result(&st, 1);
    cb_end();
    cb_len = 0;
    cb_str(TAG);
    for (i = 0; i < NRAISE; i++)
        sig_result(&ra[i], ra[i].rc != 0);
    for (i = 0; i < 3; i++)
        sig_result(&pk[i], 0);
    cb_end();
    return m != 0;
}
EOC

    cat > "$TMP/signal_default.c" <<'EOC'
#include "sig_common.h"

int main(void)
{
    write(1, "signal_default raising SIGWINCH\n", 32);
    raise(SIGWINCH);
    write(1, "signal_default raising SIGTERM\n", 31);
    raise(SIGTERM);
    write(1, "signal_default survived\n", 24);
    return 3;
}
EOC

    cat > "$TMP/signal_handler_bridge.c" <<'EOC'
#include "sig_common.h"

#define TAG "signal_handler_bridge"
#define RAISES 4

static const char *const g_words[] = {
    "handler", "bridged strlen", "write from a signal handler", "",
};
#define NW ((unsigned)(sizeof g_words / sizeof g_words[0]))

static volatile sig_atomic_t g_usr1, g_usr2, g_depth;
static volatile unsigned g_len_bad, g_copy_bad, g_cmp_bad, g_write_bad, g_nested_bad, g_bad, g_skew;
static volatile unsigned g_sum = 17;

static unsigned own_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void on_usr2(int sig)
{
    char line[] = "signal_handler_bridge nested\n";
    cb_size n = strlen(line);

    g_usr2++;
    if (sig != SIGUSR2)
        g_bad++;
    if (g_depth != 1)
        g_nested_bad++;
    if (write(1, line, n) != (long)n)
        g_write_bad++;
    if (CB_SKEWED())
        g_skew++;
}

static void on_usr1(int sig)
{
    char buf[64];
    const char *w;
    cb_size n;
    unsigned k, c;
    int before, rc;

    g_depth++;
    k = (unsigned)g_usr1++;
    if (sig != SIGUSR1)
        g_bad++;
    if (CB_SKEWED())
        g_skew++;
    w = g_words[k % NW];
    n = strlen(w);
    if (n != own_len(w) || n > 40) {
        g_len_bad++;
        g_depth--;
        return;
    }
    if (memcpy(buf, "handler ", 8) != buf || memcpy(buf + 8, w, n + 1) != buf + 8)
        g_copy_bad++;
    for (c = 0; c <= n; c++)
        if (buf[8 + c] != w[c])
            g_copy_bad++;
    if (strcmp(buf + 8, w) != 0 || strcmp(buf, w) == 0)
        g_cmp_bad++;
    buf[8 + n] = '\n';
    if (write(1, buf, 9 + n) != (long)(9 + n))
        g_write_bad++;
    for (c = 0; c < 9 + n; c++)
        g_sum = g_sum * 31u + (unsigned char)buf[c];
    before = g_usr2;
    rc = raise(SIGUSR2);
    if (rc != 0 || g_usr2 != before + 1)
        g_nested_bad++;
    g_depth--;
}

int main(void)
{
    unsigned m = 0, bit = 1, i;
    struct sigaction one, two;
    int install_rc, rc_bad = 0, seen_bad = 0, rc;

    one.sa_handler = on_usr1;
    sigemptyset(&one.sa_mask);
    one.sa_flags = 0;
    two.sa_handler = on_usr2;
    sigemptyset(&two.sa_mask);
    two.sa_flags = 0;
    install_rc = sigaction(SIGUSR1, &one, 0) | sigaction(SIGUSR2, &two, 0);

    for (i = 0; i < RAISES; i++) {
        at_note(TAG, "raising SIGUSR1");
        rc = raise(SIGUSR1);
        at_note(TAG, "raise returned");
        if (rc != 0)
            rc_bad++;
        if (g_usr1 != (int)i + 1 || g_usr2 != (int)i + 1 || g_depth != 0)
            seen_bad++;
    }

    CK(install_rc == 0 && rc_bad == 0);
    CK(seen_bad == 0 && g_usr1 == RAISES && g_usr2 == RAISES);
    CK(g_len_bad == 0);
    CK(g_copy_bad == 0);
    CK(g_cmp_bad == 0);
    CK(g_write_bad == 0);
    CK(g_nested_bad == 0);
    CK(g_bad == 0 && g_skew == 0);

    cb_begin(TAG, m);
    cb_field("hits", (unsigned)(g_usr1 + g_usr2), 0);
    cb_field("sum", g_sum, 1);
    cb_end();
    return m != 0;
}
EOC

    for name in signal_sigaction signal_signal signal_mask signal_altstack \
                signal_pthread_kill signal_kill signal_errors signal_default \
                signal_handler_bridge; do
        clang -arch x86_64 -std=c11 -O1 -fno-builtin \
                -o "$TMP/$name" "$TMP/$name.c" >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            signal_sigaction) SIG_ACTION_BIN="$TMP/$name" ;;
            signal_signal) SIG_SIGNAL_BIN="$TMP/$name" ;;
            signal_mask) SIG_MASK_BIN="$TMP/$name" ;;
            signal_altstack) SIG_ALTSTACK_BIN="$TMP/$name" ;;
            signal_pthread_kill) SIG_PTKILL_BIN="$TMP/$name" ;;
            signal_kill) SIG_KILL_BIN="$TMP/$name" ;;
            signal_errors) SIG_ERRORS_BIN="$TMP/$name" ;;
            signal_default) SIG_DEFAULT_BIN="$TMP/$name" ;;
            signal_handler_bridge) SIG_HANDLER_BIN="$TMP/$name" ;;
        esac
    done
}

build_cf_fixtures() {
    local name

    cat > "$TMP/cf_common.h" <<EOC
#include <CoreFoundation/CoreFoundation.h>
#include "cb_common.h"

#define CF_RUN_SECS ${CF_RUN_SECS}

static unsigned cf_failed, cf_group;

static void cf_note(const char *tag, const char *what)
{
    char b[96];
    cb_size n = 0;

    while (*tag && n < 40)
        b[n++] = *tag++;
    b[n++] = ':';
    b[n++] = ' ';
    while (*what && n < sizeof b - 1)
        b[n++] = *what++;
    b[n++] = '\n';
    write(2, b, n);
}

static unsigned cf_fold(unsigned h, unsigned v)
{
    h ^= v;
    h *= 0x01000193u;
    return h ^ (h >> 15);
}

static CFStringRef cf_make(const char *utf8)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, utf8, kCFStringEncodingUTF8);
}

static int cf_text_is(CFStringRef s, const char *want)
{
    char buf[256];
    int got;

    if (s == 0)
        return 0;
    got = CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    return got == true && strcmp(buf, want) == 0;
}

static int cf_equals_text(CFTypeRef v, const char *utf8)
{
    CFStringRef t = cf_make(utf8);
    int eq;

    if (t == 0)
        return 0;
    eq = v != 0 ? CFEqual(v, t) : -1;
    CFRelease(t);
    return eq == true;
}

static void cf_begin(const char *tag, const char *group, unsigned mask)
{
    cb_len = 0;
    cb_str(tag);
    cb_str(" ");
    cb_str(group);
    if (mask == 0) {
        cb_str(" ok");
    } else {
        cb_str(" bad:");
        cb_hex(mask);
        cf_failed |= 1u << cf_group;
    }
    cf_group++;
}

static void cf_long(const char *key, long v)
{
    cb_str(" ");
    cb_str(key);
    cb_str("=");
    if (v < 0) {
        cb_str("-");
        cb_dec((unsigned)-v);
    } else {
        cb_dec((unsigned)v);
    }
}

static int cf_summary(const char *tag)
{
    cb_begin(tag, cf_failed);
    cb_end();
    return cf_failed != 0;
}
EOC

    cat > "$TMP/cf_basic.c" <<'EOC'
#include "cf_common.h"

#define TAG "cf_basic"

static const char k_text[] = "Hello \xc5\xbdlu\xc5\xa5ou\xc4\x8dk\xc3\xbd \xe2\x82\xac \xf0\x9f\x98\x80";
static const unsigned k_units[] = {
    0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x17d, 0x6c, 0x75, 0x165,
    0x6f, 0x75, 0x10d, 0x6b, 0xfd, 0x20, 0x20ac, 0x20, 0xd83d, 0xde00
};
#define N_UNITS (long)(sizeof k_units / sizeof k_units[0])
static const unsigned char k_utf16le[] = {
    0x7d, 0x01, 0x6c, 0x00, 0x75, 0x00, 0x65, 0x01, 0x6f, 0x00,
    0x75, 0x00, 0x0d, 0x01, 0x6b, 0x00, 0xfd, 0x00
};
static const unsigned char k_order[] = { 0x7d, 0x01, 0x6c, 0x00 };

static void group_string(void)
{
    unsigned m = 0, bit = 1, units = 0;
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, k_text, kCFStringEncodingUTF8);
    CFStringRef w, h, host, wire;
    char buf[64], tiny[8];
    long len = -1, i, units_bad = 0;
    int got_all = -1, got_tiny = -1, got_ascii = -1, eq_w = -1, eq_h = -1;
    long len_w = -1, len_h = -1, len_host = -1, len_wire = -1;
    unsigned c_host = 0, c_wire = 0;

    if (s != 0) {
        len = CFStringGetLength(s);
        for (i = 0; i < N_UNITS && i < len; i++) {
            unsigned c = CFStringGetCharacterAtIndex(s, i);
            units = cf_fold(units, c);
            if (c != k_units[i])
                units_bad++;
        }
        got_all = CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
        got_tiny = CFStringGetCString(s, tiny, sizeof tiny, kCFStringEncodingUTF8);
        got_ascii = CFStringGetCString(s, buf + 32, 32, kCFStringEncodingASCII);
    }
    w = CFStringCreateWithBytes(0, k_utf16le, sizeof k_utf16le, kCFStringEncodingUTF16LE, false);
    if (w != 0) {
        len_w = CFStringGetLength(w);
        eq_w = CFEqual(w, CFSTR("\xc5\xbdlu\xc5\xa5ou\xc4\x8dk\xc3\xbd"));
    }
    h = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)k_text, 5, kCFStringEncodingUTF8, false);
    if (h != 0) {
        len_h = CFStringGetLength(h);
        eq_h = CFEqual(h, CFSTR("Hello"));
    }
    host = CFStringCreateWithBytes(0, k_order, sizeof k_order, kCFStringEncodingUnicode, false);
    wire = CFStringCreateWithBytes(0, k_order, sizeof k_order, kCFStringEncodingUnicode, true);
    if (host != 0) {
        len_host = CFStringGetLength(host);
        c_host = CFStringGetCharacterAtIndex(host, 0);
    }
    if (wire != 0) {
        len_wire = CFStringGetLength(wire);
        c_wire = CFStringGetCharacterAtIndex(wire, 0);
    }

    CK(s != 0 && CFGetTypeID(s) == CFStringGetTypeID());
    CK(len == N_UNITS);
    CK(units_bad == 0);
    CK(got_all == true && strcmp(buf, k_text) == 0);
    CK(got_tiny == false);
    CK(got_ascii == false);
    CK(eq_w == true && len_w == 9);
    CK(eq_h == true && len_h == 5);
    CK(len_host == 2 && c_host == 0x17d && len_wire == 2 && c_wire == 0x7d01);

    cf_begin(TAG, "string", m);
    cf_long("len", len);
    cb_field("units", units, 1);
    cb_end();
    if (s)
        CFRelease(s);
    if (w)
        CFRelease(w);
    if (h)
        CFRelease(h);
    if (host)
        CFRelease(host);
    if (wire)
        CFRelease(wire);
}

static void group_literal(void)
{
    unsigned m = 0, bit = 1;
    CFStringRef lit = CFSTR("Hello");
    CFStringRef wide = CFSTR("\xc5\xbdlu\xc5\xa5ou\xc4\x8dk\xc3\xbd");
    CFStringRef made = cf_make("Hello");
    CFStringRef made_wide = cf_make("\xc5\xbdlu\xc5\xa5ou\xc4\x8dk\xc3\xbd");
    CFStringRef copy = CFStringCreateCopy(0, lit);
    CFTypeRef again;
    const char *ptr;
    int eq1, eq2, eq_case, eq_wide, eq_copy;
    unsigned c0, c8;
    long wlen;

    eq1 = CFEqual(lit, made);
    eq2 = CFEqual(made, lit);
    eq_case = CFEqual(lit, CFSTR("hello"));
    eq_wide = CFEqual(wide, made_wide);
    eq_copy = CFEqual(copy, lit);
    again = CFRetain(lit);
    CFRelease(lit);
    wlen = CFStringGetLength(wide);
    c0 = CFStringGetCharacterAtIndex(wide, 0);
    c8 = CFStringGetCharacterAtIndex(wide, 8);
    ptr = CFStringGetCStringPtr(lit, kCFStringEncodingUTF8);

    CK(CFGetTypeID(lit) == CFStringGetTypeID() && CFGetTypeID(wide) == CFStringGetTypeID());
    CK(made != 0 && eq1 == true && eq2 == true);
    CK(CFHash(lit) == CFHash(made));
    CK(eq_case == false);
    CK(again == lit);
    CK(wlen == 9 && c0 == 0x17d && c8 == 0xfd);
    CK(made_wide != 0 && eq_wide == true && CFHash(wide) == CFHash(made_wide));
    CK(ptr == 0 || strcmp(ptr, "Hello") == 0);
    CK(copy != 0 && eq_copy == true && CFGetTypeID(copy) == CFStringGetTypeID());

    cf_begin(TAG, "literal", m);
    cb_end();
    if (made)
        CFRelease(made);
    if (made_wide)
        CFRelease(made_wide);
    if (copy)
        CFRelease(copy);
}

static void group_mutable(void)
{
    unsigned m = 0, bit = 1;
    CFMutableStringRef ms = CFStringCreateMutable(0, 0);
    CFMutableStringRef mc = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, CFSTR("Hello"));
    CFStringRef suffix = cf_make("lo\xc3\xbd");
    long len = -1, c_lt, c_gt, c_ci, c_cs, c_num, c_lex, maxlen;
    unsigned last = 0;
    int pre_yes = -1, pre_no = -1, suf_yes = -1, suf_no = -1;
    int iv_neg, iv_max, iv_space;
    double dv, dv_exp;

    if (ms != 0) {
        CFStringAppendCString(ms, "abc", kCFStringEncodingUTF8);
        CFStringAppend(ms, CFSTR("Hello"));
        CFStringAppendCString(ms, "\xc3\xbd", kCFStringEncodingUTF8);
        len = CFStringGetLength(ms);
        last = CFStringGetCharacterAtIndex(ms, 8);
        pre_yes = CFStringHasPrefix(ms, CFSTR("abcH"));
        pre_no = CFStringHasPrefix(ms, CFSTR("Hello"));
        suf_yes = CFStringHasSuffix(ms, suffix);
        suf_no = CFStringHasSuffix(ms, CFSTR("abc"));
    }
    if (mc != 0)
        CFStringAppendCString(mc, " world", kCFStringEncodingUTF8);
    c_lt = CFStringCompare(CFSTR("apple"), CFSTR("banana"), 0);
    c_gt = CFStringCompare(CFSTR("banana"), CFSTR("apple"), 0);
    c_ci = CFStringCompare(CFSTR("HELLO"), CFSTR("hello"), kCFCompareCaseInsensitive);
    c_cs = CFStringCompare(CFSTR("HELLO"), CFSTR("hello"), 0);
    c_num = CFStringCompare(CFSTR("file10"), CFSTR("file9"), kCFCompareNumerically);
    c_lex = CFStringCompare(CFSTR("file10"), CFSTR("file9"), 0);
    iv_neg = CFStringGetIntValue(CFSTR("-123456"));
    iv_max = CFStringGetIntValue(CFSTR("2147483647"));
    iv_space = CFStringGetIntValue(CFSTR("  42"));
    dv = CFStringGetDoubleValue(CFSTR("3.25"));
    dv_exp = CFStringGetDoubleValue(CFSTR("-0.5e3"));
    maxlen = CFStringGetMaximumSizeForEncoding(10, kCFStringEncodingUTF8);

    CK(ms != 0 && len == 9);
    CK(last == 0xfd && cf_equals_text(ms, "abcHello\xc3\xbd"));
    CK(c_lt == kCFCompareLessThan && c_lt == -1 && c_gt == kCFCompareGreaterThan);
    CK(c_ci == kCFCompareEqualTo && c_cs == -1);
    CK(c_num == 1 && c_lex == -1);
    CK(pre_yes == true && pre_no == false);
    CK(suffix != 0 && suf_yes == true && suf_no == false);
    CK(iv_neg == -123456 && iv_max == 2147483647 && iv_space == 42);
    CK(dv == 3.25 && dv_exp == -500.0);
    CK(mc != 0 && cf_text_is(mc, "Hello world") && cf_text_is(CFSTR("Hello"), "Hello"));
    CK(maxlen >= 30);

    cf_begin(TAG, "mutable", m);
    cf_long("len", len);
    cf_long("max", maxlen);
    cb_end();
    if (ms)
        CFRelease(ms);
    if (mc)
        CFRelease(mc);
    if (suffix)
        CFRelease(suffix);
}

static void group_split(void)
{
    unsigned m = 0, bit = 1;
    CFArrayRef parts = CFStringCreateArrayBySeparatingStrings(0, CFSTR("a,bb,,ccc"), CFSTR(","));
    CFArrayRef whole = CFStringCreateArrayBySeparatingStrings(0, CFSTR("a,bb"), CFSTR(";"));
    CFStringRef joined = 0;
    long n = -1, n_whole = -1;

    if (parts != 0) {
        n = CFArrayGetCount(parts);
        joined = CFStringCreateByCombiningStrings(0, parts, CFSTR("::"));
    }
    if (whole != 0)
        n_whole = CFArrayGetCount(whole);

    CK(parts != 0 && CFGetTypeID(parts) == CFArrayGetTypeID() && n == 4);
    CK(n == 4 && cf_equals_text(CFArrayGetValueAtIndex(parts, 0), "a") &&
       cf_equals_text(CFArrayGetValueAtIndex(parts, 1), "bb") &&
       cf_equals_text(CFArrayGetValueAtIndex(parts, 2), "") &&
       cf_equals_text(CFArrayGetValueAtIndex(parts, 3), "ccc"));
    CK(joined != 0 && cf_text_is(joined, "a::bb::::ccc"));
    CK(n_whole == 1 && cf_equals_text(CFArrayGetValueAtIndex(whole, 0), "a,bb"));

    cf_begin(TAG, "split", m);
    cf_long("n", n);
    cb_end();
    if (parts)
        CFRelease(parts);
    if (whole)
        CFRelease(whole);
    if (joined)
        CFRelease(joined);
}

static void group_array(void)
{
    unsigned m = 0, bit = 1;
    static const UInt8 bytes[4] = { 1, 2, 3, 4 };
    long long big = 0x1234567890abcdefll;
    CFTypeRef v[3];
    CFArrayRef arr, copy;
    CFMutableArrayRef mut, mcopy;
    long rc0, rc1, rc0_in, rc1_in, rc0_back, rc1_back, n = -1, i, n_mut, n_mcopy;
    long rc0_mut, rc1_mut, rc0_clear, rc1_clear;
    int identity = 1, twice = 1, order = 1, eq_copy, eq_mcopy, eq_other;
    const void *p, *q;

    v[0] = CFStringCreateMutableCopy(0, 0, CFSTR("zero"));
    v[1] = CFDataCreate(0, bytes, sizeof bytes);
    v[2] = CFNumberCreate(0, kCFNumberSInt64Type, &big);
    rc0 = CFGetRetainCount(v[0]);
    rc1 = CFGetRetainCount(v[1]);
    arr = CFArrayCreate(kCFAllocatorDefault, v, 3, &kCFTypeArrayCallBacks);
    rc0_in = CFGetRetainCount(v[0]);
    rc1_in = CFGetRetainCount(v[1]);
    if (arr != 0) {
        n = CFArrayGetCount(arr);
        for (i = 0; i < 3 && i < n; i++) {
            p = CFArrayGetValueAtIndex(arr, i);
            q = CFArrayGetValueAtIndex(arr, i);
            if (p != v[i])
                identity = 0;
            if (p != q)
                twice = 0;
        }
    }

    mut = CFArrayCreateMutable(0, 0, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(mut, v[0]);
    CFArrayAppendValue(mut, v[1]);
    CFArrayInsertValueAtIndex(mut, 0, v[2]);
    if (CFArrayGetValueAtIndex(mut, 0) != v[2] || CFArrayGetValueAtIndex(mut, 1) != v[0] ||
        CFArrayGetValueAtIndex(mut, 2) != v[1])
        order = 0;
    CFArraySetValueAtIndex(mut, 1, v[1]);
    CFArrayRemoveValueAtIndex(mut, 0);
    n_mut = CFArrayGetCount(mut);
    if (n_mut != 2 || CFArrayGetValueAtIndex(mut, 0) != v[1] || CFArrayGetValueAtIndex(mut, 1) != v[1])
        order = 0;
    rc0_mut = CFGetRetainCount(v[0]) - rc0_in;
    rc1_mut = CFGetRetainCount(v[1]) - rc1_in;
    CFArrayRemoveAllValues(mut);
    rc0_clear = CFGetRetainCount(v[0]) - rc0_in;
    rc1_clear = CFGetRetainCount(v[1]) - rc1_in;

    copy = CFArrayCreateCopy(0, arr);
    mcopy = CFArrayCreateMutableCopy(0, 0, arr);
    eq_copy = CFEqual(copy, arr);
    CFArrayAppendValue(mcopy, v[0]);
    n_mcopy = CFArrayGetCount(mcopy);
    eq_mcopy = CFEqual(mcopy, arr);
    CFArrayRemoveValueAtIndex(mcopy, 3);
    CFArraySetValueAtIndex(mcopy, 2, v[0]);
    eq_other = CFEqual(mcopy, arr);

    CK(arr != 0 && CFGetTypeID(arr) == CFArrayGetTypeID() && n == 3);
    CK(identity);
    CK(twice);
    CK(rc0_in - rc0 == 1 && rc1_in - rc1 == 1);
    CK(mut != 0 && order && n_mut == 2 && CFArrayGetCount(mut) == 0);
    CK(rc0_mut == 0 && rc1_mut == 2 && rc0_clear == 0 && rc1_clear == 0);
    CK(copy != 0 && mcopy != 0 && eq_copy == true && n_mcopy == 4 && eq_mcopy == false && eq_other == false);
    CFRelease(copy);
    CFRelease(mcopy);
    CFRelease(arr);
    rc0_back = CFGetRetainCount(v[0]);
    rc1_back = CFGetRetainCount(v[1]);
    CK(rc0_back == rc0 && rc1_back == rc1);

    cf_begin(TAG, "array", m);
    cf_long("n", n);
    cf_long("held", rc1_in - rc1);
    cb_end();
    CFRelease(mut);
    CFRelease(v[0]);
    CFRelease(v[1]);
    CFRelease(v[2]);
}

static void group_dict(void)
{
    unsigned m = 0, bit = 1;
    static const UInt8 bytes[3] = { 9, 8, 7 };
    long long raw[3] = { 0x100000001ll, 0x200000002ll, 0x300000003ll };
    CFTypeRef keys[3] = { CFSTR("one"), CFSTR("two"), CFSTR("three") };
    CFTypeRef vals[3], kbuf[3], vbuf[3];
    CFDictionaryRef d, dcopy;
    CFMutableDictionaryRef md, mdcopy;
    CFMutableStringRef key;
    CFStringRef two;
    CFDataRef data;
    CFTypeRef out;
    long n = -1, n_md_after, n_md_empty, rc_data, rc_in, rc_out, n_mdcopy, i, j;
    int has_one, has_four, present, absent, pairs = 1, eq_dcopy, eq_mdcopy;
    const void *got_two, *got_key, *got_changed, *after_add, *after_set;

    for (i = 0; i < 3; i++)
        vals[i] = CFNumberCreate(0, kCFNumberSInt64Type, &raw[i]);
    d = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 3,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    two = cf_make("two");
    n = CFDictionaryGetCount(d);
    got_two = CFDictionaryGetValue(d, two);
    has_one = CFDictionaryContainsKey(d, CFSTR("one"));
    has_four = CFDictionaryContainsKey(d, CFSTR("four"));
    out = 0;
    present = CFDictionaryGetValueIfPresent(d, CFSTR("three"), &out);
    CK(d != 0 && CFGetTypeID(d) == CFDictionaryGetTypeID() && n == 3);
    CK(two != 0 && got_two == vals[1]);
    CK(has_one == true && has_four == false);
    CK(present == true && out == vals[2]);
    out = kCFNull;
    absent = CFDictionaryGetValueIfPresent(d, CFSTR("four"), &out);
    CK(absent == false && out == kCFNull);

    CFDictionaryGetKeysAndValues(d, kbuf, vbuf);
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++)
            if (CFEqual(kbuf[i], keys[j]) == true)
                break;
        if (j == 3 || vbuf[i] != vals[j])
            pairs = 0;
    }
    CK(pairs);

    md = CFDictionaryCreateMutable(0, 0, &kCFCopyStringDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks);
    key = CFStringCreateMutableCopy(0, 0, CFSTR("key"));
    CFDictionarySetValue(md, key, vals[0]);
    CFStringAppendCString(key, "-changed", kCFStringEncodingUTF8);
    got_key = CFDictionaryGetValue(md, CFSTR("key"));
    got_changed = CFDictionaryGetValue(md, key);
    CFDictionaryAddValue(md, CFSTR("key"), vals[1]);
    after_add = CFDictionaryGetValue(md, CFSTR("key"));
    CFDictionarySetValue(md, CFSTR("key"), vals[1]);
    after_set = CFDictionaryGetValue(md, CFSTR("key"));
    CK(md != 0 && key != 0 && got_key == vals[0] && got_changed == 0);
    CK(after_add == vals[0] && after_set == vals[1]);

    data = CFDataCreate(0, bytes, sizeof bytes);
    rc_data = CFGetRetainCount(data);
    CFDictionarySetValue(md, CFSTR("data"), data);
    rc_in = CFGetRetainCount(data);
    n_md_after = CFDictionaryGetCount(md);
    CFDictionaryRemoveValue(md, CFSTR("data"));
    CFDictionaryRemoveValue(md, CFSTR("key"));
    rc_out = CFGetRetainCount(data);
    n_md_empty = CFDictionaryGetCount(md);
    CK(rc_in - rc_data == 1 && rc_out == rc_data && n_md_after == 2 && n_md_empty == 0);

    dcopy = CFDictionaryCreateCopy(0, d);
    mdcopy = CFDictionaryCreateMutableCopy(0, 0, d);
    eq_dcopy = CFEqual(dcopy, d);
    CFDictionarySetValue(mdcopy, CFSTR("four"), vals[0]);
    n_mdcopy = CFDictionaryGetCount(mdcopy);
    eq_mdcopy = CFEqual(mdcopy, d);
    CK(dcopy != 0 && mdcopy != 0 && eq_dcopy == true && n_mdcopy == 4 && eq_mdcopy == false);

    cf_begin(TAG, "dict", m);
    cf_long("n", n);
    cf_long("held", rc_in - rc_data);
    cb_end();
    CFRelease(dcopy);
    CFRelease(mdcopy);
    CFRelease(md);
    CFRelease(key);
    CFRelease(data);
    CFRelease(two);
    CFRelease(d);
    for (i = 0; i < 3; i++)
        CFRelease(vals[i]);
}

static void group_number(void)
{
    unsigned m = 0, bit = 1;
    SInt32 i32 = -123456789, o32 = 0, lossy = 0;
    SInt64 i64 = -0x123456789abcdefll, o64 = 0;
    Float64 f64 = -2.718281828459045, of64 = 0, same = -2.718281828459045, qnan = 0, pinf = 0, ninf = 0;
    CFNumberRef n32 = CFNumberCreate(0, kCFNumberSInt32Type, &i32);
    CFNumberRef n64 = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &i64);
    CFNumberRef nd = CFNumberCreate(0, kCFNumberFloat64Type, &f64);
    CFNumberRef nd2 = CFNumberCreate(0, kCFNumberFloat64Type, &same);
    int g32, g64, gd, glossy, gnan, gpinf, gninf, bt, bf;
    long t32, t64, td, c_gt, c_lt, c_eq, c_inf;

    g32 = CFNumberGetValue(n32, kCFNumberSInt32Type, &o32);
    g64 = CFNumberGetValue(n64, kCFNumberSInt64Type, &o64);
    gd = CFNumberGetValue(nd, kCFNumberFloat64Type, &of64);
    glossy = CFNumberGetValue(n64, kCFNumberSInt32Type, &lossy);
    t32 = CFNumberGetType(n32);
    t64 = CFNumberGetType(n64);
    td = CFNumberGetType(nd);
    c_gt = CFNumberCompare(n32, n64, 0);
    c_lt = CFNumberCompare(n64, n32, 0);
    c_eq = CFNumberCompare(nd, nd2, 0);
    c_inf = CFNumberCompare(kCFNumberPositiveInfinity, nd, 0);
    gnan = CFNumberGetValue(kCFNumberNaN, kCFNumberFloat64Type, &qnan);
    gpinf = CFNumberGetValue(kCFNumberPositiveInfinity, kCFNumberFloat64Type, &pinf);
    gninf = CFNumberGetValue(kCFNumberNegativeInfinity, kCFNumberFloat64Type, &ninf);
    bt = CFBooleanGetValue(kCFBooleanTrue);
    bf = CFBooleanGetValue(kCFBooleanFalse);

    CK(n32 != 0 && g32 == true && o32 == i32 && CFGetTypeID(n32) == CFNumberGetTypeID());
    CK(n64 != 0 && g64 == true && o64 == i64);
    CK(nd != 0 && gd == true && of64 == f64);
    CK(t32 == kCFNumberSInt32Type && t64 == kCFNumberSInt64Type && td == kCFNumberFloat64Type);
    CK(glossy == false);
    CK(c_gt == kCFCompareGreaterThan && c_lt == -1 && c_eq == kCFCompareEqualTo && c_inf == 1);
    CK(gnan == true && qnan != qnan && gpinf == true && pinf > 1e308 && gninf == true && ninf < -1e308);
    CK(bt == true && bf == false);
    CK(kCFBooleanTrue != kCFBooleanFalse && CFGetTypeID(kCFBooleanTrue) == CFBooleanGetTypeID() &&
       CFBooleanGetTypeID() != CFNumberGetTypeID());
    CK(kCFNull != 0 && CFEqual(kCFNull, kCFNull) == true && CFGetTypeID(kCFNull) != CFBooleanGetTypeID());

    cf_begin(TAG, "number", m);
    cf_long("types", t32 * 100 + t64 * 10 + td);
    cb_field("lo", (unsigned)o64, 1);
    cb_end();
    if (n32)
        CFRelease(n32);
    if (n64)
        CFRelease(n64);
    if (nd)
        CFRelease(nd);
    if (nd2)
        CFRelease(nd2);
}

static void group_data(void)
{
    unsigned m = 0, bit = 1, sum = 0;
    UInt8 bytes[64];
    CFDataRef d, empty;
    CFStringRef desc;
    const UInt8 *p;
    CFTypeRef again;
    long len = -1, rc0, rc1, rc2, i, diff = 0, empty_len = -1;

    for (i = 0; i < 64; i++)
        bytes[i] = (UInt8)(i * 37 + 11);
    d = CFDataCreate(kCFAllocatorDefault, bytes, sizeof bytes);
    empty = CFDataCreate(0, 0, 0);
    len = CFDataGetLength(d);
    p = CFDataGetBytePtr(d);
    for (i = 0; p != 0 && i < 64 && i < len; i++) {
        if (p[i] != bytes[i])
            diff++;
        sum = cf_fold(sum, p[i]);
    }
    rc0 = CFGetRetainCount(d);
    again = CFRetain(d);
    rc1 = CFGetRetainCount(d);
    CFRelease(d);
    rc2 = CFGetRetainCount(d);
    if (empty != 0)
        empty_len = CFDataGetLength(empty);
    desc = CFCopyDescription(d);

    CK(d != 0 && CFGetTypeID(d) == CFDataGetTypeID() && len == 64);
    CK(p != 0 && p != bytes && diff == 0);
    CK(again == d && rc1 - rc0 == 1 && rc2 == rc0);
    CK(empty != 0 && empty_len == 0);
    CK(CFGetAllocator(d) == kCFAllocatorSystemDefault);
    CK(desc != 0 && CFGetTypeID(desc) == CFStringGetTypeID() && CFStringGetLength(desc) > 0);

    cf_begin(TAG, "data", m);
    cf_long("len", len);
    cb_field("sum", sum, 1);
    cb_end();
    if (desc)
        CFRelease(desc);
    if (empty)
        CFRelease(empty);
    if (d)
        CFRelease(d);
}

int main(void)
{
    cf_note(TAG, "string");
    group_string();
    cf_note(TAG, "literal");
    group_literal();
    cf_note(TAG, "mutable");
    group_mutable();
    cf_note(TAG, "split");
    group_split();
    cf_note(TAG, "array");
    group_array();
    cf_note(TAG, "dict");
    group_dict();
    cf_note(TAG, "number");
    group_number();
    cf_note(TAG, "data");
    group_data();
    return cf_summary(TAG);
}
EOC

    cat > "$TMP/cf_callbacks.c" <<'EOC'
#include "cf_common.h"

#define TAG "cf_callbacks"
#define N_ITEMS 8
#define N_HELD 6
#define N_KEYS 8
#define IN(p, arr) ((cb_uptr)(p) - (cb_uptr)(arr) < sizeof (arr) && ((cb_uptr)(p) - (cb_uptr)(arr)) % sizeof (arr)[0] == 0)

#if defined(__x86_64__)
typedef unsigned long long wide_bool;
#define WIDE_BOOL(v) ((wide_bool)0x5a00000000ull | 0xdeadbe00u | (wide_bool)((v) != 0))
#else
typedef Boolean wide_bool;
#define WIDE_BOOL(v) ((Boolean)((v) != 0))
#endif

struct item {
    unsigned key;
    int refs;
    struct item *stand_in;
};

static struct item g_items[N_ITEMS], g_twins[N_HELD], g_proxy[1];
static unsigned g_retain, g_release, g_equal, g_desc, g_fold, g_hash, g_key_equal;
static unsigned g_skew, g_bad_value, g_bad_order, g_bad_alloc;
static CFAllocatorRef g_alloc;
static int g_alloc_set;

static int is_item(const void *p)
{
    return IN(p, g_items) || IN(p, g_twins) || IN(p, g_proxy);
}

static void seen_alloc(CFAllocatorRef alloc)
{
    if (!g_alloc_set) {
        g_alloc = alloc;
        g_alloc_set = 1;
    } else if (alloc != g_alloc) {
        g_bad_alloc++;
    }
}

static const void *item_retain(CFAllocatorRef alloc, const void *value)
{
    struct item *it = (struct item *)value;
    struct item *out;

    g_retain++;
    if (CB_SKEWED())
        g_skew++;
    seen_alloc(alloc);
    if (!is_item(value)) {
        g_bad_value++;
        return value;
    }
    out = it->stand_in ? it->stand_in : it;
    out->refs++;
    return out;
}

static void item_release(CFAllocatorRef alloc, const void *value)
{
    g_release++;
    if (CB_SKEWED())
        g_skew++;
    seen_alloc(alloc);
    if (!is_item(value)) {
        g_bad_value++;
        return;
    }
    ((struct item *)value)->refs--;
}

static CFStringRef item_desc(const void *value)
{
    char text[16] = "item-";
    unsigned key;

    g_desc++;
    if (CB_SKEWED())
        g_skew++;
    if (!is_item(value)) {
        g_bad_value++;
        return 0;
    }
    key = ((const struct item *)value)->key;
    text[5] = (char)('0' + key / 10 % 10);
    text[6] = (char)('0' + key % 10);
    text[7] = 0;
    return CFStringCreateWithCString(0, text, kCFStringEncodingUTF8);
}

static wide_bool item_equal(const void *a, const void *b)
{
    g_equal++;
    if (CB_SKEWED())
        g_skew++;
    if (!is_item(a) || !is_item(b)) {
        g_bad_value++;
        return WIDE_BOOL(0);
    }
    if (IN(a, g_twins) || !IN(b, g_twins))
        g_bad_order++;
    return WIDE_BOOL(((const struct item *)a)->key == ((const struct item *)b)->key);
}

static int contains(const char *hay, const char *needle)
{
    const char *h, *n;

    for (; *hay; hay++) {
        for (h = hay, n = needle; *n && *h == *n; h++, n++)
            ;
        if (*n == 0)
            return 1;
    }
    return 0;
}

static void group_array(void)
{
    unsigned m = 0, bit = 1;
    CFArrayCallBacks cb = { 0, item_retain, item_release, item_desc, (CFArrayEqualCallBack)item_equal };
    CFMutableArrayRef a, mcopy;
    CFArrayRef twins, odd;
    CFStringRef desc;
    const void *tv[N_HELD], *ov[N_HELD];
    static const unsigned want_keys[N_HELD] = { 16, 17, 102, 13, 14, 15 };
    char text[1024];
    long n_after = -1, n_copy = -1, i;
    int keys_ok = 1, proxy_seen = 0, refs_mid = 1, refs_end = 1, eq_twins, eq_odd, got_text = 0;
    int refs_copy = 1;
    unsigned equal_twins, equal_odd, retain_app, release_app, retain_mid, release_mid, desc_calls, freed;
    CFAllocatorRef alloc = 0;

    for (i = 0; i < N_ITEMS; i++)
        g_items[i].key = 10 + (unsigned)i;
    g_proxy[0].key = 102;
    g_items[2].stand_in = g_proxy;

    a = CFArrayCreateMutable(kCFAllocatorMalloc, 0, &cb);
    for (i = 0; i < N_HELD; i++)
        CFArrayAppendValue(a, &g_items[i]);
    retain_app = g_retain;
    release_app = g_release;
    if (CFArrayGetValueAtIndex(a, 2) == g_proxy && g_proxy[0].refs == 1 && g_items[2].refs == 0)
        proxy_seen = 1;
    CFArrayRemoveValueAtIndex(a, 1);
    CFArraySetValueAtIndex(a, 0, &g_items[6]);
    CFArrayInsertValueAtIndex(a, 1, &g_items[7]);
    n_after = CFArrayGetCount(a);
    for (i = 0; i < N_HELD && i < n_after; i++) {
        const struct item *it = CFArrayGetValueAtIndex(a, i);
        if (!is_item(it) || it->key != want_keys[i] || CFArrayGetValueAtIndex(a, i) != it)
            keys_ok = 0;
    }
    retain_mid = g_retain;
    release_mid = g_release;
    if (g_items[0].refs != 0 || g_items[1].refs != 0 || g_items[2].refs != 0 || g_proxy[0].refs != 1)
        refs_mid = 0;
    for (i = 3; i < N_ITEMS; i++)
        if (g_items[i].refs != 1)
            refs_mid = 0;
    alloc = CFGetAllocator(a);

    for (i = 0; i < N_HELD; i++) {
        g_twins[i].key = want_keys[i];
        tv[i] = &g_twins[i];
        ov[i] = &g_twins[i];
    }
    twins = CFArrayCreate(kCFAllocatorMalloc, tv, N_HELD, &cb);
    g_equal = 0;
    eq_twins = CFEqual(a, twins);
    equal_twins = g_equal;
    g_twins[3].key = 99;
    odd = CFArrayCreate(kCFAllocatorMalloc, ov, N_HELD, &cb);
    g_equal = 0;
    eq_odd = CFEqual(a, odd);
    equal_odd = g_equal;
    g_twins[3].key = want_keys[3];

    desc = CFCopyDescription(a);
    desc_calls = g_desc;
    if (desc != 0)
        got_text = CFStringGetCString(desc, text, sizeof text, kCFStringEncodingUTF8);

    mcopy = CFArrayCreateMutableCopy(kCFAllocatorMalloc, 0, a);
    if (mcopy != 0)
        n_copy = CFArrayGetCount(mcopy);
    if (g_proxy[0].refs != 2 || g_items[3].refs != 2 || g_items[7].refs != 2)
        refs_copy = 0;

    CK(a != 0 && n_after == N_HELD && keys_ok);
    CK(proxy_seen);
    CK(retain_app == N_HELD && release_app == 0 && retain_mid == N_HELD + 2 && release_mid == 2 && refs_mid);
    CK(g_alloc_set && g_alloc == kCFAllocatorMalloc && alloc == kCFAllocatorMalloc && g_bad_alloc == 0);
    CK(twins != 0 && eq_twins == true && equal_twins == N_HELD);
    CK(odd != 0 && eq_odd == false && equal_odd == 4);
    CK(g_bad_order == 0);
    CK(desc != 0 && desc_calls == N_HELD && got_text == true &&
       contains(text, "item-16") && contains(text, "item-02") && contains(text, "item-15"));
    CK(mcopy != 0 && n_copy == N_HELD && refs_copy);

    if (desc)
        CFRelease(desc);
    CFRelease(mcopy);
    CFRelease(odd);
    CFRelease(twins);
    CFArrayRemoveValueAtIndex(a, 0);
    freed = g_release;
    CFRelease(a);
    freed = g_release - freed;
    for (i = 0; i < N_ITEMS; i++)
        if (g_items[i].refs != 0)
            refs_end = 0;
    for (i = 0; i < N_HELD; i++)
        if (g_twins[i].refs != 0)
            refs_end = 0;
    if (g_proxy[0].refs != 0)
        refs_end = 0;
    CK(refs_end && freed == N_HELD - 1 && g_retain == g_release && g_bad_value == 0 && g_bad_alloc == 0 &&
       g_skew == 0);

    cf_begin(TAG, "array", m);
    cb_field("retain", g_retain, 0);
    cb_field("release", g_release, 0);
    cb_field("equal", equal_twins + equal_odd, 0);
    cb_field("desc", desc_calls, 0);
    cb_end();
}

static wide_bool fold_equal(const void *a, const void *b)
{
    g_fold++;
    if (CB_SKEWED())
        g_skew++;
    return WIDE_BOOL(CFStringCompare(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo);
}

static void group_mixed(void)
{
    unsigned m = 0, bit = 1;
    CFArrayCallBacks mixed = kCFTypeArrayCallBacks;
    CFMutableStringRef s[4];
    CFTypeRef xv[2], zv[2];
    CFArrayRef x, z;
    CFMutableArrayRef y;
    CFStringRef desc;
    long rc[4], rc_in[4], rc_out[4], i;
    int eq_xy, eq_xz, eq_other, standard, got_text = 0, held = 1, back = 1;
    unsigned fold_xy, fold_xz, skew0 = g_skew;
    char text[512];

    standard = mixed.version == 0 && mixed.retain != 0 && mixed.release != 0 &&
               mixed.copyDescription != 0 && mixed.equal != 0;
    mixed.equal = (CFArrayEqualCallBack)fold_equal;
    s[0] = CFStringCreateMutableCopy(0, 0, CFSTR("Alpha"));
    s[1] = CFStringCreateMutableCopy(0, 0, CFSTR("beta"));
    s[2] = CFStringCreateMutableCopy(0, 0, CFSTR("ALPHA"));
    s[3] = CFStringCreateMutableCopy(0, 0, CFSTR("BETA"));
    for (i = 0; i < 4; i++)
        rc[i] = CFGetRetainCount(s[i]);
    xv[0] = s[0];
    xv[1] = s[1];
    zv[0] = s[2];
    zv[1] = s[3];
    x = CFArrayCreate(0, xv, 2, &mixed);
    y = CFArrayCreateMutable(0, 0, &mixed);
    CFArrayAppendValue(y, s[2]);
    CFArrayAppendValue(y, s[3]);
    z = CFArrayCreate(0, zv, 2, &kCFTypeArrayCallBacks);
    for (i = 0; i < 4; i++) {
        rc_in[i] = CFGetRetainCount(s[i]);
        if (rc_in[i] - rc[i] != (i < 2 ? 1 : 2))
            held = 0;
    }
    g_fold = 0;
    eq_xy = CFEqual(x, y);
    fold_xy = g_fold;
    eq_xz = CFEqual(x, z);
    fold_xz = g_fold - fold_xy;
    CFStringAppendCString(s[3], "x", kCFStringEncodingUTF8);
    eq_other = CFEqual(y, x);
    desc = CFCopyDescription(x);
    if (desc != 0)
        got_text = CFStringGetCString(desc, text, sizeof text, kCFStringEncodingUTF8);

    CK(standard);
    CK(x != 0 && y != 0 && z != 0 && CFArrayGetCount(x) == 2 && CFArrayGetCount(y) == 2 &&
       CFArrayGetValueAtIndex(x, 0) == s[0] && CFArrayGetValueAtIndex(y, 1) == s[3]);
    CK(held);
    CK(eq_xy == true && fold_xy == 2);
    CK(eq_xz == false && fold_xz == 0);
    CK(eq_other == false);
    CK(desc != 0 && got_text == true && contains(text, "Alpha") && contains(text, "beta"));

    if (desc)
        CFRelease(desc);
    CFRelease(x);
    CFRelease(y);
    CFRelease(z);
    for (i = 0; i < 4; i++) {
        rc_out[i] = CFGetRetainCount(s[i]);
        if (rc_out[i] != rc[i])
            back = 0;
    }
    CK(back && g_skew == skew0);

    cf_begin(TAG, "mixed", m);
    cb_field("equal", g_fold, 0);
    cb_end();
    for (i = 0; i < 4; i++)
        CFRelease(s[i]);
}

static const char *const k_names[N_KEYS] = {
    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel"
};

static int key_index(const void *key)
{
    int i;

    for (i = 0; i < N_KEYS; i++)
        if (strcmp(key, k_names[i]) == 0)
            return i;
    return -1;
}

static CFHashCode cstr_hash_value(const char *s)
{
    unsigned long long h = 0xcbf29ce484222325ull;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 0x100000001b3ull;
    }
    return (CFHashCode)h;
}

static CFHashCode cstr_hash(const void *key)
{
    g_hash++;
    if (CB_SKEWED())
        g_skew++;
    return cstr_hash_value(key);
}

static wide_bool cstr_equal(const void *a, const void *b)
{
    const char *x = a, *y = b;

    g_key_equal++;
    if (CB_SKEWED())
        g_skew++;
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return WIDE_BOOL(*x == *y);
}

static const void *value_for(int i)
{
    return (const void *)(cb_uptr)(((cb_uptr)(i + 1) << 36) | (cb_uptr)(0x1000u + (unsigned)i));
}

struct tally {
    unsigned calls;
    unsigned sum;
    unsigned bad;
    unsigned skew;
    struct tally *self;
};

static void apply_cstr(const void *key, const void *value, void *context)
{
    struct tally *t = context;
    int i = key_index(key);

    if (CB_SKEWED())
        t->skew++;
    if (t->self != t || i < 0 || value != value_for(i)) {
        t->bad++;
        return;
    }
    t->calls++;
    t->sum += cf_fold((unsigned)(cstr_hash_value(key) >> 32), (unsigned)((cb_uptr)value >> 36));
}

static void apply_cf(const void *key, const void *value, void *context)
{
    struct tally *t = context;
    SInt64 v = 0;
    int got;

    if (CB_SKEWED())
        t->skew++;
    got = CFNumberGetValue(value, kCFNumberSInt64Type, &v);
    if (t->self != t || got != true || CFGetTypeID(key) != CFStringGetTypeID()) {
        t->bad++;
        return;
    }
    t->calls++;
    t->sum += cf_fold((unsigned)CFStringGetLength(key), (unsigned)(v >> 32) ^ (unsigned)v);
}

static void group_dict(void)
{
    unsigned m = 0, bit = 1, order = 0;
    CFDictionaryKeyCallBacks kcb = { 0, 0, 0, 0, (CFDictionaryEqualCallBack)cstr_equal, cstr_hash };
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(0, 0, &kcb, 0);
    const void *keys[N_KEYS], *vals[N_KEYS];
    const void *out;
    char probe[16];
    long n_full = -1, n_after = -1, i, j;
    int found = 1, has_zulu, present, gone, pairs = 1;
    unsigned hashes, key_equals, skew0 = g_skew;

    for (i = 0; i < N_KEYS; i++)
        CFDictionaryAddValue(d, k_names[i], value_for((int)i));
    n_full = CFDictionaryGetCount(d);
    g_key_equal = 0;
    for (i = 0; i < N_KEYS; i++) {
        for (j = 0; k_names[i][j]; j++)
            probe[j] = k_names[i][j];
        probe[j] = 0;
        if (CFDictionaryGetValue(d, probe) != value_for((int)i))
            found = 0;
    }
    key_equals = g_key_equal;
    has_zulu = CFDictionaryContainsKey(d, "zulu");
    out = 0;
    present = CFDictionaryGetValueIfPresent(d, "echo", &out);
    CFDictionaryRemoveValue(d, "bravo");
    n_after = CFDictionaryGetCount(d);
    gone = CFDictionaryContainsKey(d, "bravo");
    hashes = g_hash;

    CFDictionaryGetKeysAndValues(d, keys, vals);
    for (i = 0; i < n_after && i < N_KEYS; i++) {
        j = key_index(keys[i]);
        if (j < 0 || j == 1 || keys[i] != k_names[j] || vals[i] != value_for((int)j))
            pairs = 0;
        order = cf_fold(order, (unsigned)j);
    }

    CK(d != 0 && n_full == N_KEYS);
    CK(found && key_equals >= N_KEYS);
    CK(has_zulu == false);
    CK(present == true && out == value_for(4));
    CK(n_after == N_KEYS - 1 && gone == false);
    CK(pairs && hashes >= N_KEYS);
    CK(g_skew == skew0);

    cf_begin(TAG, "dict", m);
    cf_long("n", n_after);
    cb_field("order", order, 1);
    cb_end();
    CFRelease(d);
}

static void group_apply(void)
{
    unsigned m = 0, bit = 1, want_cstr = 0, want_cf = 0, skew0 = g_skew;
    CFDictionaryKeyCallBacks kcb = { 0, 0, 0, 0, (CFDictionaryEqualCallBack)cstr_equal, cstr_hash };
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(0, 0, &kcb, 0);
    CFMutableDictionaryRef nd = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
    struct tally t_cstr = { 0, 0, 0, 0, 0 }, t_cf = { 0, 0, 0, 0, 0 };
    SInt64 v;
    CFStringRef key;
    CFNumberRef num;
    int i;

    t_cstr.self = &t_cstr;
    t_cf.self = &t_cf;
    for (i = 0; i < N_KEYS; i++) {
        CFDictionarySetValue(d, k_names[i], value_for(i));
        want_cstr += cf_fold((unsigned)(cstr_hash_value(k_names[i]) >> 32), (unsigned)i + 1);
        v = ((SInt64)(i + 3) << 40) | (SInt64)(i * 7 + 1);
        key = cf_make(k_names[i]);
        num = CFNumberCreate(0, kCFNumberSInt64Type, &v);
        CFDictionarySetValue(nd, key, num);
        want_cf += cf_fold((unsigned)CFStringGetLength(key), (unsigned)(v >> 32) ^ (unsigned)v);
        CFRelease(key);
        CFRelease(num);
    }
    CFDictionaryApplyFunction(d, apply_cstr, &t_cstr);
    CFDictionaryApplyFunction(nd, apply_cf, &t_cf);

    CK(d != 0 && t_cstr.calls == N_KEYS && t_cstr.bad == 0);
    CK(t_cstr.sum == want_cstr);
    CK(nd != 0 && t_cf.calls == N_KEYS && t_cf.bad == 0);
    CK(t_cf.sum == want_cf);
    CK(t_cstr.skew == 0 && t_cf.skew == 0 && g_skew == skew0);

    cf_begin(TAG, "apply", m);
    cb_field("calls", t_cstr.calls + t_cf.calls, 0);
    cb_field("sum", t_cstr.sum ^ t_cf.sum, 1);
    cb_end();
    CFRelease(d);
    CFRelease(nd);
}

int main(void)
{
    cf_note(TAG, "array");
    group_array();
    cf_note(TAG, "mixed");
    group_mixed();
    cf_note(TAG, "dict");
    group_dict();
    cf_note(TAG, "apply");
    group_apply();
    return cf_summary(TAG);
}
EOC

    cat > "$TMP/cf_runloop.c" <<'EOC'
#include "cf_common.h"

#define TAG "cf_runloop"
#define FIRES 3
#define INTERVAL 0.01

struct counts {
    unsigned retain;
    unsigned release;
    unsigned bad;
};

static struct counts g_timer_info, g_observer_info, g_source_info;
static CFRunLoopRef g_rl;
static CFRunLoopTimerRef g_timer;
static CFRunLoopObserverRef g_observer;
static CFRunLoopSourceRef g_source;
static unsigned g_fires, g_performs, g_schedules, g_cancels, g_entries, g_waits, g_other;
static unsigned g_skew, g_bad_info, g_bad_timer, g_bad_observer, g_bad_source, g_bad_rl, g_bad_mode;
static unsigned g_perform_lag, g_first_activity;
static unsigned g_invalid_in_callout;

static const void *info_retain(const void *info)
{
    struct counts *c = (struct counts *)info;

    if (CB_SKEWED())
        g_skew++;
    if (c == &g_timer_info || c == &g_observer_info || c == &g_source_info)
        c->retain++;
    else
        g_bad_info++;
    return info;
}

static void info_release(const void *info)
{
    struct counts *c = (struct counts *)info;

    if (CB_SKEWED())
        g_skew++;
    if (c == &g_timer_info || c == &g_observer_info || c == &g_source_info)
        c->release++;
    else
        g_bad_info++;
}

static void on_timer(CFRunLoopTimerRef timer, void *info)
{
    int valid;

    g_fires++;
    cf_note(TAG, "timer fired");
    if (CB_SKEWED())
        g_skew++;
    if (timer != g_timer || info != &g_timer_info)
        g_bad_timer++;
    if (CFRunLoopGetCurrent() != g_rl)
        g_bad_rl++;
    if (g_performs != g_fires - 1)
        g_perform_lag++;
    if (g_fires < FIRES) {
        valid = CFRunLoopTimerIsValid(timer);
        if (valid != true)
            g_invalid_in_callout++;
        CFRunLoopSourceSignal(g_source);
        CFRunLoopWakeUp(g_rl);
    } else if (g_fires == FIRES) {
        cf_note(TAG, "stopping");
        CFRunLoopStop(g_rl);
    }
}

static void on_observer(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    if (CB_SKEWED())
        g_skew++;
    if (observer != g_observer || info != &g_observer_info)
        g_bad_observer++;
    if (g_entries + g_waits + g_other == 0)
        g_first_activity = (unsigned)activity;
    if (activity == kCFRunLoopEntry)
        g_entries++;
    else if (activity == kCFRunLoopBeforeWaiting)
        g_waits++;
    else
        g_other++;
}

static void on_schedule(void *info, CFRunLoopRef rl, CFStringRef mode)
{
    if (CB_SKEWED())
        g_skew++;
    g_schedules++;
    if (info != &g_source_info)
        g_bad_source++;
    if (rl != g_rl)
        g_bad_rl++;
    if (mode == 0 || CFEqual(mode, kCFRunLoopDefaultMode) != true)
        g_bad_mode++;
}

static void on_cancel(void *info, CFRunLoopRef rl, CFStringRef mode)
{
    if (CB_SKEWED())
        g_skew++;
    g_cancels++;
    if (info != &g_source_info)
        g_bad_source++;
    if (rl != g_rl)
        g_bad_rl++;
    if (mode == 0 || CFEqual(mode, kCFRunLoopDefaultMode) != true)
        g_bad_mode++;
}

static void on_perform(void *info)
{
    if (CB_SKEWED())
        g_skew++;
    g_performs++;
    cf_note(TAG, "source performed");
    if (info != &g_source_info)
        g_bad_source++;
    if (CFRunLoopGetCurrent() != g_rl)
        g_bad_rl++;
}

int main(void)
{
    unsigned m = 0, bit = 1;
    CFRunLoopTimerContext tctx = { 0, &g_timer_info, info_retain, info_release, 0 };
    CFRunLoopObserverContext octx = { 0, &g_observer_info, info_retain, info_release, 0 };
    CFRunLoopSourceContext sctx = { 0, &g_source_info, info_retain, info_release, 0, 0, 0,
                                    on_schedule, on_cancel, on_perform };
    CFAbsoluteTime now, fire, next;
    int rc, valid_before, valid_removed, valid_after, retained_early;
    unsigned schedules_added, cancels_removed, cancels_invalidated, timer_released;

    g_rl = CFRunLoopGetCurrent();
    now = CFAbsoluteTimeGetCurrent();
    g_timer = CFRunLoopTimerCreate(0, now + 1000.0, INTERVAL, 0, 0, on_timer, &tctx);
    g_observer = CFRunLoopObserverCreate(0, kCFRunLoopEntry | kCFRunLoopBeforeWaiting, true, 0,
                                         on_observer, &octx);
    g_source = CFRunLoopSourceCreate(0, 0, &sctx);
    retained_early = g_timer_info.retain == 1 && g_observer_info.retain == 1 && g_source_info.retain == 1 &&
                     g_timer_info.release == 0 && g_observer_info.release == 0 && g_source_info.release == 0;

    CFRunLoopAddObserver(g_rl, g_observer, kCFRunLoopDefaultMode);
    CFRunLoopAddSource(g_rl, g_source, kCFRunLoopDefaultMode);
    schedules_added = g_schedules;
    CFRunLoopAddTimer(g_rl, g_timer, kCFRunLoopDefaultMode);
    fire = CFAbsoluteTimeGetCurrent() + INTERVAL;
    CFRunLoopTimerSetNextFireDate(g_timer, fire);
    next = CFRunLoopTimerGetNextFireDate(g_timer);

    cf_note(TAG, "calling CFRunLoopRunInMode");
    rc = CFRunLoopRunInMode(kCFRunLoopDefaultMode, CF_RUN_SECS, false);
    cf_note(TAG, "returned from CFRunLoopRunInMode");

    valid_before = CFRunLoopTimerIsValid(g_timer);
    CFRunLoopRemoveTimer(g_rl, g_timer, kCFRunLoopDefaultMode);
    valid_removed = CFRunLoopTimerIsValid(g_timer);
    CFRunLoopTimerInvalidate(g_timer);
    valid_after = CFRunLoopTimerIsValid(g_timer);
    timer_released = g_timer_info.release;
    CFRunLoopRemoveObserver(g_rl, g_observer, kCFRunLoopDefaultMode);
    CFRunLoopObserverInvalidate(g_observer);
    CFRunLoopRemoveSource(g_rl, g_source, kCFRunLoopDefaultMode);
    cancels_removed = g_cancels;
    CFRunLoopSourceInvalidate(g_source);
    cancels_invalidated = g_cancels;
    CFRelease(g_timer);
    CFRelease(g_observer);
    CFRelease(g_source);

    CK(g_rl != 0 && g_rl == CFRunLoopGetMain());
    CK(g_timer != 0 && g_observer != 0 && g_source != 0 && retained_early);
    CK(next == fire);
    CK(rc == kCFRunLoopRunStopped);
    CK(g_fires == FIRES && g_bad_timer == 0 && g_invalid_in_callout == 0);
    CK(g_performs == FIRES - 1 && g_perform_lag == 0);
    CK(g_entries == 1 && g_first_activity == kCFRunLoopEntry && g_waits >= 1 && g_other == 0 &&
       g_bad_observer == 0);
    CK(schedules_added == 1 && g_schedules == 1 && cancels_removed == 1 && cancels_invalidated == 1);
    CK(g_bad_source == 0 && g_bad_rl == 0 && g_bad_mode == 0);
    CK(valid_before == true && valid_removed == true && valid_after == false);
    CK(g_timer_info.retain == FIRES + 1 && g_timer_info.release == FIRES + 1 && timer_released == FIRES + 1);
    CK(g_observer_info.retain == 1 && g_observer_info.release == 1);
    CK(g_source_info.retain == 1 && g_source_info.release == 1 && g_bad_info == 0);
    CK(g_skew == 0);

    cf_begin(TAG, "loop", m);
    cf_long("rc", rc);
    cb_field("fires", g_fires, 0);
    cb_field("performs", g_performs, 0);
    cb_field("entries", g_entries, 0);
    cb_field("retains", g_timer_info.retain + g_observer_info.retain + g_source_info.retain, 0);
    cb_end();
    return cf_summary(TAG);
}
EOC

    for name in cf_basic cf_callbacks cf_runloop; do
        clang -arch x86_64 -std=c11 -O1 -fno-builtin \
                -o "$TMP/$name" "$TMP/$name.c" -framework CoreFoundation >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            cf_basic) CF_BASIC_BIN="$TMP/$name" ;;
            cf_callbacks) CF_CALLBACKS_BIN="$TMP/$name" ;;
            cf_runloop) CF_RUNLOOP_BIN="$TMP/$name" ;;
        esac
        clang -arch arm64 -std=c11 -O1 -fno-builtin \
                -o "$TMP/$name.arm64" "$TMP/$name.c" -framework CoreFoundation >"$TMP/$name.arm64.cc.log" 2>&1 || continue
        case $name in
            cf_basic) CF_BASIC_ARM64="$TMP/$name.arm64" ;;
            cf_callbacks) CF_CALLBACKS_ARM64="$TMP/$name.arm64" ;;
            cf_runloop) CF_RUNLOOP_ARM64="$TMP/$name.arm64" ;;
        esac
    done
}

build_objc_fixtures() {
    local name arch sfx

    cat > "$TMP/objc_common.h" <<'EOC'
#import <Foundation/Foundation.h>
#include "cf_common.h"

static int objc_tagged(id o)
{
    cb_uptr p = (cb_uptr)(__bridge void *)o;

    return (p & 1) != 0 || (p >> 63) != 0;
}

static int objc_text_is(NSString *s, const char *want)
{
    const char *got = s != nil ? [s UTF8String] : 0;

    return got != 0 && strcmp(got, want) == 0;
}
EOC

    cat > "$TMP/objc_foundation.m" <<'EOC'
#include "objc_common.h"

#define TAG "objc_foundation"

static const char k_text[] = "\xc5\xbdlu\xc5\xa5ou\xc4\x8dk\xc3\xbd k\xc5\xaf\xc5\x88 \xe2\x82\xac";
static const char k_upper[] = "\xc5\xbdLU\xc5\xa4OU\xc4\x8cK\xc3\x9d K\xc5\xae\xc5\x87 \xe2\x82\xac";
static const char k_word[] = "k\xc5\xaf\xc5\x88";
static const char k_long[] = "a string far too long to fit in a tagged pointer";

static void group_string(void)
{
    unsigned m = 0, bit = 1;
    NSString *s = [NSString stringWithUTF8String:k_text];
    NSString *lit = @"\xc5\xbdlu\xc5\xa5ou\xc4\x8dk\xc3\xbd k\xc5\xaf\xc5\x88 \xe2\x82\xac";
    NSString *word = [NSString stringWithUTF8String:k_word];
    NSString *sub;
    NSRange found = { 1, 1 }, missing = { 1, 1 };
    unsigned c0 = 0, c11 = 0, c14 = 0;
    long len = -1;
    int eq_made = -1, eq_lit = -1, eq_other = -1;

    if (s != nil) {
        len = (long)[s length];
        c0 = [s characterAtIndex:0];
        c11 = [s characterAtIndex:11];
        c14 = [s characterAtIndex:14];
        eq_made = [s isEqual:lit];
        eq_lit = [lit isEqual:s];
        eq_other = [s isEqual:word];
    }
    sub = [s substringWithRange:NSMakeRange(10, 3)];
    found = [s rangeOfString:word];
    missing = [s rangeOfString:@"xyz"];

    CK(s != nil && len == 15);
    CK(c0 == 0x17d && c11 == 0x16f && c14 == 0x20ac);
    CK(objc_text_is(s, k_text));
    CK(eq_made == YES && eq_lit == YES && eq_other == NO);
    CK([s hash] == [lit hash] && [lit length] == 15);
    CK(objc_text_is([s uppercaseString], k_upper) && objc_text_is(s, k_text));
    CK(objc_text_is(sub, k_word) && [sub isEqual:word] == YES && [sub length] == 3);
    CK(found.location == 10 && found.length == 3);
    CK(missing.location == NSNotFound && missing.length == 0);

    cf_begin(TAG, "string", m);
    cf_long("len", len);
    cf_long("at", (long)found.location);
    cb_end();
}

static void group_tagged(void)
{
    unsigned m = 0, bit = 1;
    NSString *t = [NSString stringWithUTF8String:"short"];
    NSString *l = [NSString stringWithUTF8String:k_long];
    NSString *tu = [t uppercaseString];
    NSString *joined = [t stringByAppendingString:l];
    NSRange rt = [t rangeOfString:@"or"], rl = [l rangeOfString:@"tagged"];

    CK(t != nil && objc_tagged(t));
    CK(l != nil && !objc_tagged(l));
    CK([t length] == 5 && [t characterAtIndex:4] == 't' && objc_text_is(t, "short"));
    CK([t isEqual:@"short"] == YES && [@"short" isEqual:t] == YES && [t hash] == [@"short" hash]);
    CK(objc_text_is(tu, "SHORT") && [tu isEqual:@"SHORT"] == YES);
    CK(rt.location == 2 && rt.length == 2);
    CK([l length] == sizeof k_long - 1 && objc_text_is(l, k_long));
    CK([l isEqual:@"a string far too long to fit in a tagged pointer"] == YES &&
       [l hash] == [@"a string far too long to fit in a tagged pointer" hash]);
    CK(rl.location == 34 && rl.length == 6);
    CK(joined != nil && !objc_tagged(joined) && [joined length] == 5 + sizeof k_long - 1 &&
       [joined hasPrefix:t] == YES && [joined hasSuffix:l] == YES);

    cf_begin(TAG, "tagged", m);
    cf_long("short", (long)[t length]);
    cf_long("long", (long)[l length]);
    cb_end();
}

static void group_number(void)
{
    unsigned m = 0, bit = 1;
    NSNumber *i = [NSNumber numberWithInt:-123456];
    NSNumber *d = [NSNumber numberWithDouble:-2.75];
    NSNumber *big = [NSNumber numberWithLongLong:0x123456789abcdefll];
    NSNumber *lit_i = @42;
    NSNumber *lit_d = @3.5;
    NSComparisonResult lt = [i compare:lit_i], gt = [lit_i compare:i], same = [lit_d compare:[NSNumber numberWithDouble:3.5]];
    NSComparisonResult mixed = [lit_i compare:lit_d];

    CK(i != nil && [i intValue] == -123456 && [i doubleValue] == -123456.0);
    CK(d != nil && [d doubleValue] == -2.75 && [d intValue] == -2);
    CK(big != nil && [big longLongValue] == 0x123456789abcdefll);
    CK([lit_i intValue] == 42 && [lit_i doubleValue] == 42.0);
    CK([lit_d doubleValue] == 3.5 && [lit_d intValue] == 3);
    CK([lit_i class] == NSClassFromString(@"NSConstantIntegerNumber") &&
       [lit_d class] == NSClassFromString(@"NSConstantDoubleNumber"));
    CK([lit_i isKindOfClass:[NSNumber class]] == YES && [lit_d isKindOfClass:[NSNumber class]] == YES);
    CK([lit_i isEqual:[NSNumber numberWithInt:42]] == YES && [[NSNumber numberWithInt:42] isEqual:lit_i] == YES);
    CK(lt == NSOrderedAscending && lt == -1 && gt == NSOrderedDescending && gt == 1);
    CK(same == NSOrderedSame && mixed == NSOrderedDescending);

    cf_begin(TAG, "number", m);
    cf_long("int", [lit_i intValue]);
    cf_long("cmp", (long)lt);
    cb_end();
}

static void group_collection(void)
{
    unsigned m = 0, bit = 1;
    NSString *e0 = [NSString stringWithUTF8String:"alpha"];
    NSNumber *e1 = [NSNumber numberWithInt:7];
    NSString *e2 = [NSString stringWithUTF8String:k_long];
    NSString *k1 = [NSString stringWithUTF8String:"key-one"];
    NSArray *arr = @[e0, e1, e2];
    NSDictionary *dict = @{ @"k0" : e0, k1 : e1, @"a longer key for the third value" : e2 };
    NSString *probe = [NSString stringWithUTF8String:k_long];
    id first, second, a0, a2;
    long n = -1, nd = -1;

    if (arr != nil)
        n = (long)[arr count];
    if (dict != nil)
        nd = (long)[dict count];
    a0 = [arr objectAtIndex:0];
    a2 = [arr objectAtIndex:2];
    first = [dict objectForKey:[NSString stringWithUTF8String:"a longer key for the third value"]];
    second = [dict objectForKey:@"a longer key for the third value"];

    CK(arr != nil && n == 3);
    CK(a0 == e0 && [arr objectAtIndex:1] == e1 && a2 == e2);
    CK([arr objectAtIndex:2] == a2 && arr[0] == a0 && [a2 isEqual:e2] == YES);
    CK([arr containsObject:probe] == YES && probe != e2 && [arr containsObject:@"absent"] == NO);
    CK([arr indexOfObject:e1] == 1 && [arr indexOfObject:@"absent"] == NSNotFound);
    CK(dict != nil && nd == 3);
    CK([dict objectForKey:@"k0"] == e0 && [dict objectForKey:[NSString stringWithUTF8String:"key-one"]] == e1);
    CK(first == e2 && second == e2 && [first isEqual:probe] == YES);
    CK([dict objectForKey:@"absent"] == nil && dict[k1] == e1);

    cf_begin(TAG, "collection", m);
    cf_long("n", n);
    cf_long("nd", nd);
    cb_end();
}

static NSString *g_desc;

static void group_describe(void)
{
    unsigned m = 0, bit = 1;
    NSArray *arr = @[[NSString stringWithUTF8String:"alpha"], [NSNumber numberWithInt:7],
                     [NSString stringWithUTF8String:"two words"], [NSNumber numberWithDouble:3.5]];
    NSString *desc = [arr description];
    const char *text = desc != nil ? [desc UTF8String] : 0;

    CK(desc != nil && text != 0 && [desc length] > 0);
    CK([desc rangeOfString:@"two words"].location != NSNotFound);
    g_desc = desc;

    cf_begin(TAG, "describe", m);
    cf_long("len", desc != nil ? (long)[desc length] : -1);
    cb_end();
}

static void group_range(void)
{
    unsigned m = 0, bit = 1;
    NSRange in = NSMakeRange(123456789012ul, 42);
    NSValue *v = [NSValue valueWithRange:in];
    NSRange out = [v rangeValue];
    NSString *text = NSStringFromRange(out);
    NSRange parsed = NSRangeFromString(@"{7, 9}");
    NSRange cut = NSIntersectionRange(NSMakeRange(10, 20), NSMakeRange(25, 30));
    NSRange both = NSUnionRange(NSMakeRange(10, 20), NSMakeRange(25, 30));

    CK(v != nil && out.location == 123456789012ul && out.length == 42);
    CK(objc_text_is(text, "{123456789012, 42}"));
    CK(parsed.location == 7 && parsed.length == 9);
    CK(cut.location == 25 && cut.length == 5 && both.location == 10 && both.length == 45);

    cf_begin(TAG, "range", m);
    cb_str(" ");
    if (text != nil)
        cb_str([text UTF8String]);
    cb_end();
}

int main(void)
{
    int rc;

    @autoreleasepool {
        cf_note(TAG, "string");
        group_string();
        cf_note(TAG, "tagged");
        group_tagged();
        cf_note(TAG, "number");
        group_number();
        cf_note(TAG, "collection");
        group_collection();
        cf_note(TAG, "describe");
        group_describe();
        cf_note(TAG, "range");
        group_range();
        rc = cf_summary(TAG);
        if (g_desc != nil)
            puts([g_desc UTF8String]);
    }
    return rc;
}
EOC

    cat > "$TMP/objc_variadic.m" <<'EOC'
#include "objc_common.h"

#define TAG "objc_variadic"

static void objc_put(NSString *s)
{
    const char *text = s != nil ? [s UTF8String] : "(nil)";
    cb_size n = 0;

    while (text[n])
        n++;
    write(1, text, n);
    write(1, "\n", 1);
}

static void group_format(void)
{
    unsigned m = 0, bit = 1;
    NSString *obj = [NSString stringWithUTF8String:"obj"];
    NSNumber *num = [NSNumber numberWithInt:17];
    NSString *wide = [NSString stringWithUTF8String:"k\xc5\xaf\xc5\x88"];
    NSString *s, *init, *appended;

    s = [NSString stringWithFormat:@"%d %@ %.2f %s %ld|%u %@ %.3f %s %lld|%x %e %g %c %.1f|%.4f %@ %.0f %zu %g|%hhd %hd %.2f %@ %lu",
                                   -12, obj, 1.25, "cstr", 1234567890123L,
                                   4000000000u, num, 2.5, "two", -9876543210LL,
                                   0xbeef, 12345.678, 0.0001, 'Z', 3.25,
                                   -6.125, wide, 1e10, (size_t)77, 9.5,
                                   (char)-7, (short)-300, -0.75, @"lit", 18446744073709551615ul];
    init = [[NSString alloc] initWithFormat:@"%@:%d:%.1f:%s:%ld:%@:%.2e:%x:%g:%d",
                                            obj, 1, 1.5, "c", -2L, num, 314.159, 255u, 0.5, -3];
    appended = [s stringByAppendingFormat:@"|%@ %d %.1f", @"tail", 8, 8.5];

    CK(s != nil && objc_text_is(s, "-12 obj 1.25 cstr 1234567890123|4000000000 17 2.500 two -9876543210|beef 1.234568e+04 0.0001 Z 3.2|-6.1250 k\xc5\xaf\xc5\x88 10000000000 77 9.5|-7 -300 -0.75 lit 18446744073709551615"));
    CK(init != nil && objc_text_is(init, "obj:1:1.5:c:-2:17:3.14e+02:ff:0.5:-3"));
    CK(appended != nil && [appended hasPrefix:s] == YES && [appended hasSuffix:@"|tail 8 8.5"] == YES);

    cf_begin(TAG, "format", m);
    cf_long("len", s != nil ? (long)[s length] : -1);
    cb_end();
    objc_put(s);
    objc_put(init);
}

static void group_objects(void)
{
    unsigned m = 0, bit = 1;
    NSString *a = [NSString stringWithUTF8String:"a"];
    NSNumber *b = [NSNumber numberWithDouble:2.5];
    NSString *c = [NSString stringWithUTF8String:"a string long enough to live on the heap"];
    NSNumber *d = [NSNumber numberWithLongLong:-4];
    NSString *e = @"e";
    NSString *f = [NSString stringWithUTF8String:"f"];
    NSString *g = [NSString stringWithUTF8String:"g"];
    NSString *k1 = [NSString stringWithUTF8String:"k1"];
    NSString *k2 = @"k2";
    NSString *k3 = [NSString stringWithUTF8String:"a key long enough to live on the heap"];
    NSArray *three = [NSArray arrayWithObjects:a, b, c, nil];
    NSArray *seven = [NSArray arrayWithObjects:a, b, c, d, e, f, g, nil];
    NSDictionary *dict = [NSDictionary dictionaryWithObjectsAndKeys:a, k1, c, k2, d, k3, nil];
    NSDictionary *one = [NSDictionary dictionaryWithObjectsAndKeys:b, k1, nil];
    long n3 = -1, n7 = -1, nd = -1;

    if (three != nil)
        n3 = (long)[three count];
    if (seven != nil)
        n7 = (long)[seven count];
    if (dict != nil)
        nd = (long)[dict count];

    CK(three != nil && n3 == 3 && [three objectAtIndex:0] == a && [three objectAtIndex:1] == b &&
       [three objectAtIndex:2] == c);
    CK(seven != nil && n7 == 7 && [seven objectAtIndex:3] == d && [seven objectAtIndex:4] == e &&
       [seven objectAtIndex:5] == f && [seven objectAtIndex:6] == g && [seven objectAtIndex:0] == a);
    CK(dict != nil && nd == 3 && [dict objectForKey:@"k1"] == a && [dict objectForKey:k2] == c &&
       [dict objectForKey:[NSString stringWithUTF8String:"a key long enough to live on the heap"]] == d);
    CK(one != nil && [one count] == 1 && [one objectForKey:k1] == b);

    cf_begin(TAG, "objects", m);
    cf_long("n3", n3);
    cf_long("n7", n7);
    cf_long("nd", nd);
    cb_end();
}

static void group_append(void)
{
    unsigned m = 0, bit = 1;
    NSMutableString *ms = [NSMutableString stringWithString:@"start"];
    NSString *key = [NSString stringWithUTF8String:"key"];

    [ms appendFormat:@" %@=%d", key, 5];
    [ms appendFormat:@" %.2f/%s/%lu/%@/%d/%d/%d/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f",
                     0.5, "c", 99ul, @"obj", 1, 2, 3, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5];
    [ms appendFormat:@"%@", @""];

    CK(objc_text_is(ms, "start key=5 0.50/c/99/obj/1/2/3/1.5/2.5/3.5/4.5/5.5/6.5/7.5/8.5/9.5"));

    cf_begin(TAG, "append", m);
    cf_long("len", (long)[ms length]);
    cb_end();
    objc_put(ms);
}

static void group_cfformat(void)
{
    unsigned m = 0, bit = 1;
    NSString *obj = [NSString stringWithUTF8String:"obj"];
    CFStringRef s = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%d %@ %.2f %s %ld %x %c %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %d"),
                                             -1, (__bridge CFTypeRef)obj, 0.25, "cf", 9876543210L, 0xabcu, 'q',
                                             1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10);
    CFMutableStringRef ms = CFStringCreateMutable(kCFAllocatorDefault, 0);

    CFStringAppendFormat(ms, NULL, CFSTR("%@|%d|%.3f|%s|%d|%d|%d|%d"), CFSTR("lit"), 1, 0.125, "s", 2, 3, 4, 5);

    CK(s != 0 && objc_text_is((__bridge NSString *)s, "-1 obj 0.25 cf 9876543210 abc q 1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0 9.0 10"));
    CK(ms != 0 && objc_text_is((__bridge NSString *)ms, "lit|1|0.125|s|2|3|4|5"));

    cf_begin(TAG, "cfformat", m);
    cb_end();
    objc_put((__bridge NSString *)s);
    objc_put((__bridge NSString *)ms);
    if (s)
        CFRelease(s);
    if (ms)
        CFRelease(ms);
}

int main(void)
{
    int rc;

    @autoreleasepool {
        cf_note(TAG, "format");
        group_format();
        cf_note(TAG, "objects");
        group_objects();
        cf_note(TAG, "append");
        group_append();
        cf_note(TAG, "cfformat");
        group_cfformat();
        rc = cf_summary(TAG);
    }
    return rc;
}
EOC

    cat > "$TMP/native_nslog.m" <<'EOC'
#include "objc_common.h"

#define TAG "native_nslog"

static volatile long g_v[6] = { 0x1111111111111111l, -2, 0x3333333333l, 4, -0x5555555555l, 6 };
static volatile double g_d = -1.25;

static unsigned kept_sum(long a, long b, long c, long d, long e, long f, double x)
{
    unsigned h = 0x811c9dc5u;

    h = cf_fold(h, (unsigned)a);
    h = cf_fold(h, (unsigned)(b >> 32));
    h = cf_fold(h, (unsigned)c);
    h = cf_fold(h, (unsigned)(d * 3));
    h = cf_fold(h, (unsigned)(e >> 20));
    h = cf_fold(h, (unsigned)f);
    return cf_fold(h, (unsigned)(x * 1000.0));
}

__attribute__((noinline))
static unsigned log_all(NSString *obj, NSNumber *num, NSString *wide)
{
    long a = g_v[0], b = g_v[1], c = g_v[2], d = g_v[3], e = g_v[4], f = g_v[5];
    double x = g_d;

    NSLog(@"x=%d y=%@", 42, obj);
    NSLog(@"many %d %@ %.2f %s %ld %u %c %x %@|%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
          -7, num, x, "cstr", a, 4000000000u, 'N', 0xbeefu, @"lit",
          1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5);
    NSLog(@"%@ %s %C", wide, "ascii", (unichar)0x17d);
    NSLog(@"plain message with no arguments");
    return kept_sum(a, b, c, d, e, f, x);
}

int main(void)
{
    unsigned m = 0, bit = 1, want, got;
    int rc;

    @autoreleasepool {
        NSString *obj = [NSString stringWithUTF8String:"hello"];
        NSNumber *num = [NSNumber numberWithDouble:0.5];
        NSString *wide = [NSString stringWithUTF8String:"\xc5\xbdlu\xc5\xa5ou\xc4\x8dk\xc3\xbd k\xc5\xaf\xc5\x88 \xe2\x82\xac"];

        want = kept_sum(g_v[0], g_v[1], g_v[2], g_v[3], g_v[4], g_v[5], g_d);
        cf_note(TAG, "calling NSLog");
        got = log_all(obj, num, wide);
        cf_note(TAG, "returned from NSLog");

        CK(got == want);
        CK(objc_text_is(obj, "hello") && [num doubleValue] == 0.5);

        cf_begin(TAG, "log", m);
        cb_field("lines", 4, 0);
        cb_end();
        rc = cf_summary(TAG);
    }
    return rc;
}
EOC

    cat > "$TMP/native_printf.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include "cb_common.h"

#define TAG "native_printf"

int native_printf_chk(char *big, char *small, int *ret, int v, double d);
int dprintf(int, const char *, ...);
int asprintf(char **, const char *, ...);
cb_size strlen(const char *);

static unsigned np_failed, np_group;

static void np_note(const char *what)
{
    write(2, TAG ": ", sizeof TAG + 1);
    write(2, what, strlen(what));
    write(2, "\n", 1);
}

static void np_begin(const char *group, unsigned mask)
{
    cb_len = 0;
    cb_str(TAG " ");
    cb_str(group);
    if (mask == 0) {
        cb_str(" ok");
    } else {
        cb_str(" bad:");
        cb_hex(mask);
        np_failed |= 1u << np_group;
    }
    np_group++;
}

static void np_int(const char *key, int v)
{
    cb_str(" ");
    cb_str(key);
    cb_str("=");
    if (v < 0) {
        cb_str("-");
        cb_dec((unsigned)-v);
    } else {
        cb_dec((unsigned)v);
    }
}

static volatile int g_i = -42;
static volatile double g_d = 3.14159;

int main(void)
{
    unsigned m1 = 0, m2 = 0, m3 = 0, m4 = 0, m, bit;
    int r_printf, r_star, r_out, r_err, r_snprintf, r_trunc, r_sprintf, r_asprintf, r_null, r_ptr;
    int r_dout, r_derr, r_chk[2] = { -1, -1 };
    char buf[256], small[12], sbuf[96], pbuf[32], nbuf[8], cbig[160], csmall[12];
    char *heap = 0;
    int local = 0;

    np_note("printf");
    r_printf = printf("printf %d %u %ld %lld %zu %x %c %s|%f %e %g %.3f|%d %d|%g %g %g %g %g\n",
                      g_i, 4000000000u, -1234567890123L, 9223372036854775807LL, (cb_size)12345,
                      0xdeadbeefu, 'Q', "str", g_d, -2.5e-7, 1e21, 2.0 / 3, -7, 99,
                      0.1, 100.0, 1.5e300, -0.0, 5e-324);
    r_star = printf("star [%*d] [%-*d] [%*.*f] [%.*s] [%0*x] [%-*.*e]\n",
                    6, 42, 5, -3, 10, 3, g_d, 4, "truncate", 8, 0xbeef, 12, 2, -g_d);
    r_out = fprintf(stdout, "fprintf %s %d %.2f %ld %d %d %d %d|%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n",
                    "out", 1, 0.125, 1L << 40, 2, 3, 4, 5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5);
    r_err = fprintf(stderr, TAG " stderr %s %d %.3e %lu %d %d %d %d %d|%g %g %g %g %g %g %g %g %g\n",
                    "err", -1, 6.02214076e23, 18446744073709551615ul, 6, 7, 8, 9, 10,
                    0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625, 0.0078125, 0.00390625, 0.001953125);

    bit = 1;
    m = 0;
    CK(r_printf == 152);
    CK(r_star == 68);
    CK(r_out == 77);
    CK(r_err == 141);
    m1 = m;

    np_note("buffers");
    r_snprintf = snprintf(buf, sizeof buf, "snprintf %d %s %.4f %lld %x %c %d %d|%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f",
                          g_i, "s", g_d, -1LL, 255u, 'c', 7, 8, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);
    r_trunc = snprintf(small, sizeof small, "%s-%d", "truncated-text", 12345);
    r_sprintf = sprintf(sbuf, "sprintf %+d %05d %-6s| %#x %#o %e %u %ld %d %d", 17, -42, "ab", 255u, 8u, 1e-3, 1u, 2L, 3, 4);
    r_asprintf = asprintf(&heap, "asprintf %s %d %.3f %ld %d %d %d %d|%g %g %g %g %g %g %g %g %g",
                          "heap", g_i, g_d, 123456789012L, 5, 6, 7, 8, 1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8, 9.9);
    r_ptr = snprintf(pbuf, sizeof pbuf, "%p", (void *)&local);
    r_null = snprintf(nbuf, sizeof nbuf, "%p", (void *)0);
    puts(buf);
    puts(small);
    puts(sbuf);
    if (heap != 0)
        puts(heap);

    bit = 1;
    m = 0;
    CK(r_snprintf == (int)strlen(buf) && strcmp(buf, "snprintf -42 s 3.1416 -1 ff c 7 8|1.00 2.00 3.00 4.00 5.00 6.00 7.00 8.00 9.00") == 0);
    CK(r_trunc == 20 && strcmp(small, "truncated-t") == 0);
    CK(r_sprintf == (int)strlen(sbuf) && strcmp(sbuf, "sprintf +17 -0042 ab    | 0xff 010 1.000000e-03 1 2 3 4") == 0);
    CK(heap != 0 && r_asprintf == (int)strlen(heap) &&
       strcmp(heap, "asprintf heap -42 3.142 123456789012 5 6 7 8|1.1 2.2 3.3 4.4 5.5 6.6 7.7 8.8 9.9") == 0);
    CK(r_ptr > 2 && pbuf[0] == '0' && pbuf[1] == 'x' && strcmp(pbuf, "0x0") != 0);
    CK(r_null == 3 && strcmp(nbuf, "0x0") == 0);
    m2 = m;
    free(heap);

    np_note("chk");
    native_printf_chk(cbig, csmall, r_chk, g_i, g_d);
    puts(cbig);
    puts(csmall);
    bit = 1;
    m = 0;
    CK(r_chk[0] == (int)strlen(cbig));
    CK(r_chk[1] > 11 && strlen(csmall) == 11);
    m3 = m;

    fflush(stdout);
    np_note("dprintf");
    r_dout = dprintf(1, "dprintf %d %s %.2f %ld %d %d %d %d|%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n",
                     g_i, "fd", g_d, -5L, 1, 2, 3, 4, 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
    r_derr = dprintf(2, TAG " stderr dprintf %d %s %x\n", 2, "fd", 0xfdu);
    bit = 1;
    m = 0;
    CK(r_dout == 67);
    CK(r_derr == 37);
    m4 = m;

    np_begin("stdio", m1);
    np_int("printf", r_printf);
    np_int("star", r_star);
    np_int("fprintf", r_out);
    np_int("stderr", r_err);
    cb_end();
    np_begin("buffers", m2);
    np_int("snprintf", r_snprintf);
    np_int("trunc", r_trunc);
    np_int("sprintf", r_sprintf);
    np_int("asprintf", r_asprintf);
    np_int("null", r_null);
    cb_end();
    np_begin("chk", m3);
    np_int("sprintf_chk", r_chk[0]);
    np_int("snprintf_chk", r_chk[1]);
    cb_end();
    np_begin("fd", m4);
    np_int("out", r_dout);
    np_int("err", r_derr);
    np_int("lines", 2);
    cb_end();
    cb_begin(TAG, np_failed);
    cb_end();
    return np_failed != 0;
}
EOC

    cat > "$TMP/native_printf_chk.c" <<'EOC'
#include <stdio.h>

int native_printf_chk(char *big, char *small, int *ret, int v, double d)
{
    char b[160], s[12];
    int i;

    ret[0] = sprintf(b, "sprintf_chk %d %s %lld %x %c %u %d|%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f",
                     v, "chk", 1LL << 50, 0xabcu, 'k', 3000000000u, -v, d, d * 2, d * 3, d * 4, d * 5, d * 6, d * 7, d * 8, d * 9);
    ret[1] = snprintf(s, sizeof s, "snprintf_chk %d %s %d %d %d %d %d|%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
                      v, "chk", 1, 2, 3, 4, 5, d, d, d, d, d, d, d, d, d);
    for (i = 0; i < 159 && b[i] != 0; i++)
        big[i] = b[i];
    big[i] = 0;
    for (i = 0; i < 11 && s[i] != 0; i++)
        small[i] = s[i];
    small[i] = 0;
    return 0;
}
EOC

    for name in objc_foundation objc_variadic native_nslog; do
        clang -arch x86_64 -x objective-c -fobjc-arc -O1 -fno-builtin \
                -o "$TMP/$name" "$TMP/$name.m" -framework Foundation >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            objc_foundation) OBJC_FOUNDATION_BIN="$TMP/$name" ;;
            objc_variadic) OBJC_VARIADIC_BIN="$TMP/$name" ;;
            native_nslog) NATIVE_NSLOG_BIN="$TMP/$name" ;;
        esac
        clang -arch arm64 -x objective-c -fobjc-arc -O1 -fno-builtin \
                -o "$TMP/$name.arm64" "$TMP/$name.m" -framework Foundation >"$TMP/$name.arm64.cc.log" 2>&1 || continue
        case $name in
            objc_foundation) OBJC_FOUNDATION_ARM64="$TMP/$name.arm64" ;;
            objc_variadic) OBJC_VARIADIC_ARM64="$TMP/$name.arm64" ;;
            native_nslog) NATIVE_NSLOG_ARM64="$TMP/$name.arm64" ;;
        esac
    done

    for arch in x86_64 arm64; do
        sfx=""
        if [ "$arch" = arm64 ]; then
            sfx=".arm64"
        fi
        clang -arch "$arch" -std=c11 -O1 -fno-builtin -D_FORTIFY_SOURCE=0 -c \
                -o "$TMP/native_printf$sfx.o" "$TMP/native_printf.c" >"$TMP/native_printf$sfx.cc.log" 2>&1 || return
        clang -arch "$arch" -std=c11 -O2 -fno-builtin -D_FORTIFY_SOURCE=2 -c \
                -o "$TMP/native_printf_chk$sfx.o" "$TMP/native_printf_chk.c" >>"$TMP/native_printf$sfx.cc.log" 2>&1 || return
        clang -arch "$arch" -o "$TMP/native_printf$sfx" "$TMP/native_printf$sfx.o" "$TMP/native_printf_chk$sfx.o" \
                >>"$TMP/native_printf$sfx.cc.log" 2>&1 || return
        if [ "$arch" = arm64 ]; then
            NATIVE_PRINTF_ARM64="$TMP/native_printf$sfx"
        else
            NATIVE_PRINTF_BIN="$TMP/native_printf$sfx"
        fi
    done
}

build_objc_class_fixtures() {
    local name framework

    cat > "$TMP/appkit_common.h" <<'EOC'
#import <AppKit/AppKit.h>
#include "objc_common.h"

static int view_unavailable(const char *tag, const char *why)
{
    cb_len = 0;
    cb_str(tag);
    cb_str(" unavailable ");
    cb_str(why);
    cb_end();
    return 2;
}

static NSBitmapImageRep *view_device_rep(long w, long h)
{
    return [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4
                                                     hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                                                  bytesPerRow:0 bitsPerPixel:0];
}

static unsigned view_checksum(NSBitmapImageRep *rep)
{
    unsigned char *d = rep != nil ? [rep bitmapData] : 0;
    long bpp = [rep bitsPerPixel] / 8, row = [rep bytesPerRow], w = [rep pixelsWide], h = [rep pixelsHigh], x, y;
    unsigned sum = 0x811c9dc5u;

    if (d == 0 || bpp <= 0 || row < w * bpp)
        return 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w * bpp; x++) {
            sum ^= d[y * row + x];
            sum *= 0x01000193u;
        }
    }
    return sum;
}

static unsigned view_pixel(NSBitmapImageRep *rep, long x, long y)
{
    unsigned char *d = rep != nil ? [rep bitmapData] : 0;
    unsigned char *p;

    if (d == 0 || [rep bitsPerPixel] != 32 || x < 0 || y < 0 || x >= [rep pixelsWide] || y >= [rep pixelsHigh])
        return 1;
    p = d + y * [rep bytesPerRow] + x * 4;
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static int view_rect_is(NSRect r, double x, double y, double w, double h)
{
    return r.origin.x == x && r.origin.y == y && r.size.width == w && r.size.height == h;
}
EOC

    cat > "$TMP/objc_classes.m" <<'EOC'
#include "objc_common.h"

#define TAG "objc_classes"

static char g_load_order[8];
static int g_load_n;
static unsigned g_loaded;
static int g_init_shape, g_init_square, g_init_other;
static int g_dealloc_shape, g_dealloc_square, g_dealloc_name_alive;
static int g_copies, g_equal_calls, g_hash_calls, g_compare_calls, g_desc_calls, g_perform_calls, g_super_equal;

@protocol OcerzNamed <NSObject>
- (NSString *)name;
@optional
- (void)ocerzOptional;
@end

@interface OcerzShape : NSObject <OcerzNamed, NSCopying>
{
@public
    int _sides;
    double _area;
    NSString *_name;
    __weak id _owner;
}
@property (nonatomic) int sides;
@property (nonatomic) double area;
@property (strong) NSString *name;
@property (nonatomic, weak) id owner;
+ (instancetype)shapeWithName:(NSString *)name sides:(int)sides area:(double)area;
+ (id)shapeNamed:(NSString *)name;
- (instancetype)initWithName:(NSString *)name sides:(int)sides area:(double)area NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
- (NSComparisonResult)compare:(OcerzShape *)other;
- (id)scaledBy:(NSNumber *)factor;
- (id)joinedWith:(id)a and:(id)b;
@end

@interface OcerzSquare : OcerzShape
{
@public
    double _side;
    int _tag;
}
- (instancetype)initWithSide:(double)side;
@end

@interface NSString (OcerzExtras)
+ (NSString *)ocerzGreeting;
- (NSString *)ocerzReversed;
- (long long)ocerzChecksum;
@end

static long long fnv64(const char *s)
{
    unsigned long long h = 0xcbf29ce484222325ull;

    while (s != 0 && *s) {
        h ^= (unsigned char)*s++;
        h *= 0x100000001b3ull;
    }
    return (long long)h;
}

@implementation OcerzSquare

+ (void)load
{
    g_load_order[g_load_n++] = 'Q';
}

+ (void)initialize
{
    if (self == [OcerzSquare class])
        g_init_square++;
    else
        g_init_other++;
}

- (instancetype)initWithSide:(double)side
{
    self = [super initWithName:[NSString stringWithUTF8String:"square"] sides:4 area:side * side];
    if (self != nil) {
        _side = side;
        _tag = 0x5a5a;
    }
    return self;
}

- (NSString *)description
{
    return [NSString stringWithFormat:@"%@ side=%.1f", [super description], _side];
}

- (BOOL)isEqual:(id)other
{
    int same = [super isEqual:other];

    g_super_equal++;
    return same == YES && [other isKindOfClass:[OcerzSquare class]] == YES && ((OcerzSquare *)other)->_side == _side;
}

- (void)dealloc
{
    g_dealloc_square++;
}

@end

@implementation OcerzShape

@synthesize sides = _sides, area = _area, name = _name, owner = _owner;

+ (void)load
{
    g_loaded = 0x10ad;
    g_load_order[g_load_n++] = 'S';
}

+ (void)initialize
{
    if (self == [OcerzShape class])
        g_init_shape++;
    else
        g_init_other++;
}

+ (instancetype)shapeWithName:(NSString *)name sides:(int)sides area:(double)area
{
    return [[self alloc] initWithName:name sides:sides area:area];
}

+ (id)shapeNamed:(NSString *)name
{
    g_perform_calls++;
    return [[self alloc] initWithName:name sides:6 area:6.5];
}

- (instancetype)initWithName:(NSString *)name sides:(int)sides area:(double)area
{
    self = [super init];
    if (self != nil) {
        _name = name;
        _sides = sides;
        _area = area;
    }
    return self;
}

- (id)copyWithZone:(NSZone *)zone
{
    OcerzShape *c = [[[self class] allocWithZone:zone] initWithName:_name sides:_sides area:_area];

    g_copies++;
    return c;
}

- (NSString *)description
{
    g_desc_calls++;
    return [NSString stringWithFormat:@"<%@ %@ sides=%d area=%.2f>", NSStringFromClass([self class]), _name, _sides, _area];
}

- (BOOL)isEqual:(id)other
{
    OcerzShape *o;

    g_equal_calls++;
    if (other == self)
        return YES;
    if ([other isKindOfClass:[OcerzShape class]] != YES)
        return NO;
    o = other;
    return o->_sides == _sides && o->_area == _area && [o->_name isEqualToString:_name] == YES;
}

- (NSUInteger)hash
{
    g_hash_calls++;
    return (NSUInteger)fnv64([_name UTF8String]) ^ ((NSUInteger)_sides << 44) ^ (NSUInteger)(long long)(_area * 1000.0);
}

- (NSComparisonResult)compare:(OcerzShape *)other
{
    g_compare_calls++;
    if (_area != other->_area)
        return _area < other->_area ? NSOrderedAscending : NSOrderedDescending;
    if (_sides != other->_sides)
        return _sides < other->_sides ? NSOrderedAscending : NSOrderedDescending;
    return NSOrderedSame;
}

- (id)scaledBy:(NSNumber *)factor
{
    g_perform_calls++;
    return [[OcerzShape alloc] initWithName:_name sides:_sides area:_area * [factor doubleValue]];
}

- (id)joinedWith:(id)a and:(id)b
{
    g_perform_calls++;
    return [NSString stringWithFormat:@"%@+%@+%@", _name, [a name], [b name]];
}

- (void)dealloc
{
    g_dealloc_shape++;
    g_dealloc_name_alive = _name != nil && [_name length] > 0;
}

@end

@implementation NSString (OcerzExtras)

+ (void)load
{
    g_load_order[g_load_n++] = 'C';
}

+ (NSString *)ocerzGreeting
{
    return [NSString stringWithUTF8String:"hello from a category"];
}

- (NSString *)ocerzReversed
{
    NSMutableString *out = [NSMutableString stringWithCapacity:[self length]];
    NSUInteger i = [self length];

    while (i > 0) {
        i--;
        [out appendString:[self substringWithRange:NSMakeRange(i, 1)]];
    }
    return out;
}

- (long long)ocerzChecksum
{
    return fnv64([self UTF8String]);
}

@end

static void objc_put(NSString *s)
{
    const char *text = s != nil ? [s UTF8String] : "(nil)";
    cb_size n = 0;

    while (text[n])
        n++;
    write(1, text, n);
    write(1, "\n", 1);
}

static void cf_text(const char *key, const char *v)
{
    cb_str(" ");
    cb_str(key);
    cb_str("=");
    cb_str(v);
}

static void group_load(void)
{
    unsigned m = 0, bit = 1;
    int before_shape = g_init_shape, before_square = g_init_square;
    OcerzShape *s;
    OcerzSquare *q;
    int after_shape, after_square;

    CK(g_loaded == 0x10ad);
    CK(g_load_n == 3 && g_load_order[0] == 'S' && g_load_order[1] == 'Q' && g_load_order[2] == 'C');
    CK(before_shape == 0 && before_square == 0);
    s = [OcerzShape shapeWithName:@"first" sides:3 area:1.0];
    after_shape = g_init_shape;
    after_square = g_init_square;
    CK(s != nil && after_shape == 1 && after_square == 0);
    q = [[OcerzSquare alloc] initWithSide:2.0];
    CK(q != nil && g_init_shape == 1 && g_init_square == 1);
    s = [OcerzShape shapeWithName:@"again" sides:5 area:2.0];
    q = [[OcerzSquare alloc] initWithSide:3.0];
    CK(g_init_shape == 1 && g_init_square == 1 && g_init_other == 0);

    cf_begin(TAG, "load", m);
    cf_text("order", g_load_order);
    cf_long("init", g_init_shape + g_init_square + g_init_other);
    cb_end();
}

static void group_object(void)
{
    unsigned m = 0, bit = 1;
    OcerzShape *t = [OcerzShape shapeWithName:[NSString stringWithUTF8String:"triangle"] sides:3 area:1.5];
    OcerzShape *q = [[OcerzShape alloc] initWithName:@"quad" sides:4 area:2.25];
    NSString *pent = [NSString stringWithUTF8String:"pentagon with a name too long to be tagged"];

    CK(t != nil && [t class] == [OcerzShape class] && [t superclass] == [NSObject class] && [OcerzShape superclass] == [NSObject class]);
    CK([t isKindOfClass:[NSObject class]] == YES && [t isMemberOfClass:[OcerzShape class]] == YES && [t isKindOfClass:[NSString class]] == NO);
    CK(objc_text_is(NSStringFromClass([t class]), "OcerzShape") && NSClassFromString(@"OcerzShape") == [OcerzShape class]);
    CK(t.sides == 3 && t.area == 1.5 && objc_text_is(t.name, "triangle") && t->_sides == 3 && t->_area == 1.5);
    t.sides = 5;
    t.area = -0.75;
    t.name = pent;
    CK(t->_sides == 5 && [t sides] == 5 && t->_area == -0.75 && [t area] == -0.75);
    CK(t->_name == pent && [t name] == pent);
    t.owner = q;
    CK(t.owner == q && t->_owner == q);
    CK([[t valueForKey:@"sides"] intValue] == 5 && [[t valueForKey:@"area"] doubleValue] == -0.75 && [t valueForKey:@"name"] == pent);
    [t setValue:[NSNumber numberWithInt:7] forKey:@"sides"];
    [t setValue:[NSNumber numberWithDouble:12.5] forKey:@"area"];
    CK(t->_sides == 7 && t->_area == 12.5 && t.owner == q);

    cf_begin(TAG, "object", m);
    cf_long("sides", t->_sides);
    cf_long("area", (long)(t->_area * 100.0));
    cb_end();
}

static void group_describe(void)
{
    unsigned m = 0, bit = 1;
    OcerzShape *t = [OcerzShape shapeWithName:@"hexagon" sides:6 area:-0.75];
    OcerzShape *q = [[OcerzShape alloc] initWithName:[NSString stringWithUTF8String:"quad"] sides:4 area:2.25];
    int calls0 = g_desc_calls, calls1;
    NSString *formatted = [NSString stringWithFormat:@"%@", t];
    NSString *direct, *debug, *listed;

    calls1 = g_desc_calls;
    direct = [t description];
    debug = [t debugDescription];
    listed = [[NSArray arrayWithObjects:t, q, nil] description];

    CK(objc_text_is(formatted, "<OcerzShape hexagon sides=6 area=-0.75>"));
    CK(calls1 == calls0 + 1);
    CK([formatted isEqualToString:direct] == YES && [debug isEqualToString:direct] == YES);
    CK(listed != nil && [listed rangeOfString:@"<OcerzShape hexagon sides=6 area=-0.75>"].location != NSNotFound &&
       [listed rangeOfString:@"<OcerzShape quad sides=4 area=2.25>"].location != NSNotFound);
    CK(g_desc_calls == calls0 + 5);

    cf_begin(TAG, "describe", m);
    cf_long("len", formatted != nil ? (long)[formatted length] : -1);
    cb_end();
    objc_put(formatted);
    objc_put(listed);
}

static void group_equality(void)
{
    unsigned m = 0, bit = 1;
    OcerzShape *a = [[OcerzShape alloc] initWithName:[NSString stringWithUTF8String:"equal shape"] sides:4 area:1.0];
    OcerzShape *b = [[OcerzShape alloc] initWithName:@"equal shape" sides:4 area:1.0];
    OcerzShape *c = [[OcerzShape alloc] initWithName:@"equal shape" sides:5 area:1.0];
    OcerzShape *probe = [[OcerzShape alloc] initWithName:[NSString stringWithUTF8String:"equal shape"] sides:4 area:1.0];
    NSString *first = [NSString stringWithUTF8String:"the first value stored"];
    NSString *second = [NSString stringWithUTF8String:"the second value stored"];
    int e0 = g_equal_calls, h0 = g_hash_calls, c0, member_ok;
    NSSet *set;
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    NSMutableSet *many = [NSMutableSet set];
    id key, member;
    unsigned order = 0x811c9dc5u;
    int i;

    CK(a != b && [a isEqual:b] == YES && [b isEqual:a] == YES && [a isEqual:c] == NO && [a isEqual:@"equal shape"] == NO);
    CK([a hash] == [b hash] && [a hash] == [probe hash] && [a hash] != [c hash]);
    e0 = g_equal_calls;
    h0 = g_hash_calls;
    set = [NSSet setWithObjects:a, b, c, nil];
    member = [set member:probe];
    member_ok = member == a || member == b;
    CK(set != nil && [set count] == 2 && [set containsObject:probe] == YES && member_ok);
    CK(g_hash_calls > h0 && g_equal_calls > e0);
    c0 = g_copies;
    [dict setObject:first forKey:a];
    [dict setObject:second forKey:b];
    key = [[dict allKeys] firstObject];
    CK([dict count] == 1 && [dict objectForKey:probe] == second && [dict objectForKey:c] == nil);
    CK(g_copies > c0 && key != a && key != b && [key isEqual:a] == YES && [key class] == [OcerzShape class]);
    for (i = 0; i < 12; i++) {
        char nm[16] = "shape-";
        nm[6] = (char)('a' + i);
        [many addObject:[[OcerzShape alloc] initWithName:[NSString stringWithUTF8String:nm] sides:i + 3 area:i * 0.5]];
    }
    [many addObject:[[OcerzShape alloc] initWithName:@"shape-c" sides:5 area:1.0]];
    for (OcerzShape *s in many)
        order = cf_fold(order, (unsigned)s->_sides);
    CK([many count] == 12);

    cf_begin(TAG, "equality", m);
    cf_long("set", (long)[set count]);
    cb_str(" order=");
    cb_hex(order);
    cb_end();
}

static void group_sort(void)
{
    unsigned m = 0, bit = 1;
    NSArray *shapes = [NSArray arrayWithObjects:
                       [OcerzShape shapeWithName:@"e" sides:8 area:4.0],
                       [OcerzShape shapeWithName:@"b" sides:4 area:-1.5],
                       [OcerzShape shapeWithName:@"d" sides:3 area:2.5],
                       [OcerzShape shapeWithName:@"a" sides:5 area:-2.0],
                       [OcerzShape shapeWithName:@"c" sides:6 area:2.5],
                       [OcerzShape shapeWithName:@"f" sides:7 area:100.0], nil];
    int calls0 = g_compare_calls;
    NSArray *sorted = [shapes sortedArrayUsingSelector:@selector(compare:)];
    char seen[8] = { 0 };
    int i, n = sorted != nil ? (int)[sorted count] : -1;

    for (i = 0; i < n && i < 7; i++)
        seen[i] = (char)('0' + ((OcerzShape *)[sorted objectAtIndex:(NSUInteger)i])->_sides);

    CK(n == 6);
    CK(seen[0] == '5' && seen[1] == '4' && seen[2] == '3' && seen[3] == '6' && seen[4] == '8' && seen[5] == '7');
    CK(g_compare_calls > calls0);
    CK(((OcerzShape *)[shapes objectAtIndex:0])->_sides == 8 && ((OcerzShape *)[shapes objectAtIndex:5])->_sides == 7);
    CK([(OcerzShape *)[shapes objectAtIndex:1] compare:[shapes objectAtIndex:0]] == -1 && [(OcerzShape *)[shapes objectAtIndex:2] compare:[shapes objectAtIndex:4]] == -1 &&
       [(OcerzShape *)[shapes objectAtIndex:4] compare:[shapes objectAtIndex:2]] == 1 && [(OcerzShape *)[shapes objectAtIndex:3] compare:[shapes objectAtIndex:3]] == 0);

    cf_begin(TAG, "sort", m);
    cf_text("order", seen);
    cb_end();
}

static void group_subclass(void)
{
    unsigned m = 0, bit = 1;
    OcerzSquare *sq = [[OcerzSquare alloc] initWithSide:1.5];
    OcerzSquare *same = [[OcerzSquare alloc] initWithSide:9.0];
    OcerzSquare *other = [[OcerzSquare alloc] initWithSide:9.0];
    NSString *oct = [NSString stringWithUTF8String:"octagon"];
    int eq0;

    CK(sq != nil && [sq class] == [OcerzSquare class] && [sq superclass] == [OcerzShape class] && [OcerzSquare superclass] == [OcerzShape class]);
    CK([sq isKindOfClass:[OcerzShape class]] == YES && [sq isMemberOfClass:[OcerzShape class]] == NO && NSClassFromString(@"OcerzSquare") == [OcerzSquare class]);
    CK(sq->_sides == 4 && sq->_area == 2.25 && objc_text_is(sq->_name, "square") && sq->_side == 1.5 && sq->_tag == 0x5a5a);
    sq->_side = 9.0;
    sq->_tag = -1;
    CK(sq.sides == 4 && sq.area == 2.25 && objc_text_is(sq.name, "square"));
    sq.sides = 8;
    sq.area = 3.0;
    sq.name = oct;
    CK(sq->_side == 9.0 && sq->_tag == -1 && sq->_sides == 8 && sq->_area == 3.0 && sq->_name == oct);
    CK([[sq valueForKey:@"tag"] intValue] == -1 && [[sq valueForKey:@"side"] doubleValue] == 9.0 && [[sq valueForKey:@"sides"] intValue] == 8);
    [sq setValue:[NSNumber numberWithInt:1234] forKey:@"tag"];
    [sq setValue:[NSNumber numberWithDouble:-4.5] forKey:@"side"];
    CK(sq->_tag == 1234 && sq->_side == -4.5 && sq->_sides == 8 && sq->_area == 3.0 && sq->_name == oct);
    CK(objc_text_is([sq description], "<OcerzSquare octagon sides=8 area=3.00> side=-4.5"));
    eq0 = g_super_equal;
    CK([same isEqual:other] == YES && same != other && [same isEqual:sq] == NO && g_super_equal == eq0 + 2);
    CK([sq respondsToSelector:@selector(compare:)] == YES && [sq respondsToSelector:@selector(initWithSide:)] == YES);

    cf_begin(TAG, "subclass", m);
    cf_long("tag", sq->_tag);
    cb_end();
    objc_put([NSString stringWithFormat:@"%@", sq]);
}

static void group_category(void)
{
    unsigned m = 0, bit = 1;
    NSString *lit = @"Hello, world";
    NSString *made = [NSString stringWithUTF8String:"a created string that lives on the heap"];
    NSString *small = [NSString stringWithUTF8String:"abc"];
    NSMutableString *mut = [NSMutableString stringWithString:@"xyz"];
    NSString *rl = [lit ocerzReversed], *rm = [made ocerzReversed], *rs = [small ocerzReversed], *rx = [mut ocerzReversed];

    CK(objc_text_is(rl, "dlrow ,olleH"));
    CK(objc_text_is(rm, "paeh eht no sevil taht gnirts detaerc a"));
    CK(objc_tagged(small) && objc_text_is(rs, "cba"));
    CK(objc_text_is(rx, "zyx"));
    CK([lit ocerzChecksum] == fnv64("Hello, world") && [made ocerzChecksum] == fnv64([made UTF8String]) && [small ocerzChecksum] == fnv64("abc"));
    CK(objc_text_is([NSString ocerzGreeting], "hello from a category") && objc_text_is([NSMutableString ocerzGreeting], "hello from a category"));
    CK([lit respondsToSelector:@selector(ocerzReversed)] == YES && [small respondsToSelector:@selector(ocerzChecksum)] == YES &&
       [NSString instancesRespondToSelector:@selector(ocerzChecksum)] == YES && [NSNumber instancesRespondToSelector:@selector(ocerzChecksum)] == NO);

    cf_begin(TAG, "category", m);
    cf_text("lit", rl != nil ? [rl UTF8String] : "(nil)");
    cb_end();
}

static void group_protocol(void)
{
    unsigned m = 0, bit = 1;
    OcerzShape *t = [OcerzShape shapeWithName:@"protocol" sides:3 area:1.0];
    OcerzSquare *sq = [[OcerzSquare alloc] initWithSide:1.0];
    Protocol *named = @protocol(OcerzNamed);

    CK(named != nil && [OcerzShape conformsToProtocol:named] == YES && [t conformsToProtocol:named] == YES);
    CK([OcerzSquare conformsToProtocol:named] == YES && [sq conformsToProtocol:named] == YES);
    CK([NSObject conformsToProtocol:named] == NO && [@"s" conformsToProtocol:named] == NO);
    CK([OcerzShape conformsToProtocol:@protocol(NSCopying)] == YES && [OcerzShape conformsToProtocol:@protocol(NSObject)] == YES &&
       [OcerzShape conformsToProtocol:@protocol(NSCoding)] == NO);
    CK(objc_text_is(NSStringFromProtocol(named), "OcerzNamed") && NSProtocolFromString(@"OcerzNamed") == named);
    CK(NSProtocolFromString(@"NSCopying") == @protocol(NSCopying));
    CK([t respondsToSelector:@selector(name)] == YES && [t respondsToSelector:@selector(ocerzOptional)] == NO &&
       [t respondsToSelector:@selector(copyWithZone:)] == YES && [t respondsToSelector:NSSelectorFromString(@"notAMethodAnywhere")] == NO);
    CK([OcerzShape instancesRespondToSelector:@selector(scaledBy:)] == YES && [OcerzShape respondsToSelector:@selector(shapeNamed:)] == YES &&
       [OcerzShape respondsToSelector:@selector(scaledBy:)] == NO);

    cf_begin(TAG, "protocol", m);
    cf_text("name", objc_text_is(NSStringFromProtocol(named), "OcerzNamed") ? "OcerzNamed" : "?");
    cb_end();
}

static void group_lifetime(void)
{
    unsigned m = 0, bit = 1;
    int d0 = g_dealloc_shape, s0 = g_dealloc_square;
    __weak OcerzShape *weak_shape = nil;
    __weak NSString *weak_name = nil;
    __weak OcerzShape *weak_owned = nil;
    __weak OcerzShape *weak_held = nil;
    OcerzShape *holder = [[OcerzShape alloc] initWithName:@"holder" sides:1 area:1.0];
    NSMutableArray *arr = [NSMutableArray array];
    int alive_inside = 0, alive_held = 0, owner_inside = 0;

    @autoreleasepool {
        NSString *nm = [NSString stringWithUTF8String:"a transient name long enough for the heap"];
        OcerzShape *s = [[OcerzShape alloc] initWithName:nm sides:9 area:9.0];

        weak_shape = s;
        weak_name = nm;
        nm = nil;
        alive_inside = weak_shape == s && weak_name != nil && g_dealloc_shape == d0;
        s = nil;
    }
    CK(alive_inside);
    CK(weak_shape == nil && g_dealloc_shape == d0 + 1);
    CK(g_dealloc_name_alive && weak_name == nil);

    @autoreleasepool {
        OcerzShape *owned = [[OcerzShape alloc] initWithName:@"owned" sides:2 area:2.0];

        holder.owner = owned;
        weak_owned = owned;
        owner_inside = holder.owner == owned;
        owned = nil;
    }
    CK(owner_inside && weak_owned == nil && holder.owner == nil && holder->_owner == nil && g_dealloc_shape == d0 + 2);

    @autoreleasepool {
        OcerzSquare *sq = [[OcerzSquare alloc] initWithSide:2.0];

        weak_shape = sq;
        sq = nil;
    }
    CK(weak_shape == nil && g_dealloc_square == s0 + 1 && g_dealloc_shape == d0 + 3);

    @autoreleasepool {
        OcerzShape *h = [[OcerzShape alloc] initWithName:@"held" sides:3 area:3.0];

        weak_held = h;
        [arr addObject:h];
        h = nil;
    }
    alive_held = weak_held != nil && g_dealloc_shape == d0 + 3;
    [arr removeAllObjects];
    CK(alive_held && weak_held == nil && g_dealloc_shape == d0 + 4);

    cf_begin(TAG, "lifetime", m);
    cf_long("dealloc", g_dealloc_shape - d0);
    cf_long("square", g_dealloc_square - s0);
    cb_end();
}

static void group_perform(void)
{
    unsigned m = 0, bit = 1;
    OcerzShape *t = [OcerzShape shapeWithName:@"performer" sides:5 area:1.25];
    OcerzShape *a = [OcerzShape shapeWithName:@"left" sides:1 area:1.0];
    OcerzShape *b = [OcerzShape shapeWithName:@"right" sides:2 area:2.0];
    OcerzSquare *sq = [[OcerzSquare alloc] initWithSide:0.5];
    int calls0 = g_perform_calls;
    OcerzShape *scaled = [t performSelector:@selector(scaledBy:) withObject:[NSNumber numberWithInt:2]];
    id named = [t performSelector:@selector(name)];
    id joined = [t performSelector:@selector(joinedWith:and:) withObject:a withObject:b];
    id made = [OcerzShape performSelector:@selector(shapeNamed:) withObject:@"hexagon"];
    id desc = [sq performSelector:@selector(description)];

    CK(scaled != nil && [scaled class] == [OcerzShape class] && scaled->_area == 2.5 && scaled->_sides == 5 && scaled != t);
    CK(named == t->_name);
    CK(objc_text_is(joined, "performer+left+right"));
    CK([made isKindOfClass:[OcerzShape class]] == YES && ((OcerzShape *)made)->_sides == 6 && objc_text_is(((OcerzShape *)made)->_name, "hexagon"));
    CK(objc_text_is(desc, "<OcerzSquare square sides=4 area=0.25> side=0.5"));
    CK(g_perform_calls == calls0 + 3);

    cf_begin(TAG, "perform", m);
    cf_long("calls", g_perform_calls - calls0);
    cb_end();
}

int main(void)
{
    int rc;

    @autoreleasepool {
        cf_note(TAG, "load");
        group_load();
        cf_note(TAG, "object");
        group_object();
        cf_note(TAG, "describe");
        group_describe();
        cf_note(TAG, "equality");
        group_equality();
        cf_note(TAG, "sort");
        group_sort();
        cf_note(TAG, "subclass");
        group_subclass();
        cf_note(TAG, "category");
        group_category();
        cf_note(TAG, "protocol");
        group_protocol();
        cf_note(TAG, "lifetime");
        group_lifetime();
        cf_note(TAG, "perform");
        group_perform();
        rc = cf_summary(TAG);
    }
    return rc;
}
EOC

    cat > "$TMP/objc_view_render.m" <<'EOC'
#include "appkit_common.h"

#define TAG "objc_view_render"
#define VIEW_W 32
#define VIEW_H 24
#define BACKGROUND 0x204060ffu
#define FILL 0xff0000ffu
#define STROKE 0x0080ffffu
#define CGFILL 0x00ff40ffu

static int g_draw, g_flip;
static NSRect g_dirty[3];
static cb_uptr g_draw_self[3];
static int g_ctx_ok[3], g_ctx_flipped[3], g_cg_ok[3];

@interface OcerzTestView : NSView
@end

@implementation OcerzTestView

- (BOOL)isFlipped
{
    g_flip++;
    return YES;
}

- (void)drawRect:(NSRect)dirty
{
    NSGraphicsContext *ctx = [NSGraphicsContext currentContext];
    CGContextRef cg = [ctx CGContext];
    NSBezierPath *path;
    int i = g_draw < 2 ? g_draw : 2;

    g_draw++;
    g_dirty[i] = dirty;
    g_draw_self[i] = (cb_uptr)(__bridge void *)self;
    g_ctx_ok[i] = ctx != nil;
    g_ctx_flipped[i] = [ctx isFlipped];
    g_cg_ok[i] = cg != 0;
    [super drawRect:dirty];
    [ctx setShouldAntialias:NO];
    [[NSColor colorWithDeviceRed:32 / 255.0 green:64 / 255.0 blue:96 / 255.0 alpha:1.0] set];
    NSRectFill([self bounds]);
    [[NSColor colorWithDeviceRed:1.0 green:0.0 blue:0.0 alpha:1.0] set];
    NSRectFill(NSMakeRect(2, 2, 10, 6));
    if (cg != 0) {
        CGContextSetRGBFillColor(cg, 0.0, 1.0, 64 / 255.0, 1.0);
        CGContextFillRect(cg, CGRectMake(20, 2, 8, 4));
    }
    path = [NSBezierPath bezierPathWithRect:NSMakeRect(4.5, 12.5, 20, 8)];
    [path setLineWidth:1.0];
    [[NSColor colorWithDeviceRed:0.0 green:128 / 255.0 blue:1.0 alpha:1.0] set];
    [path stroke];
}

@end

static OcerzTestView *g_view;
static NSBitmapImageRep *g_rep, *g_dev;
static const char *g_unavailable;
static int g_own_flips;
static long g_scale;

static void group_view(void)
{
    unsigned m = 0, bit = 1;
    NSRect frame, bounds;
    NSString *frame_text;
    int flipped;

    g_view = [[OcerzTestView alloc] initWithFrame:NSMakeRect(5, 7, VIEW_W, VIEW_H)];
    flipped = [g_view isFlipped];
    g_own_flips = g_flip;
    frame = [g_view frame];
    bounds = [g_view bounds];
    frame_text = NSStringFromRect(frame);

    CK(g_view != nil && [g_view class] == [OcerzTestView class] && [g_view superclass] == [NSView class] &&
       [g_view isKindOfClass:[NSView class]] == YES && NSClassFromString(@"OcerzTestView") == [OcerzTestView class]);
    CK(flipped == YES && g_own_flips == 1);
    CK(view_rect_is(frame, 5, 7, VIEW_W, VIEW_H) && view_rect_is(bounds, 0, 0, VIEW_W, VIEW_H));
    CK(objc_text_is(frame_text, "{{5, 7}, {32, 24}}"));
    CK(g_draw == 0);

    cf_begin(TAG, "view", m);
    cb_str(" ");
    cb_str(frame_text != nil ? [frame_text UTF8String] : "(nil)");
    cb_end();
}

static void group_render(void)
{
    unsigned m = 0, bit = 1;
    NSRect bounds = NSMakeRect(0, 0, VIEW_W, VIEW_H);
    cb_uptr self_ptr = (cb_uptr)(__bridge void *)g_view;
    int appkit_flips;

    g_rep = [g_view bitmapImageRepForCachingDisplayInRect:bounds];
    if (g_rep == nil) {
        g_unavailable = "bitmapImageRepForCachingDisplayInRect: returned nil";
        return;
    }
    [g_view cacheDisplayInRect:bounds toBitmapImageRep:g_rep];
    if (g_draw == 0) {
        g_unavailable = "cacheDisplayInRect:toBitmapImageRep: never called drawRect:";
        return;
    }
    g_dev = view_device_rep(VIEW_W, VIEW_H);
    [g_view cacheDisplayInRect:bounds toBitmapImageRep:g_dev];
    appkit_flips = g_flip - g_own_flips;
    if ([g_rep pixelsWide] % VIEW_W == 0)
        g_scale = [g_rep pixelsWide] / VIEW_W;

    CK(g_scale >= 1 && [g_rep pixelsHigh] == VIEW_H * g_scale && [g_rep size].width == VIEW_W && [g_rep size].height == VIEW_H);
    CK(g_dev != nil && [g_dev pixelsWide] == VIEW_W && [g_dev pixelsHigh] == VIEW_H && [g_dev bitsPerPixel] == 32);
    CK(g_draw == 2);
    CK(view_rect_is(g_dirty[0], 0, 0, VIEW_W, VIEW_H) && view_rect_is(g_dirty[1], 0, 0, VIEW_W, VIEW_H));
    CK(g_draw_self[0] == self_ptr && g_draw_self[1] == self_ptr);
    CK(g_ctx_ok[0] && g_ctx_ok[1] && g_ctx_flipped[0] == YES && g_ctx_flipped[1] == YES && g_cg_ok[0] && g_cg_ok[1]);
    CK(appkit_flips > 0);

    cf_begin(TAG, "render", m);
    cf_long("draws", g_draw);
    cf_long("flips", appkit_flips);
    cf_long("scale", g_scale);
    cb_end();
}

static void group_pixels(void)
{
    unsigned m = 0, bit = 1;
    unsigned red = g_scale >= 1 ? view_pixel(g_rep, 3 * g_scale, 3 * g_scale) : 1;
    NSBitmapImageRep *dev = g_dev;

    CK(view_pixel(dev, 0, 0) == BACKGROUND && view_pixel(dev, VIEW_W - 1, VIEW_H - 1) == BACKGROUND);
    CK(view_pixel(dev, 2, 2) == FILL && view_pixel(dev, 11, 7) == FILL);
    CK(view_pixel(dev, 12, 2) == BACKGROUND && view_pixel(dev, 2, 8) == BACKGROUND && view_pixel(dev, 1, 1) == BACKGROUND);
    CK(view_pixel(dev, 2, 17) == BACKGROUND && view_pixel(dev, 11, 21) == BACKGROUND);
    CK(view_pixel(dev, 4, 12) == STROKE && view_pixel(dev, 24, 12) == STROKE && view_pixel(dev, 4, 20) == STROKE &&
       view_pixel(dev, 24, 20) == STROKE && view_pixel(dev, 14, 12) == STROKE && view_pixel(dev, 4, 16) == STROKE);
    CK(view_pixel(dev, 14, 16) == BACKGROUND && view_pixel(dev, 25, 12) == BACKGROUND && view_pixel(dev, 4, 11) == BACKGROUND);
    CK(view_pixel(dev, 20, 2) == CGFILL && view_pixel(dev, 27, 5) == CGFILL && view_pixel(dev, 19, 2) == BACKGROUND &&
       view_pixel(dev, 20, 6) == BACKGROUND && view_pixel(dev, 20, 19) == BACKGROUND);
    CK((red >> 24) > 0xc0 && ((red >> 16) & 0xff) < 0x40 && ((red >> 8) & 0xff) < 0x40 && (red & 0xff) == 0xff);

    cf_begin(TAG, "pixels", m);
    cb_str(" dev=");
    cb_hex(view_checksum(dev));
    cb_str(" rep=");
    cb_hex(view_checksum(g_rep));
    cb_end();
}

int main(void)
{
    int rc;

    @autoreleasepool {
        cf_note(TAG, "view");
        group_view();
        cf_note(TAG, "render");
        group_render();
        if (g_unavailable != 0)
            return view_unavailable(TAG, g_unavailable);
        cf_note(TAG, "pixels");
        group_pixels();
        rc = cf_summary(TAG);
    }
    return rc;
}
EOC

    cat > "$TMP/objc_view_ivar.m" <<'EOC'
#include "appkit_common.h"
#include <objc/runtime.h>

#define TAG "objc_view_ivar"
#define VIEW_W 24
#define VIEW_H 16
#define WHITE 0xffffffffu
#define FILL1 0x3090c0ffu
#define FILL2 0xc06010ffu

@interface OcerzIvarView : NSView
{
@public
    int _marker;
    double _scale;
    NSColor *_fill;
    NSRect _box;
    unsigned char _tail;
}
@end

static int g_init, g_draw;
static NSRect g_init_frame[2];
static cb_uptr g_seen_self[3];
static int g_seen_marker[3];
static double g_seen_scale[3];
static cb_uptr g_seen_fill[3];
static NSRect g_seen_box[3];
static unsigned g_seen_tail[3];
static NSRect g_seen_frame[3];

@implementation OcerzIvarView

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self != nil) {
        _marker = 0x5eed1234;
        _scale = -1.0625;
        _fill = [NSColor colorWithDeviceRed:48 / 255.0 green:144 / 255.0 blue:192 / 255.0 alpha:1.0];
        _box = NSMakeRect(3, 4, 12, 9);
        _tail = 0xa7;
        g_init_frame[g_init < 1 ? g_init : 1] = [self frame];
        g_init++;
    }
    return self;
}

- (BOOL)isFlipped
{
    return YES;
}

- (void)drawRect:(NSRect)dirty
{
    int i = g_draw < 2 ? g_draw : 2;

    g_draw++;
    g_seen_self[i] = (cb_uptr)(__bridge void *)self;
    g_seen_marker[i] = _marker;
    g_seen_scale[i] = _scale;
    g_seen_fill[i] = (cb_uptr)(__bridge void *)_fill;
    g_seen_box[i] = _box;
    g_seen_tail[i] = _tail;
    g_seen_frame[i] = [self frame];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [[NSColor colorWithDeviceRed:1.0 green:1.0 blue:1.0 alpha:1.0] set];
    NSRectFill([self bounds]);
    [_fill set];
    NSRectFill(_box);
}

@end

static OcerzIvarView *g_v1, *g_v2;
static NSBitmapImageRep *g_dev1, *g_dev2;
static const char *g_unavailable;

static cb_uptr view_offset(OcerzIvarView *v, void *field)
{
    return (cb_uptr)field - (cb_uptr)(__bridge void *)v;
}

static void group_layout(void)
{
    unsigned m = 0, bit = 1;
    OcerzIvarView *v1, *v2;
    size_t base, size;
    cb_uptr off_marker, off_scale, off_fill, off_box, off_tail;
    NSRect box;

    g_v1 = [[OcerzIvarView alloc] initWithFrame:NSMakeRect(0, 0, VIEW_W, VIEW_H)];
    g_v2 = [[OcerzIvarView alloc] initWithFrame:NSMakeRect(10, 20, VIEW_W, VIEW_H)];
    v1 = g_v1;
    v2 = g_v2;
    if (v2 != nil) {
        v2->_marker = 0x0badf00d;
        v2->_scale = 2.5;
        v2->_fill = [NSColor colorWithDeviceRed:192 / 255.0 green:96 / 255.0 blue:16 / 255.0 alpha:1.0];
        v2->_box = NSMakeRect(8, 2, 6, 10);
        v2->_tail = 0x3c;
    }
    base = class_getInstanceSize([NSView class]);
    size = class_getInstanceSize([OcerzIvarView class]);
    off_marker = view_offset(v1, &v1->_marker);
    off_scale = view_offset(v1, &v1->_scale);
    off_fill = view_offset(v1, &v1->_fill);
    off_box = view_offset(v1, &v1->_box);
    off_tail = view_offset(v1, &v1->_tail);
    box = [[v1 valueForKey:@"box"] rectValue];

    CK(v1 != nil && v2 != nil && g_init == 2 && [v1 class] == [OcerzIvarView class] && [OcerzIvarView superclass] == [NSView class]);
    CK(base > 8 && off_marker >= base && off_scale > off_marker && off_fill > off_scale && off_box > off_fill && off_tail > off_box);
    CK(size >= off_tail + 1 && size > base);
    CK(view_rect_is(g_init_frame[0], 0, 0, VIEW_W, VIEW_H) && view_rect_is(g_init_frame[1], 10, 20, VIEW_W, VIEW_H));
    CK(view_rect_is([v1 frame], 0, 0, VIEW_W, VIEW_H) && view_rect_is([v2 frame], 10, 20, VIEW_W, VIEW_H) &&
       view_rect_is([v2 bounds], 0, 0, VIEW_W, VIEW_H));
    CK([[v1 valueForKey:@"marker"] intValue] == 0x5eed1234 && [[v2 valueForKey:@"marker"] intValue] == 0x0badf00d);
    CK([[v1 valueForKey:@"scale"] doubleValue] == -1.0625 && [v1 valueForKey:@"fill"] == v1->_fill && view_rect_is(box, 3, 4, 12, 9) &&
       [[v1 valueForKey:@"tail"] unsignedCharValue] == 0xa7);
    [v1 setValue:[NSNumber numberWithInt:0x7ead] forKey:@"marker"];
    CK(v1->_marker == 0x7ead && v2->_marker == 0x0badf00d);
    v1->_marker = 0x5eed1234;

    cf_begin(TAG, "layout", m);
    cf_long("views", g_init);
    cb_end();
}

static void group_draw(void)
{
    unsigned m = 0, bit = 1;
    OcerzIvarView *v1 = g_v1, *v2 = g_v2;
    NSRect bounds = NSMakeRect(0, 0, VIEW_W, VIEW_H);

    g_dev1 = view_device_rep(VIEW_W, VIEW_H);
    g_dev2 = view_device_rep(VIEW_W, VIEW_H);
    if (g_dev1 == nil || g_dev2 == nil) {
        g_unavailable = "no device RGB bitmap could be created";
        return;
    }
    [v1 cacheDisplayInRect:bounds toBitmapImageRep:g_dev1];
    if (g_draw == 0) {
        g_unavailable = "cacheDisplayInRect:toBitmapImageRep: never called drawRect:";
        return;
    }
    [v2 cacheDisplayInRect:bounds toBitmapImageRep:g_dev2];

    CK(g_draw == 2);
    CK(g_seen_self[0] == (cb_uptr)(__bridge void *)v1 && g_seen_self[1] == (cb_uptr)(__bridge void *)v2);
    CK(g_seen_marker[0] == 0x5eed1234 && g_seen_scale[0] == -1.0625);
    CK(g_seen_fill[0] == (cb_uptr)(__bridge void *)v1->_fill && g_seen_fill[0] != 0);
    CK(view_rect_is(g_seen_box[0], 3, 4, 12, 9) && g_seen_tail[0] == 0xa7);
    CK(g_seen_marker[1] == 0x0badf00d && g_seen_scale[1] == 2.5 && g_seen_fill[1] == (cb_uptr)(__bridge void *)v2->_fill &&
       view_rect_is(g_seen_box[1], 8, 2, 6, 10) && g_seen_tail[1] == 0x3c);
    CK(view_rect_is(g_seen_frame[0], 0, 0, VIEW_W, VIEW_H) && view_rect_is(g_seen_frame[1], 10, 20, VIEW_W, VIEW_H));

    cf_begin(TAG, "draw", m);
    cf_long("draws", g_draw);
    cb_end();
}

static void group_pixels(void)
{
    unsigned m = 0, bit = 1;
    NSBitmapImageRep *d1 = g_dev1, *d2 = g_dev2;

    CK(view_pixel(d1, 0, 0) == WHITE && view_pixel(d1, VIEW_W - 1, VIEW_H - 1) == WHITE);
    CK(view_pixel(d1, 3, 4) == FILL1 && view_pixel(d1, 14, 12) == FILL1);
    CK(view_pixel(d1, 2, 4) == WHITE && view_pixel(d1, 15, 12) == WHITE && view_pixel(d1, 3, 13) == WHITE && view_pixel(d1, 3, 3) == WHITE);
    CK(view_pixel(d2, 8, 2) == FILL2 && view_pixel(d2, 13, 11) == FILL2 && view_pixel(d2, 3, 4) == WHITE);
    CK(view_pixel(d2, 7, 2) == WHITE && view_pixel(d2, 14, 11) == WHITE && view_pixel(d2, 8, 12) == WHITE);

    cf_begin(TAG, "pixels", m);
    cb_str(" dev1=");
    cb_hex(view_checksum(d1));
    cb_str(" dev2=");
    cb_hex(view_checksum(d2));
    cb_end();
}

int main(void)
{
    int rc;

    @autoreleasepool {
        cf_note(TAG, "layout");
        group_layout();
        cf_note(TAG, "draw");
        group_draw();
        if (g_unavailable != 0)
            return view_unavailable(TAG, g_unavailable);
        cf_note(TAG, "pixels");
        group_pixels();
        rc = cf_summary(TAG);
    }
    return rc;
}
EOC

    for name in objc_classes objc_view_render objc_view_ivar; do
        framework=Foundation
        case $name in
            objc_view_*) framework=AppKit ;;
        esac
        clang -arch x86_64 -x objective-c -fobjc-arc -O1 -fno-builtin \
                -o "$TMP/$name" "$TMP/$name.m" -framework "$framework" >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            objc_classes) OBJC_CLASSES_BIN="$TMP/$name" ;;
            objc_view_render) OBJC_VIEW_RENDER_BIN="$TMP/$name" ;;
            objc_view_ivar) OBJC_VIEW_IVAR_BIN="$TMP/$name" ;;
        esac
        clang -arch arm64 -x objective-c -fobjc-arc -O1 -fno-builtin \
                -o "$TMP/$name.arm64" "$TMP/$name.m" -framework "$framework" >"$TMP/$name.arm64.cc.log" 2>&1 || continue
        case $name in
            objc_classes) OBJC_CLASSES_ARM64="$TMP/$name.arm64" ;;
            objc_view_render) OBJC_VIEW_RENDER_ARM64="$TMP/$name.arm64" ;;
            objc_view_ivar) OBJC_VIEW_IVAR_ARM64="$TMP/$name.arm64" ;;
        esac
    done
}

build_app_fixtures() {
    local root bundle
    local defs=(-DAPP_ID="\"$APP_ID\"" -DAPP_KEY="\"$APP_KEY\"" -DAPP_VALUE="\"$APP_VALUE\""
                -DAPP_TEXT="\"$APP_TEXT\"" -DAPP_ARG="\"$APP_ARG\"")

    cat > "$TMP/app_bundle.m" <<'EOC'
#include "appkit_common.h"
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach-o/ldsyms.h>

#define TAG "app_bundle"
#define APP_VAR "OCERZ_APP_BUNDLE_VAR"
#define APP_PATH_MAX 1024

extern char **environ;

static int g_argc;
static const char **g_argv;
static int g_will_finish, g_did_finish, g_order_ok;
static id g_delegate;
static char g_exe[APP_PATH_MAX];

static const char *app_leaf(const char *path)
{
    const char *slash = path != 0 ? strrchr(path, '/') : 0;

    return slash != 0 ? slash + 1 : path;
}

static void app_line(const char *what, const char *text)
{
    cb_len = 0;
    cb_str(TAG);
    cb_str(" ");
    cb_str(what);
    if (text != 0) {
        cb_str(" ");
        cb_str(text);
    }
    cb_end();
}

static void group_bundle(void)
{
    unsigned m = 0, bit = 1;
    NSBundle *b = [NSBundle mainBundle];
    NSString *exe = [b executablePath];
    NSString *root = [[[exe stringByDeletingLastPathComponent] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    NSString *rp = [b pathForResource:@"greeting" ofType:@"txt"];
    NSString *text = rp != nil ? [NSString stringWithContentsOfFile:rp encoding:NSUTF8StringEncoding error:NULL] : nil;
    CFBundleRef cb = CFBundleGetMainBundle();
    CFTypeRef cv = cb != 0 ? CFBundleGetValueForInfoDictionaryKey(cb, CFSTR(APP_KEY)) : 0;

    CK(b != nil && objc_text_is([b bundleIdentifier], APP_ID));
    CK(objc_text_is([b objectForInfoDictionaryKey:@APP_KEY], APP_VALUE));
    CK([[b bundlePath] isEqual:root] == YES && [[[b bundlePath] pathExtension] isEqual:@"app"] == YES);
    CK(rp != nil && [rp hasPrefix:[[b resourcePath] stringByAppendingString:@"/"]] == YES && objc_text_is(text, APP_TEXT));
    CK(cb != 0 && cf_text_is(CFBundleGetIdentifier(cb), APP_ID) && cv != 0 && cf_equals_text(cv, APP_VALUE));
    CK([b principalClass] == [NSApplication class]);

    cf_begin(TAG, "bundle", m);
    cb_str(" ");
    cb_str([[[b bundlePath] lastPathComponent] UTF8String] ?: "(nil)");
    cb_str(" ");
    cb_str([[b bundleIdentifier] UTF8String] ?: "(nil)");
    cb_end();
    app_line("resource", text != nil ? [text UTF8String] : "(nil)");
}

static void group_process(void)
{
    unsigned m = 0, bit = 1;
    NSProcessInfo *pi = [NSProcessInfo processInfo];
    NSArray *args = [pi arguments];
    NSRunningApplication *me = [NSRunningApplication currentApplication];
    const char *leaf = app_leaf(g_exe);

    CK(objc_text_is([pi processName], leaf));
    CK(getprogname() != 0 && strcmp(getprogname(), app_leaf(g_argv[0])) == 0);
    CK([args count] == (NSUInteger)g_argc && objc_text_is([args lastObject], APP_ARG) && objc_text_is([[args objectAtIndex:0] lastPathComponent], app_leaf(g_argv[0])));
    CK(getenv("CFProcessPath") == 0);
    CK(me != nil && objc_text_is([me bundleIdentifier], APP_ID) && [me processIdentifier] == [pi processIdentifier]);
    NSLog(@"%s process %s", TAG, leaf);

    cf_begin(TAG, "process", m);
    cb_str(" ");
    cb_str([[pi processName] UTF8String] ?: "(nil)");
    cf_long("args", (long)[args count]);
    cb_end();
}

static void group_crt(void)
{
    unsigned m = 0, bit = 1;
    char small[4];
    uint32_t size = APP_PATH_MAX, tiny = sizeof small;
    int rc_small = _NSGetExecutablePath(small, &tiny);
    char **env;
    int seen = 0;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_NSGetMachExecuteHeader();

    CK(_NSGetExecutablePath(g_exe, &size) == 0 && size == APP_PATH_MAX);
    CK(strcmp(g_exe, [[[NSBundle mainBundle] executablePath] fileSystemRepresentation]) == 0);
    CK(rc_small == -1 && tiny == strlen(g_exe) + 1);
    CK(*_NSGetArgc() == g_argc && *_NSGetArgv() == (char **)g_argv && strcmp((*_NSGetArgv())[g_argc - 1], APP_ARG) == 0);
    setenv(APP_VAR, "set", 1);
    for (env = *_NSGetEnviron(); env != 0 && *env != 0; env++)
        seen += strcmp(*env, APP_VAR "=set") == 0;
    CK(*_NSGetEnviron() == environ && seen == 1);
    CK(*_NSGetProgname() == getprogname());
    CK(mh == &_mh_execute_header && mh->magic == MH_MAGIC_64 && mh->filetype == MH_EXECUTE);

    cf_begin(TAG, "crt", m);
    cb_str(" ");
    cb_str(app_leaf(g_exe));
    cf_long("argc", *_NSGetArgc());
    cb_end();
}

static void group_app(void)
{
    unsigned m = 0, bit = 1;

    CK(g_will_finish == 1 && g_did_finish == 1 && g_order_ok);
    CK(NSApp != nil && NSApp == [NSApplication sharedApplication] && [NSApp class] == [NSApplication class]);
    CK([NSApp delegate] == g_delegate);
    CK([NSApp isRunning] == YES);
    CK([NSApp activationPolicy] == NSApplicationActivationPolicyAccessory);

    cf_begin(TAG, "app", m);
    cf_long("policy", (long)[NSApp activationPolicy]);
    cb_end();
}

@interface OcerzAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation OcerzAppDelegate

- (void)applicationWillFinishLaunching:(NSNotification *)note
{
    g_will_finish++;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    g_did_finish++;
    g_order_ok = g_will_finish == 1 && [note object] == NSApp;
    cf_note(TAG, "bundle");
    group_bundle();
    cf_note(TAG, "crt");
    group_crt();
    cf_note(TAG, "process");
    group_process();
    cf_note(TAG, "app");
    group_app();
    cf_summary(TAG);
    printf("%s buffered until exit\n", TAG);
    cf_note(TAG, "terminating");
    [NSApp terminate:nil];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    app_line("will-terminate", 0);
}

@end

static void app_atexit(void)
{
    app_line("atexit", 0);
}

int main(int argc, const char *argv[])
{
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();

    g_argc = argc;
    g_argv = argv;
    if (session == 0)
        return view_unavailable(TAG, "no window server session");
    CFRelease(session);
    atexit(app_atexit);
    @autoreleasepool {
        cf_note(TAG, "setting up");
        [NSApplication sharedApplication];
        g_delegate = [OcerzAppDelegate new];
        [NSApp setDelegate:g_delegate];
        cf_note(TAG, "calling NSApplicationMain");
    }
    NSApplicationMain(argc, argv);
    app_line("returned from NSApplicationMain", 0);
    return 3;
}
EOC

    clang -arch x86_64 -x objective-c -fobjc-arc -O1 -fno-builtin "${defs[@]}" \
            -o "$TMP/app_bundle" "$TMP/app_bundle.m" -framework AppKit >"$TMP/app_bundle.cc.log" 2>&1 || return
    APP_BUNDLE_BIN="$TMP/app_bundle"
    clang -arch arm64 -x objective-c -fobjc-arc -O1 -fno-builtin "${defs[@]}" \
            -o "$TMP/app_bundle.arm64" "$TMP/app_bundle.m" -framework AppKit >"$TMP/app_bundle.arm64.cc.log" 2>&1 || return
    APP_BUNDLE_ARM64="$TMP/app_bundle.arm64"

    root="$(cd "$TMP" && pwd -P)"
    bundle="$root/$APP_NAME.app"
    mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
    printf '%s' "$APP_TEXT" > "$bundle/Contents/Resources/greeting.txt"
    cat > "$bundle/Contents/Info.plist" <<EOC
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>$APP_NAME</string>
	<key>CFBundleIdentifier</key>
	<string>$APP_ID</string>
	<key>CFBundleName</key>
	<string>$APP_NAME</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
	<key>LSUIElement</key>
	<true/>
	<key>$APP_KEY</key>
	<string>$APP_VALUE</string>
</dict>
</plist>
EOC
    lipo -create -output "$bundle/Contents/MacOS/$APP_NAME" "$APP_BUNDLE_BIN" "$APP_BUNDLE_ARM64" \
            >"$TMP/app_bundle.lipo.log" 2>&1 || return
    APP_BUNDLE_EXE="$bundle/Contents/MacOS/$APP_NAME"
}

build_block_fixtures() {
    local name flags

    BLOCK_EXPORTS='__Block_copy __Block_release __Block_object_assign __Block_object_dispose __NSConcreteGlobalBlock __NSConcreteStackBlock'
    BLOCK_EXPORTS="$BLOCK_EXPORTS _objc_retainBlock _objc_unsafeClaimAutoreleasedReturnValue _qsort_b _bsearch_b"
    BLOCK_EXPORTS="$BLOCK_EXPORTS _dispatch_once _dispatch_sync _dispatch_barrier_sync _dispatch_async _dispatch_after _dispatch_apply"
    BLOCK_EXPORTS="$BLOCK_EXPORTS _dispatch_group_async _dispatch_group_notify _dispatch_group_wait _dispatch_group_create"
    BLOCK_EXPORTS="$BLOCK_EXPORTS _dispatch_get_global_queue _dispatch_queue_create _dispatch_release _dispatch_time"
    BLOCK_EXPORTS="$BLOCK_EXPORTS _dispatch_semaphore_create _dispatch_semaphore_signal _dispatch_semaphore_wait"
    BLOCK_EXPORTS="$BLOCK_EXPORTS _dispatch_block_create _dispatch_block_perform _dispatch_block_wait _dispatch_block_cancel _dispatch_block_testcancel"
    BLOCK_EXPORTS="$BLOCK_EXPORTS _OBJC_CLASS_\$_NSBlockOperation _OBJC_CLASS_\$_NSOperationQueue _OBJC_CLASS_\$_NSItemProvider"
    BLOCK_EXPORTS="$BLOCK_EXPORTS _OBJC_CLASS_\$_NSNotificationCenter _OBJC_CLASS_\$_NSConstantArray _OBJC_CLASS_\$_NSConstantDictionary"
    BLOCK_EXPORTS="$BLOCK_EXPORTS _OBJC_CLASS_\$_NSXPCListener _OBJC_CLASS_\$_NSXPCConnection _OBJC_CLASS_\$_NSXPCInterface _OBJC_CLASS_\$_NSNumber"
    BLOCK_DISPATCH_BIN=""
    BLOCK_DISPATCH_ARM64=""
    BLOCK_RUNTIME_BIN=""
    BLOCK_RUNTIME_ARM64=""
    BLOCK_FOUNDATION_BIN=""
    BLOCK_FOUNDATION_ARM64=""

    cat > "$TMP/block_dispatch.c" <<'EOC'
#include <dispatch/dispatch.h>
#include "cf_common.h"

#define TAG "block_dispatch"
#define WAIT_SECS 20

static int g_once_runs;
static long g_once_value;

static dispatch_time_t bj_deadline(void)
{
    return dispatch_time(DISPATCH_TIME_NOW, (int64_t)WAIT_SECS * (int64_t)NSEC_PER_SEC);
}

static void group_once(void)
{
    unsigned m = 0, bit = 1;
    static dispatch_once_t pred;
    static dispatch_once_t pred2;
    long local = 7;
    int i;

    for (i = 0; i < 3; i++)
        dispatch_once(&pred, ^{ g_once_runs++; g_once_value = 42; });
    dispatch_once(&pred2, ^{ g_once_value += local; });
    dispatch_once(&pred2, ^{ g_once_value += 1000; });

    CK(g_once_runs == 1);
    CK(g_once_value == 49);

    cf_begin(TAG, "once", m);
    cf_long("value", g_once_value);
    cb_end();
}

static void group_sync(void)
{
    unsigned m = 0, bit = 1;
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_queue_t serial = dispatch_queue_create("ocerz.blocks.serial", 0);
    __block int counter = 0;
    int add = 5;

    dispatch_sync(q, ^{ counter += add; });
    CK(counter == 5);
    dispatch_sync(serial, ^{ counter *= 3; });
    CK(counter == 15);
    dispatch_sync(serial, ^{ dispatch_sync(q, ^{ counter += 1; }); });
    CK(counter == 16);
    dispatch_barrier_sync(serial, ^{ counter -= 2; });
    CK(counter == 14);
    dispatch_release(serial);

    cf_begin(TAG, "sync", m);
    cf_long("counter", counter);
    cb_end();
}

static void group_async(void)
{
    unsigned m = 0, bit = 1;
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_group_t g = dispatch_group_create();
    __block int hits = 0;
    __block long gsum = 0;
    __block int notified = 0;
    long waited = 0;
    int i;

    for (i = 0; i < 8; i++)
        dispatch_async(q, ^{ __sync_fetch_and_add(&hits, i + 1); dispatch_semaphore_signal(sem); });
    for (i = 0; i < 8; i++)
        waited |= dispatch_semaphore_wait(sem, bj_deadline());
    CK(waited == 0 && hits == 36);

    for (i = 0; i < 10; i++)
        dispatch_group_async(g, q, ^{ __sync_fetch_and_add(&gsum, (long)i * 10); });
    CK(dispatch_group_wait(g, bj_deadline()) == 0);
    CK(gsum == 450);

    dispatch_group_notify(g, q, ^{ notified = 1; dispatch_semaphore_signal(sem); });
    CK(dispatch_semaphore_wait(sem, bj_deadline()) == 0 && notified == 1);

    dispatch_release(g);
    dispatch_release(sem);

    cf_begin(TAG, "async", m);
    cf_long("hits", hits);
    cf_long("sum", gsum);
    cb_end();
}

static void group_after(void)
{
    unsigned m = 0, bit = 1;
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block int fired = 0;
    int early;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * (int64_t)NSEC_PER_MSEC), q,
                   ^{ fired = 1; dispatch_semaphore_signal(sem); });
    early = fired;
    CK(early == 0);
    CK(dispatch_semaphore_wait(sem, bj_deadline()) == 0 && fired == 1);
    dispatch_release(sem);

    cf_begin(TAG, "after", m);
    cf_long("fired", fired);
    cb_end();
}

static void group_byref(void)
{
    unsigned m = 0, bit = 1;
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block long acc = 1;
    long mult = 3;
    int i;

    for (i = 0; i < 4; i++)
        dispatch_sync(q, ^{ acc = acc * mult + i; });
    CK(acc == 99);
    dispatch_async(q, ^{ acc += 1; dispatch_semaphore_signal(sem); });
    CK(dispatch_semaphore_wait(sem, bj_deadline()) == 0);
    CK(acc == 100);
    acc += 5;
    dispatch_sync(q, ^{ acc *= 2; });
    CK(acc == 210);
    dispatch_release(sem);

    cf_begin(TAG, "byref", m);
    cf_long("acc", acc);
    cb_end();
}

static void group_apply(void)
{
    unsigned m = 0, bit = 1;
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    static long squares[256];
    __block long sum = 0;
    long total = 0;
    long base = 3;
    int i;

    dispatch_apply(1000, q, ^(size_t k) { __sync_fetch_and_add(&sum, (long)k); });
    CK(sum == 499500);
    dispatch_apply(256, DISPATCH_APPLY_AUTO, ^(size_t k) { squares[k] = (long)(k * k) + base; });
    for (i = 0; i < 256; i++)
        total += squares[i];
    CK(total == 5559680 + 256 * 3);

    cf_begin(TAG, "apply", m);
    cf_long("sum", sum);
    cf_long("squares", total);
    cb_end();
}

int main(void)
{
    cf_note(TAG, "once");
    group_once();
    cf_note(TAG, "sync");
    group_sync();
    cf_note(TAG, "async");
    group_async();
    cf_note(TAG, "after");
    group_after();
    cf_note(TAG, "byref");
    group_byref();
    cf_note(TAG, "apply");
    group_apply();
    return cf_summary(TAG);
}
EOC

    cat > "$TMP/block_runtime.c" <<'EOC'
#include <Block.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include "cf_common.h"

#define TAG "block_runtime"

typedef int (^IntFn)(int);

static IntFn make_adder(int n)
{
    return Block_copy(^(int x) { return x + n; });
}

static void group_copy(void)
{
    unsigned m = 0, bit = 1;
    IntFn a = make_adder(10), b = make_adder(20), a2, g, inner, outer_stack, outer;
    int r1, r2, r3, r4;

    CK(a != 0 && b != 0 && a != b);
    r1 = a(1);
    r2 = b(2);
    CK(r1 == 11 && r2 == 22);
    a2 = Block_copy(a);
    CK(a2 == a);
    Block_release(a2);
    r3 = a(5);
    CK(r3 == 15);
    Block_release(a);
    Block_release(b);

    g = ^(int x) { return x * 2; };
    CK(Block_copy(g) == g);
    Block_release(g);

    inner = make_adder(3);
    outer_stack = ^(int x) { return inner(x) * 2; };
    outer = Block_copy(outer_stack);
    CK(outer != outer_stack);
    Block_release(inner);
    r4 = outer(4);
    CK(r4 == 14);
    Block_release(outer);

    cf_begin(TAG, "copy", m);
    cf_long("sum", r1 + r2 + r3 + r4);
    cb_end();
}

static void group_byref(void)
{
    unsigned m = 0, bit = 1;
    __block int shared = 0;
    __block IntFn slot = make_adder(100);
    void (^inc)(void) = Block_copy(^{ shared += 1; });
    void (^dbl)(void) = Block_copy(^{ shared *= 2; });
    void (^swap)(void) = Block_copy(^{ IntFn old = slot; slot = make_adder(200); Block_release(old); });
    int got;

    inc();
    dbl();
    inc();
    CK(shared == 3);
    shared += 10;
    dbl();
    CK(shared == 26);
    Block_release(inc);
    Block_release(dbl);

    swap();
    got = slot(1);
    CK(got == 201);
    Block_release(swap);
    Block_release(slot);

    cf_begin(TAG, "byref", m);
    cf_long("shared", shared);
    cf_long("slot", got);
    cb_end();
}

static void group_sort(void)
{
    unsigned m = 0, bit = 1;
    static int v[64];
    __block int compares = 0;
    int i, sorted = 1, found = 0;
    unsigned seed = 12345;

    for (i = 0; i < 64; i++) {
        seed = seed * 1103515245u + 12345u;
        v[i] = (int)((seed >> 8) % 100000u) * 64 + i;
    }
    qsort_b(v, 64, sizeof v[0], ^int(const void *x, const void *y) {
        int p = *(const int *)x, q = *(const int *)y;
        compares++;
        return p < q ? -65536 : p > q ? 65536 : 0;
    });
    for (i = 1; i < 64; i++)
        sorted &= v[i - 1] < v[i];
    CK(sorted && compares > 0);
    for (i = 0; i < 64; i++) {
        int key = v[i];
        int *hit = bsearch_b(&key, v, 64, sizeof v[0], ^int(const void *x, const void *y) {
            int p = *(const int *)x, q = *(const int *)y;
            return p < q ? -1 : p > q ? 1 : 0;
        });
        found += hit == &v[i];
    }
    CK(found == 64);

    cf_begin(TAG, "sort", m);
    cf_long("first", v[0]);
    cf_long("last", v[63]);
    cb_end();
}

static void group_result(void)
{
    unsigned m = 0, bit = 1;
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    __block int ran = 0;
    int add = 4;
    dispatch_block_t db = dispatch_block_create(0, ^{ ran += add; });
    dispatch_block_t dw = dispatch_block_create(0, ^{ ran += add * 2; });
    dispatch_block_t dc = dispatch_block_create(0, ^{ ran += 1000; });

    CK(db != 0 && dw != 0 && dc != 0);
    db();
    CK(ran == 4);
    db();
    CK(ran == 8);
    dispatch_async(q, dw);
    CK(dispatch_block_wait(dw, dispatch_time(DISPATCH_TIME_NOW, 20 * (int64_t)NSEC_PER_SEC)) == 0);
    CK(ran == 16);
    dispatch_block_perform(0, ^{ ran += 100; });
    CK(ran == 116);
    dispatch_block_cancel(dc);
    CK(dispatch_block_testcancel(dc) != 0 && dispatch_block_testcancel(db) == 0);
    dispatch_sync(q, dc);
    CK(ran == 116);
    Block_release(db);
    Block_release(dw);
    Block_release(dc);

    cf_begin(TAG, "result", m);
    cf_long("ran", ran);
    cb_end();
}

int main(void)
{
    cf_note(TAG, "copy");
    group_copy();
    cf_note(TAG, "byref");
    group_byref();
    cf_note(TAG, "sort");
    group_sort();
    cf_note(TAG, "result");
    group_result();
    return cf_summary(TAG);
}
EOC

    cat > "$TMP/block_foundation.m" <<'EOC'
#include "objc_common.h"

#define TAG "block_foundation"

static int g_live;

@interface OcerzTracked : NSObject
@property (nonatomic) long value;
@end

@implementation OcerzTracked
- (instancetype)init
{
    if ((self = [super init]))
        g_live++;
    return self;
}
- (void)dealloc
{
    g_live--;
}
@end

static int g_writer_calls;

@interface OcerzWriter : NSObject <NSItemProviderWriting>
@end

@implementation OcerzWriter
+ (NSArray<NSString *> *)writableTypeIdentifiersForItemProvider
{
    return @[ @"public.utf8-plain-text" ];
}
- (NSProgress *)loadDataWithTypeIdentifier:(NSString *)typeIdentifier
          forItemProviderCompletionHandler:(void (^)(NSData *, NSError *))completionHandler
{
    g_writer_calls++;
    completionHandler([@"written by the guest" dataUsingEncoding:NSUTF8StringEncoding], nil);
    return nil;
}
@end

static dispatch_time_t bf_deadline(void)
{
    return dispatch_time(DISPATCH_TIME_NOW, 20 * (int64_t)NSEC_PER_SEC);
}

static void group_enumerate(void)
{
    unsigned m = 0, bit = 1;
    NSArray *a = @[ @5, @3, @9, @1, @7 ];
    NSDictionary *d = @{ @"a" : @1, @"b" : @2, @"c" : @4 };
    __block long total = 0, dsum = 0, csum = 0;
    __block unsigned long seen = 0;
    NSIndexSet *big;

    [a enumerateObjectsUsingBlock:^(id o, NSUInteger i, BOOL *stop) {
        total += [o longValue];
        seen++;
        if (i == 2)
            *stop = YES;
    }];
    CK(total == 17 && seen == 3);
    [d enumerateKeysAndObjectsUsingBlock:^(id k, id v, BOOL *stop) {
        dsum += [v longValue] * (long)[k length];
    }];
    CK(dsum == 7);
    big = [a indexesOfObjectsPassingTest:^BOOL(id o, NSUInteger i, BOOL *stop) {
        return [o intValue] > 4;
    }];
    CK(big.count == 3 && [big containsIndex:0] && [big containsIndex:2] && [big containsIndex:4]);
    [a enumerateObjectsWithOptions:NSEnumerationConcurrent usingBlock:^(id o, NSUInteger i, BOOL *stop) {
        __sync_fetch_and_add(&csum, [o longValue]);
    }];
    CK(csum == 25);

    cf_begin(TAG, "enumerate", m);
    cf_long("total", total);
    cf_long("seen", (long)seen);
    cf_long("concurrent", csum);
    cb_end();
}

static void group_sort(void)
{
    unsigned m = 0, bit = 1;
    NSArray *a = @[ @5, @3, @9, @1, @7 ];
    NSMutableArray *mut = [a mutableCopy];
    __block int calls = 0;
    NSArray *up = [a sortedArrayUsingComparator:^NSComparisonResult(id x, id y) {
        calls++;
        return [x compare:y];
    }];
    NSArray *down = [a sortedArrayUsingComparator:^NSComparisonResult(id x, id y) {
        return [y compare:x];
    }];

    [mut sortUsingComparator:^NSComparisonResult(id x, id y) {
        long p = [x longValue] % 3, q = [y longValue] % 3;
        return p < q ? NSOrderedAscending : p > q ? NSOrderedDescending : [x compare:y];
    }];
    CK(objc_text_is([up componentsJoinedByString:@","], "1,3,5,7,9") && calls > 0);
    CK(objc_text_is([down componentsJoinedByString:@","], "9,7,5,3,1"));
    CK(objc_text_is([mut componentsJoinedByString:@","], "3,9,1,7,5"));

    cf_begin(TAG, "sort", m);
    cb_end();
    printf("%s|%s\n", [[up componentsJoinedByString:@","] UTF8String], [[mut componentsJoinedByString:@","] UTF8String]);
    fflush(stdout);
}

static void group_notify(void)
{
    unsigned m = 0, bit = 1;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    __block int notes = 0;
    __block int wrong = 0;
    id token;

    @autoreleasepool {
        OcerzTracked *t = [OcerzTracked new];
        t.value = 1;
        token = [nc addObserverForName:@"OcerzBlockNote" object:nil queue:nil usingBlock:^(NSNotification *n) {
            notes += [n.userInfo[@"k"] intValue] * (int)t.value;
            if (![n.name isEqualToString:@"OcerzBlockNote"])
                wrong++;
        }];
    }
    [nc postNotificationName:@"OcerzBlockNote" object:nil userInfo:@{ @"k" : @3 }];
    [nc postNotificationName:@"OcerzBlockNote" object:nil userInfo:@{ @"k" : @4 }];
    CK(notes == 7 && wrong == 0);
    CK(g_live == 1);
    [nc removeObserver:token];
    token = nil;
    [nc postNotificationName:@"OcerzBlockNote" object:nil userInfo:@{ @"k" : @100 }];
    CK(notes == 7);

    cf_begin(TAG, "notify", m);
    cf_long("notes", notes);
    cb_end();
}

static void group_lifetime(void)
{
    unsigned m = 0, bit = 1;
    long seen = 0;

    @autoreleasepool {
        OcerzTracked *t = [OcerzTracked new];
        NSMutableArray *keep = [NSMutableArray array];
        t.value = 11;
        void (^bump)(void) = ^{ t.value += 1; };
        dispatch_sync(dispatch_get_global_queue(0, 0), bump);
        [keep addObject:bump];
        void (^back)(void) = keep[0];
        back();
        dispatch_group_t g = dispatch_group_create();
        dispatch_group_async(g, dispatch_get_global_queue(0, 0), bump);
        CK(dispatch_group_wait(g, bf_deadline()) == 0);
        [keep removeAllObjects];
        seen = t.value;
        CK(seen == 14);
        CK(g_live == 1);
    }
    CK(g_live == 0);

    cf_begin(TAG, "lifetime", m);
    cf_long("value", seen);
    cf_long("live", g_live);
    cb_end();
}

static void group_provider(void)
{
    unsigned m = 0, bit = 1;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block NSString *got = nil;
    __block int completions = 0;

    @autoreleasepool {
        NSItemProvider *p = [[NSItemProvider alloc] initWithObject:[OcerzWriter new]];
        [p loadDataRepresentationForTypeIdentifier:@"public.utf8-plain-text"
                                 completionHandler:^(NSData *d, NSError *e) {
            got = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
            completions++;
            dispatch_semaphore_signal(sem);
        }];
    }
    CK(dispatch_semaphore_wait(sem, bf_deadline()) == 0);
    CK(g_writer_calls == 1 && completions == 1);
    CK(objc_text_is(got, "written by the guest"));

    cf_begin(TAG, "provider", m);
    cf_long("calls", g_writer_calls);
    cb_end();
}

static void group_operation(void)
{
    unsigned m = 0, bit = 1;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSOperationQueue *queue = [NSOperationQueue new];
    __block long work = 0;
    __block int completed = 0;
    NSBlockOperation *op = [NSBlockOperation blockOperationWithBlock:^{ __sync_fetch_and_add(&work, 1); }];
    void (^done)(void) = ^{ completed++; dispatch_semaphore_signal(sem); };

    [op addExecutionBlock:^{ __sync_fetch_and_add(&work, 10); }];
    op.completionBlock = done;
    [queue addOperation:op];
    [queue addOperationWithBlock:^{ __sync_fetch_and_add(&work, 100); }];
    [queue waitUntilAllOperationsAreFinished];
    CK(dispatch_semaphore_wait(sem, bf_deadline()) == 0);
    CK(work == 111 && completed == 1);
    NSBlockOperation *idle = [NSBlockOperation new];
    idle.completionBlock = done;
    void (^again)(void) = idle.completionBlock;
    CK(again != nil);
    if (again)
        again();
    CK(completed == 2);
    idle.completionBlock = nil;
    CK(idle.completionBlock == nil);

    cf_begin(TAG, "operation", m);
    cf_long("work", work);
    cf_long("completed", completed);
    cb_end();
}

@protocol OcerzDoubler
- (void)doubleAll:(NSArray *)values reply:(void (^)(NSArray *doubled, long count))reply;
@end

static int g_served;

@interface OcerzDoublerService : NSObject <OcerzDoubler, NSXPCListenerDelegate>
@end

@implementation OcerzDoublerService
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
    connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(OcerzDoubler)];
    connection.exportedObject = self;
    [connection resume];
    return YES;
}
- (void)doubleAll:(NSArray *)values reply:(void (^)(NSArray *, long))reply
{
    NSMutableArray *out = [NSMutableArray array];
    for (NSNumber *n in values)
        [out addObject:@([n longValue] * 2)];
    g_served++;
    reply(out, (long)out.count);
}
@end

static void group_xpc(void)
{
    unsigned m = 0, bit = 1;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block NSString *got = nil;
    __block long count = -1;
    __block int errors = 0;
    OcerzDoublerService *service = [OcerzDoublerService new];
    NSXPCListener *listener = [NSXPCListener anonymousListener];
    NSXPCConnection *c;

    listener.delegate = service;
    [listener resume];
    c = [[NSXPCConnection alloc] initWithListenerEndpoint:listener.endpoint];
    c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(OcerzDoubler)];
    [c resume];
    id proxy = [c remoteObjectProxyWithErrorHandler:^(NSError *e) {
        errors++;
        dispatch_semaphore_signal(sem);
    }];
    [proxy doubleAll:@[ @1, @2, @21 ] reply:^(NSArray *doubled, long n) {
        got = [doubled componentsJoinedByString:@","];
        count = n;
        dispatch_semaphore_signal(sem);
    }];
    CK(dispatch_semaphore_wait(sem, bf_deadline()) == 0);
    CK(errors == 0 && g_served == 1);
    CK(objc_text_is(got, "2,4,42") && count == 3);
    [c invalidate];
    [listener invalidate];

    cf_begin(TAG, "xpc", m);
    cf_long("count", count);
    cb_end();
}

int main(void)
{
    int rc;

    @autoreleasepool {
        cf_note(TAG, "enumerate");
        group_enumerate();
        cf_note(TAG, "sort");
        group_sort();
        cf_note(TAG, "notify");
        group_notify();
        cf_note(TAG, "lifetime");
        group_lifetime();
        cf_note(TAG, "provider");
        group_provider();
        cf_note(TAG, "operation");
        group_operation();
        cf_note(TAG, "xpc");
        group_xpc();
        rc = cf_summary(TAG);
    }
    return rc;
}
EOC

    for name in block_dispatch block_runtime block_foundation; do
        if [ "$name" = block_foundation ]; then
            flags="-x objective-c -fobjc-arc -O1 -fno-builtin $TMP/$name.m -framework Foundation"
        else
            flags="-std=c11 -O1 -fno-builtin $TMP/$name.c"
        fi
        clang -arch x86_64 -o "$TMP/$name" $flags >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            block_dispatch) BLOCK_DISPATCH_BIN="$TMP/$name" ;;
            block_runtime) BLOCK_RUNTIME_BIN="$TMP/$name" ;;
            block_foundation) BLOCK_FOUNDATION_BIN="$TMP/$name" ;;
        esac
        clang -arch arm64 -o "$TMP/$name.arm64" $flags >"$TMP/$name.arm64.cc.log" 2>&1 || continue
        case $name in
            block_dispatch) BLOCK_DISPATCH_ARM64="$TMP/$name.arm64" ;;
            block_runtime) BLOCK_RUNTIME_ARM64="$TMP/$name.arm64" ;;
            block_foundation) BLOCK_FOUNDATION_ARM64="$TMP/$name.arm64" ;;
        esac
    done
}

build_dl_fixtures() {
    local arch dir

    cat > "$TMP/dl_basic.m" <<'EOC'
#include "objc_common.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdlib.h>

#define TAG "dl_basic"

typedef struct { unsigned platform, version; } dl_version;
extern unsigned dyld_get_program_sdk_version(void);
extern unsigned dyld_get_program_min_os_version(void);
extern unsigned dyld_get_active_platform(void);
extern bool dyld_program_sdk_at_least(dl_version);
extern const char *dyld_image_path_containing_address(const void *);
extern bool _dyld_is_memory_immutable(const void *, size_t);
extern const struct mach_header *_dyld_get_prog_image_header(void);

@interface DLHostBase : NSObject
- (int)hostValue;
- (NSString *)describe;
@end

@implementation DLHostBase
- (int)hostValue
{
    return 40;
}
- (NSString *)describe
{
    return @"host";
}
@end

@interface NSString (DLPlug)
- (NSString *)dlplugShout;
@end

@interface NSObject (DLPlugThing)
- (int)thingValue;
@end

int dl_main_marker(int x)
{
    return x * 3;
}

static char g_dir[1024];
static unsigned g_adds, g_adds_at_register, g_count_at_register;
static const void *g_seen[1024];
static void *g_plug, *g_plug_add;
static const void *g_plug_hdr, *g_bundle_hdr;

static void on_add(const struct mach_header *mh, intptr_t slide)
{
    (void)slide;
    if (g_adds < 1024)
        g_seen[g_adds] = mh;
    g_adds++;
}

static int saw(const void *mh)
{
    unsigned i;
    for (i = 0; mh && i < g_adds && i < 1024; i++)
        if (g_seen[i] == mh)
            return 1;
    return 0;
}

static int ends_with(const char *s, const char *tail)
{
    cb_size a = s ? strlen(s) : 0, b = strlen(tail);
    return s && a >= b && strcmp(s + a - b, tail) == 0;
}

static const char *path_of(const char *leaf, char *buf, cb_size n)
{
    snprintf(buf, n, "%s/%s", g_dir, leaf);
    return buf;
}

static void group_images(void)
{
    unsigned m = 0, bit = 1, n = _dyld_image_count(), i, all = 1;
    const struct mach_header *h0 = _dyld_get_image_header(0);
    Dl_info info;

    memset(&info, 0, sizeof info);
    for (i = 1; i < n; i++)
        if (!_dyld_get_image_header(i) || !_dyld_get_image_name(i))
            all = 0;
    CK(n > 1 && h0 != 0);
    CK(dladdr((void *)dl_main_marker, &info) && info.dli_fbase == h0);
    CK(_dyld_get_prog_image_header() == h0);
    CK(ends_with(_dyld_get_image_name(0), "/" TAG));
    CK(_dyld_get_image_header_containing_address((void *)dl_main_marker) == h0);
    CK(_dyld_get_image_header(n) == 0 && _dyld_get_image_name(n) == 0);
    CK(all);
    CK(ends_with(dyld_image_path_containing_address((void *)dl_main_marker), "/" TAG));
    CK(_dyld_get_image_vmaddr_slide(0) == (intptr_t)h0 - (intptr_t)0x100000000);
    cf_begin(TAG, "images", m);
    cb_end();
}

static void *tls_thread(void *arg)
{
    int *(*addr)(void) = (int *(*)(void))arg;
    int first = *addr();

    *addr() = 9;
    return (void *)(long)(first == 7 && *addr() == 9);
}

static void group_load(void)
{
    unsigned m = 0, bit = 1;
    char path[1200];
    Class thing;
    int *(*tls)(void);
    pthread_t t;
    void *r = 0;
    int joined = 0;

    g_plug = dlopen(path_of("libdlplug.dylib", path, sizeof path), RTLD_NOW);
    thing = NSClassFromString(@"DLPlugThing");
    tls = g_plug ? (int *(*)(void))dlsym(g_plug, "dlplug_tls_addr") : 0;
    CK(g_plug != 0);
    CK(thing != nil && [[thing new] thingValue] == 99);
    CK([[@"abc" dlplugShout] isEqualToString:@"ABC!"]);
    CK([[[NSString stringWithUTF8String:"xyz"] dlplugShout] isEqualToString:@"XYZ!"]);
    CK(tls != 0 && *tls() == 7);
    if (tls != 0) {
        *tls() = 8;
        joined = pthread_create(&t, 0, tls_thread, (void *)tls) == 0 && pthread_join(t, &r) == 0;
    }
    CK(joined && r != 0);
    CK(tls != 0 && *tls() == 8);
    cf_begin(TAG, "load", m);
    cb_end();
}

static void group_order(void)
{
    unsigned m = 0, bit = 1;
    const char *(*order)(void) = g_plug ? (const char *(*)(void))dlsym(g_plug, "dlplug_order") : 0;

    CK(order != 0 && strcmp(order(), "LC") == 0);
    cf_begin(TAG, "order", m);
    cb_end();
}

static void group_sym(void)
{
    unsigned m = 0, bit = 1;
    int (*add)(int, int) = g_plug ? (int (*)(int, int))dlsym(g_plug, "dlplug_add") : 0;
    int *data = g_plug ? (int *)dlsym(g_plug, "dlplug_data") : 0;
    int (*get)(void) = g_plug ? (int (*)(void))dlsym(g_plug, "dlplug_get_data") : 0;

    g_plug_add = (void *)add;
    CK(add != 0 && add(2, 3) == 5);
    CK(data != 0 && *data == 1234);
    if (data != 0)
        *data = 4321;
    CK(get != 0 && get() == 4321);
    CK(g_plug != 0 && dlsym(g_plug, "dlplug_nope") == 0);
    CK(add != 0 && dlsym(RTLD_DEFAULT, "dlplug_add") == (void *)add);
    CK(dlsym(RTLD_SELF, "dl_main_marker") == (void *)dl_main_marker);
    CK(dlsym(RTLD_MAIN_ONLY, "dl_main_marker") == (void *)dl_main_marker);
    CK(dlsym(RTLD_MAIN_ONLY, "dlplug_add") == 0);
    CK(dlsym(RTLD_DEFAULT, "dl_no_such_symbol_anywhere") == 0);
    dlerror();
    cf_begin(TAG, "sym", m);
    cb_end();
}

static void *dlerror_thread(void *arg)
{
    const char *before = dlerror(), *mine;
    unsigned ok;

    (void)arg;
    dlsym(RTLD_DEFAULT, "dl_thread_nope");
    mine = dlerror();
    ok = before == 0 && mine != 0 && strstr(mine, "dl_thread_nope") != 0 && dlerror() == 0;
    return (void *)(long)ok;
}

static void group_dlerror(void)
{
    unsigned m = 0, bit = 1;
    char path[1200];
    const char *e;
    pthread_t t;
    void *r = 0;
    int joined;

    dlerror();
    CK(dlerror() == 0);
    dlsym(g_plug, "dlplug_nope");
    e = dlerror();
    CK(e != 0 && strstr(e, "dlplug_nope") != 0);
    CK(dlerror() == 0);
    CK(dlopen(path_of("libdlnope.dylib", path, sizeof path), RTLD_NOW) == 0);
    e = dlerror();
    CK(e != 0 && strstr(e, "libdlnope.dylib") != 0);
    CK(dlerror() == 0);
    dlsym(g_plug, "dlplug_nope2");
    joined = pthread_create(&t, 0, dlerror_thread, 0) == 0 && pthread_join(t, &r) == 0;
    CK(joined && r != 0);
    e = dlerror();
    CK(e != 0 && strstr(e, "dlplug_nope2") != 0 && strstr(e, "dl_thread_nope") == 0);
    cf_begin(TAG, "dlerror", m);
    cb_end();
}

static void group_path(void)
{
    unsigned m = 0, bit = 1;
    char path[1200], other[1200], nope[1200];

    path_of("libdlplug.dylib", path, sizeof path);
    path_of("libdlother.dylib", other, sizeof other);
    path_of("libdlnope.dylib", nope, sizeof nope);
    CK(g_plug != 0 && dlopen(path, RTLD_NOW) == g_plug);
    CK(g_plug != 0 && dlopen(path, RTLD_NOLOAD) == g_plug);
    CK(dlopen(other, RTLD_NOLOAD) == 0);
    CK(dlopen(nope, RTLD_NOW) == 0);
    CK(dlopen_preflight(path) && !dlopen_preflight(nope));
    CK(g_plug != 0 && dlclose(g_plug) == 0 && dlclose(g_plug) == 0);
    CK(g_plug != 0 && dlsym(g_plug, "dlplug_add") == g_plug_add);
    dlerror();
    cf_begin(TAG, "path", m);
    cb_end();
}

static void group_rpath(void)
{
    unsigned m = 0, bit = 1;

    CK(g_plug != 0 && dlopen("@executable_path/libdlplug.dylib", RTLD_NOW) == g_plug);
    CK(g_plug != 0 && dlopen("@rpath/libdlplug.dylib", RTLD_NOW) == g_plug);
    CK(g_plug != 0 && dlopen("@loader_path/libdlplug.dylib", RTLD_NOW) == g_plug);
    CK(dlopen("@rpath/libdlnope.dylib", RTLD_NOW) == 0);
    dlerror();
    cf_begin(TAG, "rpath", m);
    cb_end();
}

static void group_handles(void)
{
    unsigned m = 0, bit = 1;
    char path[1200];
    void *self = dlopen(0, RTLD_NOW);
    void *first = dlopen(path_of("libdlplug.dylib", path, sizeof path), RTLD_NOW | RTLD_FIRST);

    CK(self != 0 && dlsym(self, "dl_main_marker") == (void *)dl_main_marker);
    CK(self != 0 && g_plug_add != 0 && dlsym(self, "dlplug_add") == g_plug_add);
    CK(first != 0 && g_plug_add != 0 && dlsym(first, "dlplug_add") == g_plug_add);
    CK(first != 0 && dlsym(first, "strlen") == 0);
    CK(g_plug != 0 && dlsym(g_plug, "strlen") == (void *)strlen);
    CK(dlsym(RTLD_NEXT, "dl_main_marker") == 0);
    CK(dlsym(RTLD_NEXT, "strlen") == (void *)strlen);
    CK(first != 0 && self != 0 && dlclose(first) == 0 && dlclose(self) == 0);
    dlerror();
    CK(dlclose((void *)0x1234) == -1 && dlerror() != 0);
    CK(dlsym((void *)0x1234, "strlen") == 0 && dlerror() != 0);
    cf_begin(TAG, "handles", m);
    cb_end();
}

static void group_local(void)
{
    unsigned m = 0, bit = 1;
    char path[1200];
    void *o = dlopen(path_of("libdlother.dylib", path, sizeof path), RTLD_NOW | RTLD_LOCAL);
    void *fn = o != 0 ? dlsym(o, "dlother") : 0;
    void *hidden = dlsym(RTLD_DEFAULT, "dlother");
    void *o2 = dlopen(path, RTLD_NOW | RTLD_GLOBAL);

    CK(o != 0);
    CK(fn != 0);
    CK(hidden == 0);
    CK(o2 == o);
    CK(fn != 0 && dlsym(RTLD_DEFAULT, "dlother") == fn);
    dlerror();
    cf_begin(TAG, "local", m);
    cb_end();
}

static void group_strlen(void)
{
    unsigned m = 0, bit = 1;
    cb_size (*len)(const char *) = (cb_size (*)(const char *))dlsym(RTLD_DEFAULT, "strlen");

    CK(len != 0);
    CK(len != 0 && len("twelve chars") == 12);
    CK((void *)len == (void *)strlen);
    cf_begin(TAG, "strlen", m);
    cb_end();
}

static void group_dladdr(void)
{
    unsigned m = 0, bit = 1, n = _dyld_image_count(), i, idx = 0;
    Dl_info info;

    memset(&info, 0, sizeof info);
    CK(g_plug_add != 0 && dladdr(g_plug_add, &info));
    CK(info.dli_sname != 0 && strcmp(info.dli_sname, "dlplug_add") == 0 && info.dli_saddr == g_plug_add);
    CK(ends_with(info.dli_fname, "/libdlplug.dylib"));
    g_plug_hdr = info.dli_fbase;
    CK(g_plug_hdr != 0 && _dyld_get_image_header_containing_address(g_plug_add) == g_plug_hdr);
    CK(ends_with(dyld_image_path_containing_address(g_plug_add), "/libdlplug.dylib"));
    memset(&info, 0, sizeof info);
    CK(g_plug_add != 0 && dladdr((char *)g_plug_add + 1, &info) && info.dli_saddr == g_plug_add);
    memset(&info, 0, sizeof info);
    CK(dladdr((void *)dl_main_marker, &info) && info.dli_sname != 0 &&
       strcmp(info.dli_sname, "dl_main_marker") == 0);
    for (i = 1; i < n && !idx; i++)
        if (g_plug_hdr != 0 && (const void *)_dyld_get_image_header(i) == g_plug_hdr)
            idx = i;
    CK(idx != 0 && ends_with(_dyld_get_image_name(idx), "/libdlplug.dylib") &&
       _dyld_get_image_vmaddr_slide(idx) == (intptr_t)g_plug_hdr);
    cf_begin(TAG, "dladdr", m);
    cb_end();
}

static void group_plugin(void)
{
    unsigned m = 0, bit = 1;
    char path[1200];
    void *b = dlopen(path_of("dlbundle.bundle", path, sizeof path), RTLD_NOW);
    Class p = NSClassFromString(@"DLPlugin");
    id o = p != nil ? [p new] : nil;
    void *cls = b != 0 ? dlsym(b, "OBJC_CLASS_$_DLPlugin") : 0;

    g_bundle_hdr = cls != 0 ? _dyld_get_image_header_containing_address(cls) : 0;
    CK(b != 0);
    CK(p != nil && [p superclass] == [DLHostBase class]);
    CK(o != nil && [o hostValue] == 42);
    CK(o != nil && [[o describe] isEqualToString:@"plugin<host>"]);
    CK(o != nil && [o isKindOfClass:[DLHostBase class]]);
    CK(cls != 0 && cls == (__bridge void *)p);
    cf_begin(TAG, "plugin", m);
    cb_end();
}

static void group_callbacks(void)
{
    unsigned m = 0, bit = 1, n = _dyld_image_count(), i, all = 1;

    for (i = 0; i < n; i++)
        if (!saw(_dyld_get_image_header(i)))
            all = 0;
    CK(g_adds_at_register == g_count_at_register);
    CK(saw(_dyld_get_image_header(0)));
    CK(saw(g_plug_hdr));
    CK(saw(g_bundle_hdr));
    CK(g_adds == n);
    CK(all);
    cf_begin(TAG, "callbacks", m);
    cb_end();
}

static void group_version(void)
{
    unsigned m = 0, bit = 1;
    char path[1200];
    dl_version old = { 1, 0x000a0e00 }, future = { 1, 0x007f0000 };
    void *heap = malloc(64);

    CK(dyld_get_active_platform() == 1);
    CK(dyld_get_program_min_os_version() == 0x000c0000);
    CK(dyld_get_program_sdk_version() >= 0x000c0000);
    CK(dyld_program_sdk_at_least(old) && !dyld_program_sdk_at_least(future));
    CK(_dyld_shared_cache_contains_path("/usr/lib/libSystem.B.dylib"));
    CK(!_dyld_shared_cache_contains_path(path_of("libdlplug.dylib", path, sizeof path)));
    CK(heap != 0 && !_dyld_is_memory_immutable(heap, 16));
    free(heap);
    cf_begin(TAG, "version", m);
    cb_field("sdk", dyld_get_program_sdk_version(), 1);
    cb_end();
}

int main(void)
{
    int rc;
    char *slash;

    @autoreleasepool {
        snprintf(g_dir, sizeof g_dir, "%s", _dyld_get_image_name(0));
        slash = strrchr(g_dir, '/');
        if (slash != 0)
            *slash = 0;
        g_count_at_register = _dyld_image_count();
        _dyld_register_func_for_add_image(on_add);
        g_adds_at_register = g_adds;
        cf_note(TAG, "images");
        group_images();
        cf_note(TAG, "load");
        group_load();
        cf_note(TAG, "order");
        group_order();
        cf_note(TAG, "sym");
        group_sym();
        cf_note(TAG, "dlerror");
        group_dlerror();
        cf_note(TAG, "path");
        group_path();
        cf_note(TAG, "rpath");
        group_rpath();
        cf_note(TAG, "handles");
        group_handles();
        cf_note(TAG, "local");
        group_local();
        cf_note(TAG, "strlen");
        group_strlen();
        cf_note(TAG, "dladdr");
        group_dladdr();
        cf_note(TAG, "plugin");
        group_plugin();
        cf_note(TAG, "callbacks");
        group_callbacks();
        cf_note(TAG, "version");
        group_version();
        rc = cf_summary(TAG);
    }
    return rc;
}
EOC

    cat > "$TMP/libdlplug.m" <<'EOC'
#import <Foundation/Foundation.h>
#include <unistd.h>

static char g_order[8];
static int g_order_n;

static void note(char c)
{
    if (g_order_n < 7)
        g_order[g_order_n++] = c;
}

int dlplug_data = 1234;
__thread int dlplug_tls = 7;

@interface DLPlugThing : NSObject
- (int)thingValue;
@end

@implementation DLPlugThing
+ (void)load
{
    note('L');
}
- (int)thingValue
{
    return 99;
}
@end

@implementation NSString (DLPlug)
- (NSString *)dlplugShout
{
    return [[self uppercaseString] stringByAppendingString:@"!"];
}
@end

__attribute__((constructor)) static void dlplug_ctor(void)
{
    note(getpid() > 0 ? 'C' : 'c');
}

int dlplug_add(int a, int b)
{
    return a + b;
}

int dlplug_get_data(void)
{
    return dlplug_data;
}

int *dlplug_tls_addr(void)
{
    return &dlplug_tls;
}

const char *dlplug_order(void)
{
    return g_order;
}
EOC

    cat > "$TMP/dlbundle.m" <<'EOC'
#import <Foundation/Foundation.h>

@interface DLHostBase : NSObject
- (int)hostValue;
- (NSString *)describe;
@end

@interface DLPlugin : DLHostBase
@end

@implementation DLPlugin
- (int)hostValue
{
    return [super hostValue] + 2;
}
- (NSString *)describe
{
    return [NSString stringWithFormat:@"plugin<%@>", [super describe]];
}
@end
EOC

    cat > "$TMP/dl_refusals.c" <<'EOC'
#include "cb_common.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>

#define TAG "dl_refusals"

static char g_dir[1024];

static const char *path_of(const char *leaf, char *buf, cb_size n)
{
    snprintf(buf, n, "%s/%s", g_dir, leaf);
    return buf;
}

static void text(const char *label, const char *e)
{
    char line[2400];
    cb_size i, n;

    n = (cb_size)snprintf(line, 64, "%s text %s ", TAG, label);
    if (!e)
        e = "(null)";
    for (i = 0; e[i] && n < sizeof line - 2; i++)
        line[n++] = e[i] == '\n' ? '|' : e[i];
    line[n++] = '\n';
    write(1, line, n);
}

static int refused(const char *label, const char *path, char *first, cb_size n)
{
    unsigned before = _dyld_image_count();
    void *h = dlopen(path, RTLD_NOW);
    const char *e = dlerror();
    int ok = h == 0 && e != 0 && _dyld_image_count() == before && dlerror() == 0;

    text(label, e);
    if (first)
        snprintf(first, n, "%s", e ? e : "");
    return ok;
}

int main(void)
{
    unsigned m = 0, bit = 1, before;
    char path[1200], first[2048], second[2048];
    void *h, *cf;
    char *slash;

    snprintf(g_dir, sizeof g_dir, "%s", _dyld_get_image_name(0));
    slash = strrchr(g_dir, '/');
    if (slash)
        *slash = 0;

    CK(refused("arm64", path_of("libdlarm.dylib", path, sizeof path), 0, 0));
    CK(refused("hostlib", "/usr/lib/libz.1.dylib", 0, 0));
    CK(refused("bare", "libz.dylib", 0, 0));
    CK(refused("missingdep", path_of("libdlneedsgone.dylib", path, sizeof path), first, sizeof first));
    CK(refused("missingsym", path_of("libdlneedsym.dylib", path, sizeof path), second, sizeof second));
    CK(refused("missingsym", path_of("libdlneedsym.dylib", path, sizeof path), first, sizeof first) &&
       strcmp(first, second) == 0);

    before = _dyld_image_count();
    h = dlopen(path_of("libdlother.dylib", path, sizeof path), RTLD_NOW);
    CK(h != 0 && dlsym(h, "dlother") != 0 && _dyld_image_count() == before + 1);

    h = dlopen("libc.dylib", RTLD_NOW);
    CK(h != 0 && dlsym(h, "strlen") == (void *)strlen);
    CK(dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOLOAD) == h);

    before = _dyld_image_count();
    CK(dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOLOAD) == 0);
    dlerror();
    cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    CK(cf != 0 && dlsym(cf, "CFStringGetLength") != 0 && _dyld_image_count() == before + 1);
    CK(cf != 0 && strcmp(_dyld_get_image_name(before),
                         "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation") == 0);
    CK(dlopen_preflight("/usr/lib/libSystem.B.dylib") && !dlopen_preflight("/usr/lib/libz.1.dylib"));
    text("preflight", dlerror());

    cb_begin(TAG, m);
    cb_end();
    return m != 0;
}
EOC

    printf 'int dlother(void) { return 1; }\n' > "$TMP/dlother.c"
    printf 'int dl_arm(void) { return 1; }\n' > "$TMP/dlarm.c"
    printf 'int dl_gone(void) { return 2; }\n' > "$TMP/dlgone.c"
    printf 'extern int dl_gone(void);\nint dl_needs_gone(void) { return dl_gone(); }\n' > "$TMP/dlneedsgone.c"
    printf 'int dl_vanishing(void) { return 3; }\nint dl_staying(void) { return 4; }\n' > "$TMP/dlweak1.c"
    printf 'int dl_staying(void) { return 4; }\n' > "$TMP/dlweak2.c"
    printf 'extern int dl_vanishing(void);\nint dl_needs_sym(void) { return dl_vanishing(); }\n' > "$TMP/dlneedsym.c"

    for arch in x86_64 arm64; do
        dir="$TMP/dl.$arch"
        mkdir -p "$dir"
        if {
            clang -arch $arch -mmacosx-version-min=12.0 -dynamiclib -install_name @rpath/libdlother.dylib \
                -o "$dir/libdlother.dylib" "$TMP/dlother.c" &&
            clang -arch $arch -x objective-c -fobjc-arc -O1 -mmacosx-version-min=12.0 -dynamiclib \
                -install_name @rpath/libdlplug.dylib -o "$dir/libdlplug.dylib" "$TMP/libdlplug.m" -framework Foundation &&
            clang -arch $arch -x objective-c -fobjc-arc -O1 -fno-builtin -Wno-deprecated-declarations \
                -mmacosx-version-min=12.0 -I"$TMP" -Wl,-rpath,@executable_path \
                -o "$dir/dl_basic" "$TMP/dl_basic.m" -framework Foundation &&
            clang -arch $arch -x objective-c -fobjc-arc -O1 -mmacosx-version-min=12.0 -bundle \
                -bundle_loader "$dir/dl_basic" -o "$dir/dlbundle.bundle" "$TMP/dlbundle.m" -framework Foundation
        } >"$TMP/dl_basic.$arch.cc.log" 2>&1; then
            case $arch in
                x86_64) DL_BASIC_BIN="$dir/dl_basic" ;;
                arm64) DL_BASIC_ARM64="$dir/dl_basic" ;;
            esac
        fi
    done
    cp "$TMP/dl_basic.x86_64.cc.log" "$TMP/dl_basic.cc.log" 2>/dev/null

    dir="$TMP/dl.x86_64"
    if {
        clang -arch arm64 -dynamiclib -o "$dir/libdlarm.dylib" "$TMP/dlarm.c" &&
        clang -arch x86_64 -dynamiclib -install_name @rpath/libdlgone.dylib -o "$dir/libdlgone.dylib" "$TMP/dlgone.c" &&
        clang -arch x86_64 -dynamiclib -install_name @rpath/libdlneedsgone.dylib -Wl,-rpath,@loader_path \
            -o "$dir/libdlneedsgone.dylib" "$TMP/dlneedsgone.c" "$dir/libdlgone.dylib" &&
        rm -f "$dir/libdlgone.dylib" &&
        clang -arch x86_64 -dynamiclib -install_name @rpath/libdlweak.dylib -o "$dir/libdlweak.dylib" "$TMP/dlweak1.c" &&
        clang -arch x86_64 -dynamiclib -install_name @rpath/libdlneedsym.dylib -Wl,-rpath,@loader_path \
            -o "$dir/libdlneedsym.dylib" "$TMP/dlneedsym.c" "$dir/libdlweak.dylib" &&
        clang -arch x86_64 -dynamiclib -install_name @rpath/libdlweak.dylib -o "$dir/libdlweak.dylib" "$TMP/dlweak2.c" &&
        [ -f "$dir/libdlother.dylib" ] &&
        clang -arch x86_64 -O1 -fno-builtin -I"$TMP" -o "$dir/dl_refusals" "$TMP/dl_refusals.c"
    } >"$TMP/dl_refusals.cc.log" 2>&1; then
        DL_REFUSALS_BIN="$dir/dl_refusals"
    fi
}

build_sys_fixtures() {
    local name

    mkdir -p "$SYS_WORK"
    cat > "$TMP/sys_common.h" <<'EOC'
#include "cb_common.h"

static unsigned sys_failed, sys_group;

static void sys_note(const char *tag, const char *what)
{
    char b[128];
    cb_size n = 0;

    while (*tag && n < 40)
        b[n++] = *tag++;
    b[n++] = ':';
    b[n++] = ' ';
    while (*what && n < sizeof b - 1)
        b[n++] = *what++;
    b[n++] = '\n';
    write(2, b, n);
}

static void sys_begin(const char *tag, const char *group, unsigned mask)
{
    cb_len = 0;
    cb_str(tag);
    cb_str(" ");
    cb_str(group);
    if (mask == 0) {
        cb_str(" ok");
    } else {
        cb_str(" bad:");
        cb_hex(mask);
        sys_failed |= 1u << sys_group;
    }
    sys_group++;
}

static void sys_int(const char *key, long v)
{
    cb_str(" ");
    cb_str(key);
    cb_str("=");
    if (v < 0) {
        cb_str("-");
        cb_dec((unsigned)-v);
    } else {
        cb_dec((unsigned)v);
    }
}

static int sys_summary(const char *tag)
{
    cb_begin(tag, sys_failed);
    cb_end();
    return sys_failed != 0;
}
EOC

    cat > "$TMP/sys_files.c" <<'EOC'
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <termios.h>
#include <ulimit.h>
#include <unistd.h>
#include "sys_common.h"

#define TAG "sys_files"

int open_nc(const char *path, int flags, ...) __asm__("_open$NOCANCEL");
int openat_nc(int fd, const char *path, int flags, ...) __asm__("_openat$NOCANCEL");
int fcntl_nc(int fd, int cmd, ...) __asm__("_fcntl$NOCANCEL");

static char g_path[PATH_MAX], g_at[64], g_sem[40], g_shm[40];

static int mode_is(int fd, int want)
{
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 && (int)(st.st_mode & 0777) == want;
}

static int reads(int fd, const char *want, int n)
{
    char buf[32];
    return fd >= 0 && read(fd, buf, (size_t)n) == n && memcmp(buf, want, (size_t)n) == 0;
}

static unsigned t_open(void)
{
    unsigned m = 0, bit = 1;
    struct stat st;
    int fd = open(g_path, O_CREAT | O_EXCL | O_RDWR, 0640);
    CK(fd >= 0);
    CK(mode_is(fd, 0640));
    CK(fd >= 0 && write(fd, "0123456789abcdef", 16) == 16);
    if (fd >= 0)
        close(fd);
    errno = 0;
    CK(open(g_path, O_CREAT | O_EXCL | O_RDWR, 0600) == -1 && errno == EEXIST);
    int ro = open(g_path, O_RDONLY);
    CK(reads(ro, "0123", 4));
    if (ro >= 0)
        close(ro);
    int nc = open_nc(g_path, O_RDONLY);
    CK(reads(nc, "01", 2));
    if (nc >= 0)
        close(nc);
    int tr = open(g_path, O_WRONLY | O_TRUNC);
    CK(tr >= 0 && fstat(tr, &st) == 0 && st.st_size == 0 && write(tr, "0123456789abcdef", 16) == 16);
    if (tr >= 0)
        close(tr);
    errno = 0;
    CK(open("/nonexistent-dir/sys_files", O_RDONLY) == -1 && errno == ENOENT);
    errno = 0;
    CK(open_nc("/nonexistent-dir/sys_files", O_CREAT | O_WRONLY, 0644) == -1 && errno == ENOENT);
    return m;
}

static unsigned t_openat(const char *dir)
{
    unsigned m = 0, bit = 1;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    CK(dfd >= 0);
    int afd = openat(dfd, g_at, O_CREAT | O_EXCL | O_WRONLY, 0604);
    CK(mode_is(afd, 0604));
    CK(afd >= 0 && write(afd, "wxyz", 4) == 4);
    if (afd >= 0)
        close(afd);
    int rfd = openat_nc(dfd, g_at, O_RDONLY);
    CK(reads(rfd, "wxyz", 4));
    if (rfd >= 0)
        close(rfd);
    errno = 0;
    CK(openat(dfd, g_at, O_CREAT | O_EXCL | O_WRONLY, 0600) == -1 && errno == EEXIST);
    CK(unlinkat(dfd, g_at, 0) == 0);
    if (dfd >= 0)
        close(dfd);
    return m;
}

static unsigned t_fcntl(void)
{
    unsigned m = 0, bit = 1;
    char real[PATH_MAX], got[PATH_MAX];
    int fd = open(g_path, O_RDWR);
    CK(fd >= 0);
    int fl = fcntl(fd, F_GETFL);
    CK((fl & O_ACCMODE) == O_RDWR && !(fl & O_NONBLOCK));
    CK(fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    CK(fcntl_nc(fd, F_SETFL, fl) == 0 && !(fcntl_nc(fd, F_GETFL) & O_NONBLOCK));
    CK(fcntl(fd, F_GETFD) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    int d = fcntl(fd, F_DUPFD, 50);
    CK(d >= 50 && fcntl(d, F_GETFD) == 0);
    if (d >= 0)
        close(d);
    int dc = fcntl(fd, F_DUPFD_CLOEXEC, 60);
    CK(dc >= 60 && fcntl(dc, F_GETFD) == FD_CLOEXEC);
    if (dc >= 0)
        close(dc);
    memset(got, 0, sizeof got);
    CK(fcntl(fd, F_GETPATH, got) == 0 && realpath(g_path, real) && strcmp(got, real) == 0);
    fstore_t fst = { F_ALLOCATEALL, F_PEOFPOSMODE, 0, 65536, 0 };
    CK(fcntl(fd, F_PREALLOCATE, &fst) == 0 && fst.fst_bytesalloc >= 65536);
    CK(fcntl(fd, F_NOCACHE, 1) == 0 && fcntl(fd, F_NOCACHE, 0) == 0);
    CK(fcntl(fd, F_FULLFSYNC) == 0);
    errno = 0;
    CK(fcntl(-1, F_GETFL) == -1 && errno == EBADF);
    if (fd >= 0)
        close(fd);
    return m;
}

static unsigned t_lock(void)
{
    unsigned m = 0, bit = 1;
    int fd = open(g_path, O_RDWR);
    int fd2 = open(g_path, O_RDWR);
    CK(fd >= 0 && fd2 >= 0);
    struct flock lk = { .l_start = 0, .l_len = 100, .l_pid = 0, .l_type = F_WRLCK, .l_whence = SEEK_SET };
    CK(fcntl(fd, F_SETLK, &lk) == 0);
    struct flock q = { .l_start = 0, .l_len = 100, .l_pid = 0, .l_type = F_WRLCK, .l_whence = SEEK_SET };
    CK(fcntl(fd, F_GETLK, &q) == 0 && q.l_type == F_UNLCK);
    struct flock o = { .l_start = 200, .l_len = 10, .l_pid = 0, .l_type = F_WRLCK, .l_whence = SEEK_SET };
    CK(fcntl(fd, F_OFD_SETLK, &o) == 0);
    struct flock oq = { .l_start = 200, .l_len = 10, .l_pid = 0, .l_type = F_RDLCK, .l_whence = SEEK_SET };
    CK(fcntl(fd2, F_OFD_GETLK, &oq) == 0 && oq.l_type == F_WRLCK && oq.l_start == 200 && oq.l_len == 10);
    struct flock oc = { .l_start = 205, .l_len = 1, .l_pid = 0, .l_type = F_WRLCK, .l_whence = SEEK_SET };
    errno = 0;
    CK(fcntl(fd2, F_OFD_SETLK, &oc) == -1 && (errno == EAGAIN || errno == EACCES));
    o.l_type = F_UNLCK;
    CK(fcntl(fd, F_OFD_SETLK, &o) == 0 && fcntl(fd2, F_OFD_SETLK, &oc) == 0);
    lk.l_type = F_UNLCK;
    CK(fcntl(fd, F_SETLK, &lk) == 0);
    if (fd2 >= 0)
        close(fd2);
    if (fd >= 0)
        close(fd);
    return m;
}

static unsigned t_ioctl(void)
{
    unsigned m = 0, bit = 1;
    int p[2] = { -1, -1 };
    CK(pipe(p) == 0 && write(p[1], "abcdefg", 7) == 7);
    int avail = -1;
    CK(ioctl(p[0], FIONREAD, &avail) == 0 && avail == 7);
    int on = 1;
    char buf[16];
    CK(ioctl(p[0], FIONBIO, &on) == 0 && read(p[0], buf, sizeof buf) == 7);
    errno = 0;
    CK(read(p[0], buf, sizeof buf) == -1 && errno == EAGAIN);
    CK(ioctl(p[1], FIOCLEX) == 0 && fcntl(p[1], F_GETFD) == FD_CLOEXEC);
    CK(ioctl(p[1], FIONCLEX) == 0 && fcntl(p[1], F_GETFD) == 0);
    close(p[0]);
    close(p[1]);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    CK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    const char *name = master >= 0 ? ptsname(master) : NULL;
    int slave = name ? open(name, O_RDWR | O_NOCTTY) : -1;
    CK(slave >= 0);
    struct winsize set = { 33, 101, 0, 0 }, got = { 0, 0, 0, 0 };
    CK(ioctl(master, TIOCSWINSZ, &set) == 0);
    CK(ioctl(slave, TIOCGWINSZ, &got) == 0 && got.ws_row == 33 && got.ws_col == 101);
    struct termios tio;
    CK(ioctl(slave, TIOCGETA, &tio) == 0 && tcgetattr(slave, &tio) == 0);
    errno = 0;
    CK(ioctl(-1, TIOCGWINSZ, &got) == -1 && errno == EBADF);
    if (slave >= 0)
        close(slave);
    if (master >= 0)
        close(master);
    return m;
}

static unsigned t_sem(void)
{
    unsigned m = 0, bit = 1;
    sem_unlink(g_sem);
    sem_t *s = sem_open(g_sem, O_CREAT | O_EXCL, 0600, 3);
    int ok = s != SEM_FAILED;
    CK(ok);
    CK(ok && sem_trywait(s) == 0 && sem_trywait(s) == 0 && sem_trywait(s) == 0);
    errno = 0;
    CK(ok && sem_trywait(s) == -1 && errno == EAGAIN);
    CK(ok && sem_post(s) == 0 && sem_trywait(s) == 0);
    errno = 0;
    CK(sem_open(g_sem, O_CREAT | O_EXCL, 0600, 1) == SEM_FAILED && errno == EEXIST);
    sem_t *again = sem_open(g_sem, 0);
    CK(again != SEM_FAILED);
    if (again != SEM_FAILED)
        sem_close(again);
    CK(ok && sem_close(s) == 0);
    CK(sem_unlink(g_sem) == 0);
    errno = 0;
    CK(sem_open(g_sem, 0) == SEM_FAILED && errno == ENOENT);
    return m;
}

static unsigned t_shm(void)
{
    unsigned m = 0, bit = 1;
    struct stat st;
    shm_unlink(g_shm);
    int fd = shm_open(g_shm, O_CREAT | O_EXCL | O_RDWR, 0600);
    CK(fd >= 0);
    CK(fd >= 0 && ftruncate(fd, 16384) == 0);
    CK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 16384 && (st.st_mode & 0777) == 0600);
    errno = 0;
    CK(shm_open(g_shm, O_CREAT | O_EXCL | O_RDWR, 0600) == -1 && errno == EEXIST);
    int again = shm_open(g_shm, O_RDONLY);
    CK(again >= 0);
    if (again >= 0)
        close(again);
    if (fd >= 0)
        close(fd);
    CK(shm_unlink(g_shm) == 0);
    errno = 0;
    CK(shm_open(g_shm, O_RDWR) == -1 && errno == ENOENT);
    return m;
}

static unsigned t_semctl(int *ran)
{
    unsigned m = 0, bit = 1;
    int id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
    *ran = id >= 0;
    if (id < 0)
        return 0;
    union semun arg;
    arg.val = 5;
    CK(semctl(id, 0, SETVAL, arg) == 0 && semctl(id, 0, GETVAL) == 5);
    unsigned short vals[2] = { 7, 9 }, out[2] = { 0, 0 };
    arg.array = vals;
    CK(semctl(id, 0, SETALL, arg) == 0);
    arg.array = out;
    CK(semctl(id, 0, GETALL, arg) == 0 && out[0] == 7 && out[1] == 9);
    struct semid_ds ds;
    memset(&ds, 0, sizeof ds);
    arg.buf = &ds;
    CK(semctl(id, 0, IPC_STAT, arg) == 0 && ds.sem_nsems == 2 && (ds.sem_perm.mode & 0777) == 0600);
    ds.sem_perm.mode = 0640;
    CK(semctl(id, 0, IPC_SET, arg) == 0);
    memset(&ds, 0, sizeof ds);
    CK(semctl(id, 0, IPC_STAT, arg) == 0 && (ds.sem_perm.mode & 0777) == 0640);
    CK(semctl(id, 1, GETVAL) == 9 && semctl(id, 0, GETNCNT) == 0);
    CK(semctl(id, 0, IPC_RMID) == 0);
    CK(semctl(id, 0, GETVAL) == -1);
    return m;
}

static unsigned t_ulimit(void)
{
    unsigned m = 0, bit = 1;
    long cur = ulimit(UL_GETFSIZE);
    CK(cur > 0);
    struct rlimit rl;
    CK(getrlimit(RLIMIT_FSIZE, &rl) == 0 && (rl.rlim_cur == RLIM_INFINITY || cur == (long)(rl.rlim_cur / 512)));
    long lower = 1L << 30;
    CK(ulimit(UL_SETFSIZE, lower) == lower && ulimit(UL_GETFSIZE) == lower);
    errno = 0;
    CK(ulimit(1234) == -1 && errno == EINVAL);
    return m;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    int pid = (int)getpid(), sem_ran = 0;
    snprintf(g_path, sizeof g_path, "%s/sys_files.%d", dir, pid);
    snprintf(g_at, sizeof g_at, "sys_files_at.%d", pid);
    snprintf(g_sem, sizeof g_sem, "/sysf.%d", pid);
    snprintf(g_shm, sizeof g_shm, "/sysf-shm.%d", pid);
    unlink(g_path);
    umask(022);

    sys_note(TAG, "open");
    sys_begin(TAG, "open", t_open());
    cb_end();
    sys_note(TAG, "openat");
    sys_begin(TAG, "openat", t_openat(dir));
    cb_end();
    sys_note(TAG, "fcntl");
    sys_begin(TAG, "fcntl", t_fcntl());
    cb_end();
    sys_note(TAG, "lock");
    sys_begin(TAG, "lock", t_lock());
    cb_end();
    sys_note(TAG, "ioctl");
    sys_begin(TAG, "ioctl", t_ioctl());
    cb_end();
    sys_note(TAG, "sem_open");
    sys_begin(TAG, "sem_open", t_sem());
    cb_end();
    sys_note(TAG, "shm_open");
    sys_begin(TAG, "shm_open", t_shm());
    cb_end();
    sys_note(TAG, "semctl");
    unsigned sm = t_semctl(&sem_ran);
    sys_begin(TAG, "semctl", sm);
    sys_int("ran", sem_ran);
    cb_end();
    sys_note(TAG, "ulimit");
    sys_begin(TAG, "ulimit", t_ulimit());
    cb_end();
    unlink(g_path);
    return sys_summary(TAG);
}
EOC

    cat > "$TMP/sys_mmap.c" <<'EOC'
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#if defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#endif
#include "sys_common.h"

#define TAG "sys_mmap"
#define PAGE 16384

static char g_path[1024];
static sigjmp_buf g_fault_env;
static volatile sig_atomic_t g_faults;
static unsigned char g_file[3 * PAGE];

static int all_pattern(const unsigned char *p, int n)
{
    unsigned bad = 0;
    for (int i = 0; i < n; i++)
        bad += p[i] != (unsigned char)(i * 7);
    return bad == 0;
}

static unsigned t_anon(void)
{
    unsigned m = 0, bit = 1;
    unsigned char *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    int ok = p != MAP_FAILED;
    CK(ok && ((uintptr_t)p & (PAGE - 1)) == 0);
    unsigned zero = 0;
    for (int i = 0; ok && i < 4 * PAGE; i++)
        zero |= p[i];
    CK(ok && zero == 0);
    for (int i = 0; ok && i < 4 * PAGE; i++)
        p[i] = (unsigned char)(i * 7);
    CK(ok && all_pattern(p, 4 * PAGE));
    unsigned char *q = ok ? mmap(p + PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED,
                                 -1, 0) : MAP_FAILED;
    CK(ok && q == p + PAGE && q[0] == 0 && q[PAGE - 1] == 0 && p[1] == 7 &&
       p[2 * PAGE] == (unsigned char)(2 * PAGE * 7));
    CK(ok && madvise(p, 4 * PAGE, MADV_WILLNEED) == 0 && madvise(p, PAGE, MADV_DONTNEED) == 0);
    CK(ok && munmap(p, 4 * PAGE) == 0);
    return m;
}

static unsigned t_file(void)
{
    unsigned m = 0, bit = 1;
    int fd = open(g_path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    for (int i = 0; i < 3 * PAGE; i++)
        g_file[i] = (unsigned char)(i * 13 + 5);
    CK(fd >= 0 && write(fd, g_file, sizeof g_file) == (ssize_t)sizeof g_file);
    unsigned char *r = mmap(NULL, 2 * PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
    CK(r != MAP_FAILED && memcmp(r, g_file + PAGE, 2 * PAGE) == 0);
    CK(r != MAP_FAILED && munmap(r, 2 * PAGE) == 0);
    unsigned char *w = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    unsigned char b0 = 0;
    if (w != MAP_FAILED)
        w[0] = 0xaa;
    CK(w != MAP_FAILED && pread(fd, &b0, 1, 0) == 1 && b0 == g_file[0] && w[0] == 0xaa && w[1] == g_file[1]);
    CK(w != MAP_FAILED && munmap(w, PAGE) == 0);
    unsigned char *s = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PAGE);
    if (s != MAP_FAILED)
        memcpy(s + 100, "shared-write", 12);
    CK(s != MAP_FAILED && msync(s, PAGE, MS_SYNC) == 0);
    char back[12] = { 0 };
    CK(s != MAP_FAILED && pread(fd, back, 12, PAGE + 100) == 12 && memcmp(back, "shared-write", 12) == 0);
    CK(s != MAP_FAILED && mlock(s, PAGE) == 0 && munlock(s, PAGE) == 0);
    CK(s != MAP_FAILED && munmap(s, PAGE) == 0);
    if (fd >= 0)
        close(fd);
    unlink(g_path);
    return m;
}

static unsigned code_put(unsigned char *p, unsigned value)
{
#if defined(__x86_64__)
    p[0] = 0xb8;
    memcpy(p + 1, &value, 4);
    p[5] = 0xc3;
#else
    unsigned lo = (0x52800000u | ((value & 0xffffu) << 5));
    unsigned hi = (0x72a00000u | ((value >> 16) << 5));
    unsigned ret = 0xd65f03c0u;
    memcpy(p, &lo, 4);
    memcpy(p + 4, &hi, 4);
    memcpy(p + 8, &ret, 4);
#endif
    return value;
}

static unsigned code_run(unsigned char *p)
{
#if defined(__aarch64__)
    sys_icache_invalidate(p, PAGE);
#endif
    return ((unsigned (*)(void))(void *)p)();
}

static int code_cycle(unsigned char *p, unsigned value)
{
    unsigned want;
    if (mprotect(p, PAGE, PROT_READ | PROT_WRITE) != 0)
        return 0;
    want = code_put(p, value);
    if (mprotect(p, PAGE, PROT_READ | PROT_EXEC) != 0)
        return 0;
    return code_run(p) == want;
}

static unsigned t_code(void)
{
    unsigned m = 0, bit = 1;
    unsigned char *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    int ok = p != MAP_FAILED;
    CK(ok);
    CK(ok && code_cycle(p, 0x12345678u));
    CK(ok && code_cycle(p, 0x0badcafeu));
    int all = ok;
    for (unsigned i = 0; ok && i < 5; i++)
        all &= code_cycle(p, 1000u + i);
    CK(all);
    CK(ok && munmap(p, PAGE) == 0);
    unsigned char *q = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CK(q != MAP_FAILED && code_cycle(q, 0x600dd00du) && munmap(q, PAGE) == 0);
    return m;
}

static void on_fault(int sig)
{
    (void)sig;
    g_faults++;
    siglongjmp(g_fault_env, 1);
}

static unsigned t_fault(void)
{
    unsigned m = 0, bit = 1;
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    CK(sigaction(SIGSEGV, &sa, &old_segv) == 0 && sigaction(SIGBUS, &sa, &old_bus) == 0);
    volatile unsigned char *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    int ok = p != MAP_FAILED;
    if (ok)
        p[10] = 42;
    CK(ok && mprotect((void *)p, PAGE, PROT_NONE) == 0);
    volatile int reached = 0;
    volatile unsigned char got = 0;
    if (ok && sigsetjmp(g_fault_env, 1) == 0) {
        got = p[10];
        reached = 1;
    }
    CK(ok && reached == 0 && g_faults == 1);
    CK(ok && mprotect((void *)p, PAGE, PROT_READ) == 0 && p[10] == 42);
    if (ok && sigsetjmp(g_fault_env, 1) == 0) {
        p[11] = 1;
        reached = 1;
    }
    CK(ok && reached == 0 && g_faults == 2);
    CK(ok && mprotect((void *)p, PAGE, PROT_READ | PROT_WRITE) == 0);
    if (ok)
        p[11] = 7;
    CK(ok && p[11] == 7 && got == 0);
    CK(ok && munmap((void *)p, PAGE) == 0);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    return m;
}

static unsigned t_mach(void)
{
    unsigned m = 0, bit = 1;
    mach_vm_address_t a = 0;
    CK(mach_vm_allocate(mach_task_self(), &a, 3 * PAGE, VM_FLAGS_ANYWHERE) == KERN_SUCCESS && a != 0);
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)a;
    CK(a != 0 && p[0] == 0 && p[3 * PAGE - 1] == 0);
    if (a)
        p[5] = 9;
    CK(a != 0 && mach_vm_protect(mach_task_self(), a, PAGE, FALSE, VM_PROT_READ) == KERN_SUCCESS && p[5] == 9);
    CK(a != 0 && mach_vm_protect(mach_task_self(), a, PAGE, FALSE, VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS);
    if (a)
        p[6] = 8;
    CK(a != 0 && mach_vm_deallocate(mach_task_self(), a, 3 * PAGE) == KERN_SUCCESS);
    vm_address_t v = 0;
    CK(vm_allocate(mach_task_self(), &v, PAGE, VM_FLAGS_ANYWHERE) == KERN_SUCCESS && v != 0);
    if (v)
        ((volatile char *)v)[1] = 3;
    CK(v != 0 && vm_protect(mach_task_self(), v, PAGE, FALSE, VM_PROT_READ) == KERN_SUCCESS);
    CK(v != 0 && vm_deallocate(mach_task_self(), v, PAGE) == KERN_SUCCESS);
    return m;
}

static unsigned t_host(void)
{
    unsigned m = 0, bit = 1;
    void *h = NULL;
    CK(posix_memalign(&h, PAGE, PAGE) == 0 && h != NULL);
    if (h)
        memset(h, 0x11, PAGE);
    CK(h != NULL && mprotect(h, PAGE, PROT_READ) == 0 && ((volatile unsigned char *)h)[100] == 0x11);
    CK(h != NULL && mprotect(h, PAGE, PROT_READ | PROT_WRITE) == 0);
    if (h)
        ((volatile unsigned char *)h)[100] = 0x22;
    CK(h != NULL && madvise(h, PAGE, MADV_WILLNEED) == 0);
    free(h);
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    CK(task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS && count >= 1 && threads != NULL);
    for (mach_msg_type_number_t i = 0; threads && i < count; i++)
        mach_port_deallocate(mach_task_self(), threads[i]);
    CK(threads != NULL &&
       vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof threads[0]) == KERN_SUCCESS);
    return m;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    snprintf(g_path, sizeof g_path, "%s/sys_mmap.%d", dir, (int)getpid());
    sys_note(TAG, "anon");
    sys_begin(TAG, "anon", t_anon());
    cb_end();
    sys_note(TAG, "file");
    sys_begin(TAG, "file", t_file());
    cb_end();
    sys_note(TAG, "code");
    sys_begin(TAG, "code", t_code());
    cb_end();
    sys_note(TAG, "fault");
    sys_begin(TAG, "fault", t_fault());
    cb_end();
    sys_note(TAG, "mach");
    sys_begin(TAG, "mach", t_mach());
    cb_end();
    sys_note(TAG, "host");
    sys_begin(TAG, "host", t_host());
    cb_end();
    return sys_summary(TAG);
}
EOC

    cat > "$TMP/sys_jmp.c" <<'EOC'
#include <fenv.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sys_common.h"

#define TAG "sys_jmp"
#define ALT_BYTES 65536

static jmp_buf g_jb;
static sigjmp_buf g_sjb;
static volatile sig_atomic_t g_hits, g_on_alt;
static char *g_alt;
static volatile unsigned g_depth_reached;

__attribute__((noinline)) static void dive(int n, int value)
{
    volatile char pad[64];
    pad[n & 63] = (char)n;
    g_depth_reached = (unsigned)n;
    if (n == 0)
        longjmp(g_jb, value);
    dive(n - 1, value + pad[n & 63] * 0);
}

static unsigned t_value(void)
{
    unsigned m = 0, bit = 1;
    volatile int round = 0, first = -1, second = -1, third = -1;
    volatile unsigned long long keep = 0x1122334455667788ull;
    int r = setjmp(g_jb);
    round++;
    if (round == 1) {
        first = r;
        dive(40, 42);
    }
    if (round == 2) {
        second = r;
        longjmp(g_jb, 0);
    }
    if (round == 3) {
        third = r;
        longjmp(g_jb, -7);
    }
    CK(first == 0);
    CK(second == 42 && g_depth_reached == 0);
    CK(third == 1);
    CK(round == 4 && r == -7 && keep == 0x1122334455667788ull);
    jmp_buf copy;
    volatile int copied = 0;
    if (setjmp(g_jb) == 0) {
        memcpy(copy, g_jb, sizeof copy);
        copied = 1;
        longjmp(copy, 3);
    }
    CK(copied == 1);
    return m;
}

static int blocked(int sig)
{
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    return sigismember(&cur, sig);
}

static void block(int sig, int how)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    sigprocmask(how, &s, NULL);
}

static unsigned t_mask(void)
{
    unsigned m = 0, bit = 1;
    volatile int step = 0;
    block(SIGUSR2, SIG_UNBLOCK);
    if (setjmp(g_jb) == 0) {
        block(SIGUSR2, SIG_BLOCK);
        step = 1;
        longjmp(g_jb, 1);
    }
    CK(step == 1 && !blocked(SIGUSR2));
    if (_setjmp(g_jb) == 0) {
        block(SIGUSR2, SIG_BLOCK);
        step = 2;
        _longjmp(g_jb, 1);
    }
    CK(step == 2 && blocked(SIGUSR2));
    block(SIGUSR2, SIG_UNBLOCK);
    if (sigsetjmp(g_sjb, 1) == 0) {
        block(SIGUSR2, SIG_BLOCK);
        step = 3;
        siglongjmp(g_sjb, 1);
    }
    CK(step == 3 && !blocked(SIGUSR2));
    if (sigsetjmp(g_sjb, 0) == 0) {
        block(SIGUSR2, SIG_BLOCK);
        step = 4;
        siglongjmp(g_sjb, 1);
    }
    CK(step == 4 && blocked(SIGUSR2));
    block(SIGUSR2, SIG_UNBLOCK);
    if (sigsetjmp(g_sjb, 1) == 0) {
        block(SIGUSR2, SIG_BLOCK);
        step = 5;
        longjmp(g_sjb, 1);
    }
    CK(step == 5 && !blocked(SIGUSR2));
    return m;
}

static void on_usr1(int sig)
{
    g_hits++;
    siglongjmp(g_sjb, sig);
}

static void on_usr1_bsd(int sig)
{
    g_hits++;
    longjmp(g_jb, sig + 100);
}

static unsigned t_handler(void)
{
    unsigned m = 0, bit = 1;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    CK(sigaction(SIGUSR1, &sa, &old) == 0);
    g_hits = 0;
    volatile int rounds = 0, wrong = 0;
    int r = sigsetjmp(g_sjb, 1);
    if (r != 0 && (r != SIGUSR1 || blocked(SIGUSR1)))
        wrong++;
    if (rounds++ < 3)
        raise(SIGUSR1);
    CK(rounds == 4 && g_hits == 3 && wrong == 0);
    sa.sa_handler = on_usr1_bsd;
    CK(sigaction(SIGUSR1, &sa, NULL) == 0);
    rounds = 0;
    r = setjmp(g_jb);
    if (r != 0 && (r != SIGUSR1 + 100 || blocked(SIGUSR1)))
        wrong++;
    if (rounds++ < 2)
        kill(getpid(), SIGUSR1);
    CK(rounds == 3 && g_hits == 5 && wrong == 0);
    sa.sa_handler = on_usr1;
    CK(sigaction(SIGUSR1, &sa, NULL) == 0);
    if (sigsetjmp(g_sjb, 0) == 0)
        raise(SIGUSR1);
    CK(g_hits == 6 && blocked(SIGUSR1));
    block(SIGUSR1, SIG_UNBLOCK);
    sigaction(SIGUSR1, &old, NULL);
    return m;
}

static void on_alt(int sig)
{
    char here;
    g_hits++;
    g_on_alt += (uintptr_t)&here - (uintptr_t)g_alt < ALT_BYTES;
    siglongjmp(g_sjb, sig);
}

static unsigned t_altstack(void)
{
    unsigned m = 0, bit = 1;
    g_alt = malloc(ALT_BYTES);
    stack_t ss = { .ss_sp = g_alt, .ss_size = ALT_BYTES, .ss_flags = 0 }, got;
    CK(g_alt != NULL && sigaltstack(&ss, NULL) == 0);
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alt;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    CK(sigaction(SIGUSR2, &sa, &old) == 0);
    g_hits = 0;
    g_on_alt = 0;
    volatile int rounds = 0, still_on = 0;
    if (sigsetjmp(g_sjb, 1) != 0 && (sigaltstack(NULL, &got) != 0 || (got.ss_flags & SS_ONSTACK)))
        still_on++;
    if (rounds++ < 3)
        raise(SIGUSR2);
    CK(rounds == 4 && g_hits == 3);
    CK(g_on_alt == 3);
    CK(still_on == 0);
    sigaction(SIGUSR2, &old, NULL);
    ss.ss_flags = SS_DISABLE;
    CK(sigaltstack(&ss, NULL) == 0);
    free(g_alt);
    return m;
}

static int g_cmp_calls, g_cmp_bad;

static int cmp_jump(const void *a, const void *b)
{
    jmp_buf inner;
    volatile int once = 0;
    g_cmp_calls++;
    if (setjmp(inner) == 0) {
        once = 1;
        longjmp(inner, 1);
    }
    if (once != 1)
        g_cmp_bad++;
    return *(const int *)a - *(const int *)b;
}

static unsigned t_callback(void)
{
    unsigned m = 0, bit = 1;
    int v[16];
    for (int i = 0; i < 16; i++)
        v[i] = (i * 7) % 16;
    g_cmp_calls = 0;
    qsort(v, 16, sizeof v[0], cmp_jump);
    int sorted = 1;
    for (int i = 0; i < 16; i++)
        sorted &= v[i] == i;
    CK(sorted && g_cmp_calls > 0 && g_cmp_bad == 0);
    return m;
}

#if defined(__x86_64__)
static unsigned t_x86(void)
{
    unsigned m = 0, bit = 1;
    unsigned short cw_before = 0, cw_after = 0;
    unsigned mx_after = 0;
    fesetround(FE_TONEAREST);
    __asm__ volatile("fnstcw %0" : "=m"(cw_before));
    if (setjmp(g_jb) == 0) {
        unsigned short cw = (unsigned short)((cw_before & ~0x0c00u) | 0x0400u);
        fesetround(FE_UPWARD);
        __asm__ volatile("fldcw %0" : : "m"(cw));
        longjmp(g_jb, 1);
    }
    __asm__ volatile("fnstcw %0" : "=m"(cw_after));
    __asm__ volatile("stmxcsr %0" : "=m"(mx_after));
    CK(fegetround() == FE_TONEAREST && ((mx_after >> 13) & 3) == 0);
    CK(cw_after == cw_before);
    volatile double x = 1.0, third = 3.0;
    CK(x / third < 0.33333333333333338 && x / third > 0.33333333333333326);
    if (setjmp(g_jb) == 0) {
        __asm__ volatile("std");
        longjmp(g_jb, 1);
    }
    unsigned long long fl;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    CK(((fl >> 10) & 1) == 0);
    return m;
}
#endif

int main(void)
{
    sys_note(TAG, "value");
    sys_begin(TAG, "value", t_value());
    cb_end();
    sys_note(TAG, "mask");
    sys_begin(TAG, "mask", t_mask());
    cb_end();
    sys_note(TAG, "handler");
    sys_begin(TAG, "handler", t_handler());
    cb_end();
    sys_note(TAG, "altstack");
    sys_begin(TAG, "altstack", t_altstack());
    cb_end();
    sys_note(TAG, "callback");
    sys_begin(TAG, "callback", t_callback());
    cb_end();
#if defined(__x86_64__)
    sys_note(TAG, "x86");
    sys_begin(TAG, "x86", t_x86());
    cb_end();
#endif
    return sys_summary(TAG);
}
EOC

    cat > "$TMP/sys_proc.c" <<'EOC'
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sys_common.h"

#define TAG "sys_proc"

extern char **environ;

static char g_self[PATH_MAX], g_dir[PATH_MAX], g_base[256], g_quoted[PATH_MAX + 2];

static const char *base_of(const char *p)
{
    const char *b = p ? strrchr(p, '/') : NULL;
    b = b ? b + 1 : (p ? p : "(null)");
    return strncmp(b, "sys_proc", 8) == 0 && (b[8] == '\0' || strcmp(b + 8, ".arm64") == 0) ? "sys_proc" : b;
}

static void line(const char *a, const char *b, const char *c)
{
    cb_len = 0;
    cb_str(a);
    if (b)
        cb_str(b);
    if (c)
        cb_str(c);
    cb_end();
}

static int child_report(int argc, char **argv)
{
    const char *kind = argc > 2 ? argv[2] : "none";
    const char *mark = getenv("SYS_MARK");
    char exe[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getpid(), exe, sizeof exe) <= 0)
        strcpy(exe, "?");
    const char *log = getenv("SYS_MODE_LOG");
    if (log) {
        const char *mode = getenv("OCERZ_MODE");
        int fd = open(log, O_WRONLY | O_APPEND | O_CREAT, 0644);
        char b[512];
        int n = snprintf(b, sizeof b, "%s mode=%s exe=%s\n", base_of(kind), mode ? mode : "none",
                         base_of(exe));
        if (fd >= 0) {
            write(fd, b, (size_t)n);
            close(fd);
        }
    }
    cb_len = 0;
    cb_str("report ");
    cb_str(base_of(kind));
    cb_str(" a0=");
    cb_str(base_of(argv[0]));
    sys_int("argc", argc);
    cb_str(" mark=");
    cb_str(mark ? mark : "none");
    cb_str(" path=");
    cb_str(getenv("PATH") ? "set" : "none");
    for (int i = 3; i < argc && i < 12; i++) {
        cb_str(" ");
        cb_str(base_of(argv[i]));
    }
    cb_end();
    return 20 + argc;
}

static int child_sink(void)
{
    char b[256];
    long total = 0;
    ssize_t n;
    while ((n = read(0, b, sizeof b)) > 0)
        total += n;
    return (int)(total % 200);
}

pid_t sys_vfork(void) __asm__("_vfork");

static int status_code(int st)
{
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static int wait_code(pid_t pid)
{
    int st = 0;
    pid_t w;
    do {
        w = waitpid(pid, &st, 0);
    } while (w == -1 && errno == EINTR);
    return w == pid ? status_code(st) : -1;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void *thread_body(void *arg)
{
    return (void *)((long)arg * 3);
}

static unsigned t_fork(void)
{
    unsigned m = 0, bit = 1;
    pid_t parent = getpid();
    int p[2];
    CK(pipe(p) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        unsigned cm = 0, cbit = 1;
        close(p[0]);
#define CCK(c) do { if (!(c)) cm |= cbit; cbit <<= 1; } while (0)
        CCK(getpid() != parent && getppid() == parent);
        int v[32];
        for (int i = 0; i < 32; i++)
            v[i] = (i * 11) % 32;
        qsort(v, 32, sizeof v[0], cmp_int);
        int sorted = 1;
        for (int i = 0; i < 32; i++)
            sorted &= v[i] == i;
        CCK(sorted);
        pthread_t th;
        void *ret = NULL;
        CCK(pthread_create(&th, NULL, thread_body, (void *)14) == 0 && pthread_join(th, &ret) == 0 &&
            (long)ret == 42);
        char *heap = malloc(64);
        CCK(heap != NULL);
        if (heap)
            strcpy(heap, "child-heap");
        CCK(write(p[1], "child-ok", 8) == 8);
        free(heap);
        _exit((int)cm);
    }
    CK(pid > 0);
    close(p[1]);
    char buf[16] = { 0 };
    CK(read(p[0], buf, sizeof buf - 1) == 8 && strcmp(buf, "child-ok") == 0);
    close(p[0]);
    CK(wait_code(pid) == 0);
    pid_t again = fork();
    if (again == 0)
        _exit(17);
    int st = 0;
    struct rusage ru;
    CK(again > 0 && wait4(again, &st, 0, &ru) == again && status_code(st) == 17);
    pid_t vp = sys_vfork();
    if (vp == 0)
        _exit(7);
    CK(vp > 0 && wait_code(vp) == 7);
    errno = 0;
    CK(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD);
    return m;
}

static unsigned t_spawn(void)
{
    unsigned m = 0, bit = 1;
    int p[2];
    CK(pipe(p) == 0);
    posix_spawn_file_actions_t fa;
    CK(posix_spawn_file_actions_init(&fa) == 0);
    CK(posix_spawn_file_actions_adddup2(&fa, p[1], 1) == 0);
    CK(posix_spawn_file_actions_addclose(&fa, p[0]) == 0 && posix_spawn_file_actions_addclose(&fa, p[1]) == 0);
    char *argv[] = { (char *)"-sys-child", (char *)"child-report", (char *)"spawn", (char *)"x", NULL };
    char logkv[PATH_MAX + 16];
    char *envp[] = { (char *)"SYS_MARK=spawn", NULL, NULL };
    if (getenv("SYS_MODE_LOG")) {
        snprintf(logkv, sizeof logkv, "SYS_MODE_LOG=%s", getenv("SYS_MODE_LOG"));
        envp[1] = logkv;
    }
    pid_t pid = 0;
    CK(posix_spawn(&pid, g_self, &fa, NULL, argv, envp) == 0 && pid > 0);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    char buf[256] = { 0 };
    ssize_t n = read(p[0], buf, sizeof buf - 1);
    close(p[0]);
    CK(n > 0);
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = 0;
    line("spawn got '", buf, "'");
    CK(pid > 0 && wait_code(pid) == 24);

    posix_spawnattr_t at;
    CK(posix_spawnattr_init(&at) == 0);
    CK(posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP) == 0 && posix_spawnattr_setpgroup(&at, 0) == 0);
    char *argv2[] = { (char *)"attr-child", (char *)"child-report", (char *)"spawn-attr", NULL };
    pid = 0;
    CK(posix_spawn(&pid, g_self, NULL, &at, argv2, environ) == 0 && pid > 0);
    CK(pid > 0 && wait_code(pid) == 23);
    posix_spawnattr_destroy(&at);

    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s:/usr/bin:/bin", g_dir);
    char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
    setenv("PATH", path, 1);
    char *argv3[] = { g_base, (char *)"child-report", (char *)"spawnp", NULL };
    pid = 0;
    CK(posix_spawnp(&pid, g_base, NULL, NULL, argv3, environ) == 0 && pid > 0);
    CK(pid > 0 && wait_code(pid) == 23);
    CK(posix_spawnp(&pid, "sys-proc-no-such-program", NULL, NULL, argv3, environ) == ENOENT);
    if (old_path) {
        setenv("PATH", old_path, 1);
        free(old_path);
    }
    CK(posix_spawn(&pid, "/nonexistent/sys-proc", NULL, NULL, argv3, environ) == ENOENT);

    char script[PATH_MAX + 32];
    snprintf(script, sizeof script, "%s/sys_proc_script", g_dir);
    FILE *f = fopen(script, "w");
    if (f) {
        fprintf(f, "#!%s child-report\n", g_self);
        fclose(f);
        chmod(script, 0755);
    }
    char *argv4[] = { (char *)"script-argv0", (char *)"extra", NULL };
    pid = 0;
    CK(f != NULL && posix_spawn(&pid, script, NULL, NULL, argv4, environ) == 0 && pid > 0);
    CK(pid > 0 && wait_code(pid) == 24);
    unlink(script);
    return m;
}

static int fork_exec(int how)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)"-exec-child", (char *)"child-report", NULL, NULL };
        char *envp[] = { (char *)"SYS_MARK=exec", NULL, NULL };
        char logkv[PATH_MAX + 16];
        if (getenv("SYS_MODE_LOG")) {
            snprintf(logkv, sizeof logkv, "SYS_MODE_LOG=%s", getenv("SYS_MODE_LOG"));
            envp[1] = logkv;
        }
        switch (how) {
        case 0:
            argv[2] = (char *)"execv";
            execv(g_self, argv);
            break;
        case 1:
            argv[2] = (char *)"execve";
            execve(g_self, argv, envp);
            break;
        case 2:
            argv[2] = (char *)"execve-null";
            execve(g_self, argv, NULL);
            break;
        case 3:
            argv[0] = g_base;
            argv[2] = (char *)"execvp";
            execvp(g_base, argv);
            break;
        case 4:
            argv[2] = (char *)"execvP";
            execvP(g_base, g_dir, argv);
            break;
        case 5:
            execl(g_self, "-l", "child-report", "execl", (char *)0);
            break;
        case 6:
            execle(g_self, "-le", "child-report", "execle", (char *)0, envp);
            break;
        case 7:
            execlp(g_base, "-lp", "child-report", "execlp", (char *)0);
            break;
        case 8:
            execl(g_self, "-many", "child-report", "many", "a", "b", "c", "d", "e", "f", (char *)0);
            break;
        }
        _exit(99);
    }
    return pid > 0 ? wait_code(pid) : -1;
}

static unsigned t_exec(void)
{
    unsigned m = 0, bit = 1;
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s:/usr/bin:/bin", g_dir);
    char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
    setenv("PATH", path, 1);
    CK(fork_exec(0) == 23);
    CK(fork_exec(1) == 23);
    CK(fork_exec(2) == 23);
    CK(fork_exec(3) == 23);
    CK(fork_exec(4) == 23);
    CK(fork_exec(5) == 23);
    CK(fork_exec(6) == 23);
    CK(fork_exec(7) == 23);
    CK(fork_exec(8) == 29);
    if (old_path) {
        setenv("PATH", old_path, 1);
        free(old_path);
    }
    errno = 0;
    char *argv[] = { (char *)"x", NULL };
    CK(execv("/nonexistent/sys-proc", argv) == -1 && errno == ENOENT);
    errno = 0;
    CK(execvp("sys-proc-no-such-program", argv) == -1 && errno == ENOENT);
    return m;
}

static unsigned t_shell(void)
{
    unsigned m = 0, bit = 1;
    CK(system(NULL) != 0);
    int st = system("exit 3");
    CK(WIFEXITED(st) && WEXITSTATUS(st) == 3);
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "%s child-report system y z", g_quoted);
    st = system(cmd);
    CK(WIFEXITED(st) && WEXITSTATUS(st) == 25);
    st = system("x=sh; for i in 1 2; do echo \"line-$x-$i\"; done; (exit 4)");
    CK(WIFEXITED(st) && WEXITSTATUS(st) == 4);
    snprintf(cmd, sizeof cmd, "%s child-report popen", g_quoted);
    FILE *f = popen(cmd, "r");
    char buf[256] = { 0 };
    CK(f != NULL && fgets(buf, sizeof buf, f) != NULL);
    buf[strcspn(buf, "\n")] = 0;
    line("popen got '", buf, "'");
    st = f ? pclose(f) : -1;
    CK(WIFEXITED(st) && WEXITSTATUS(st) == 23);
    snprintf(cmd, sizeof cmd, "%s child-sink", g_quoted);
    f = popen(cmd, "w");
    CK(f != NULL && fputs("forty-two bytes of text for the sink ....\n", f) >= 0);
    st = f ? pclose(f) : -1;
    CK(WIFEXITED(st) && WEXITSTATUS(st) == 42);
    f = popen("read a; echo \"back:$a\"", "r+");
    memset(buf, 0, sizeof buf);
    CK(f != NULL && fputs("ping\n", f) >= 0 && fflush(f) == 0);
    CK(f != NULL && fgets(buf, sizeof buf, f) != NULL && strcmp(buf, "back:ping\n") == 0);
    CK(f != NULL && pclose(f) == 0);
    errno = 0;
    CK(popen("true", "x") == NULL && errno == EINVAL);
    CK(pclose(stdin) == -1);
    return m;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child-report") == 0)
        return child_report(argc, argv);
    if (argc > 1 && strcmp(argv[1], "child-sink") == 0)
        return child_sink();
    if (!realpath(argv[0], g_self))
        return 2;
    snprintf(g_dir, sizeof g_dir, "%s", g_self);
    *strrchr(g_dir, '/') = 0;
    snprintf(g_base, sizeof g_base, "%s", base_of(g_self));
    snprintf(g_quoted, sizeof g_quoted, "'%s'", g_self);

    sys_note(TAG, "fork");
    sys_begin(TAG, "fork", t_fork());
    cb_end();
    sys_note(TAG, "spawn");
    sys_begin(TAG, "spawn", t_spawn());
    cb_end();
    sys_note(TAG, "exec");
    sys_begin(TAG, "exec", t_exec());
    cb_end();
    sys_note(TAG, "shell");
    sys_begin(TAG, "shell", t_shell());
    cb_end();
    return sys_summary(TAG);
}
EOC

    cat > "$TMP/sys_jmp_refused.c" <<'EOC'
#include <setjmp.h>
#include <stdlib.h>
#include "sys_common.h"

#define TAG "sys_jmp_refused"

static jmp_buf g_outer;
static int g_calls;

static int cmp_leave(const void *a, const void *b)
{
    if (++g_calls == 3)
        longjmp(g_outer, 5);
    return *(const int *)a - *(const int *)b;
}

int main(void)
{
    int v[8] = { 5, 3, 7, 1, 8, 2, 6, 4 };
    cb_len = 0;
    cb_str(TAG " start");
    cb_end();
    int r = setjmp(g_outer);
    if (r != 0) {
        cb_len = 0;
        cb_str(TAG " jumped");
        sys_int("r", r);
        sys_int("calls", g_calls);
        cb_end();
        return 0;
    }
    qsort(v, 8, sizeof v[0], cmp_leave);
    cb_len = 0;
    cb_str(TAG " returned");
    cb_end();
    return 1;
}
EOC

    cat > "$TMP/sys_fork_callback.c" <<'EOC'
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sys_common.h"

#define TAG "sys_fork_callback"

static int g_calls, g_status = -1;

static int cmp_fork(const void *a, const void *b)
{
    if (++g_calls == 3) {
        pid_t pid = fork();
        if (pid == 0) {
            cb_len = 0;
            cb_str(TAG " child");
            cb_end();
            _exit(8);
        }
        int st = 0;
        if (pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st))
            g_status = WEXITSTATUS(st);
    }
    return *(const int *)a - *(const int *)b;
}

int main(void)
{
    int v[8] = { 5, 3, 7, 1, 8, 2, 6, 4 };
    cb_len = 0;
    cb_str(TAG " start");
    cb_end();
    qsort(v, 8, sizeof v[0], cmp_fork);
    int sorted = 1;
    for (int i = 0; i < 8; i++)
        sorted &= v[i] == i + 1;
    cb_len = 0;
    cb_str(TAG " parent");
    sys_int("saw", g_status);
    sys_int("sorted", sorted);
    cb_end();
    return 0;
}
EOC

    for name in sys_files sys_mmap sys_jmp sys_proc sys_jmp_refused sys_fork_callback; do
        clang -arch x86_64 -std=c11 -O1 -fno-builtin \
                -o "$TMP/$name" "$TMP/$name.c" >"$TMP/$name.cc.log" 2>&1 || continue
        case $name in
            sys_files) SYS_FILES_BIN="$TMP/$name" ;;
            sys_mmap) SYS_MMAP_BIN="$TMP/$name" ;;
            sys_jmp) SYS_JMP_BIN="$TMP/$name" ;;
            sys_proc) SYS_PROC_BIN="$TMP/$name" ;;
            sys_jmp_refused) SYS_REFUSE_BIN="$TMP/$name"; continue ;;
            sys_fork_callback) SYS_FORKCB_BIN="$TMP/$name" ;;
        esac
        clang -arch arm64 -std=c11 -O1 -fno-builtin \
                -o "$TMP/$name.arm64" "$TMP/$name.c" >"$TMP/$name.arm64.cc.log" 2>&1 || continue
        case $name in
            sys_files) SYS_FILES_ARM64="$TMP/$name.arm64" ;;
            sys_mmap) SYS_MMAP_ARM64="$TMP/$name.arm64" ;;
            sys_jmp) SYS_JMP_ARM64="$TMP/$name.arm64" ;;
            sys_proc) SYS_PROC_ARM64="$TMP/$name.arm64" ;;
            sys_fork_callback) SYS_FORKCB_ARM64="$TMP/$name.arm64" ;;
        esac
    done
}

run_probe() {
    local out="$1" err="$2"
    shift 2
    run_bounded "$out" "$err" env "$PROBE_VAR=$PROBE_VAL" "$OCERZ" "$@" "$PROBE_BIN"
}

probe_group() {
    local file="$1" group="$2"
    grep -E "^$group " "$file" 2>/dev/null | head -1
}

case_native_dyn() {
    local kern="$1" name="native_dyn_$1" reason="" rc_jit rc_nojit
    local jo="$TMP/native.$kern.jit.out" je="$TMP/native.$kern.jit.err"
    local no="$TMP/native.$kern.nojit.out" ne="$TMP/native.$kern.nojit.err"

    run_bounded "$jo" "$je" "$OCERZ" -v -native "$DYN" "$kern" "$SCALE"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$DYN" "$kern" "$SCALE"
    rc_nojit=$?

    reason="$(native_run_reason "$rc_jit" "$jo" "$je")"
    if [ -z "$reason" ]; then
        reason="$(native_run_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif [ ! -s "$jo" ]; then
            reason="the guest ran but printed nothing"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit and no-jit stdout differ"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$(head -1 "$jo")'"
}

case_cache_dyn() {
    local kern="$1" name="cache_dyn_$1" rc_jit rc_nojit reason=""
    local jo="$TMP/cache.$kern.jit.out" no="$TMP/cache.$kern.nojit.out"

    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$jo" "$TMP/cache.$kern.jit.err" "$OCERZ" "$DYN" "$kern" "$SCALE"
    rc_jit=$?
    run_bounded "$no" "$TMP/cache.$kern.nojit.err" "$OCERZ" -no-jit "$DYN" "$kern" "$SCALE"
    rc_nojit=$?
    if [ "$rc_jit" -ne 0 ]; then
        reason="jit exit $rc_jit, want 0"
    elif [ "$rc_nojit" -ne 0 ]; then
        reason="no-jit exit $rc_nojit, want 0"
    elif [ ! -s "$jo" ]; then
        reason="no stdout"
    elif ! cmp -s "$jo" "$no"; then
        reason="jit and no-jit stdout differ"
    fi
    record "$name" "$reason" "out='$(head -1 "$jo")'"
}

case_native_matches_cache() {
    local kern="$1" name="native_matches_cache_$1" reason=""
    local nj="$TMP/native.$kern.jit.out" nn="$TMP/native.$kern.nojit.out"
    local cj="$TMP/cache.$kern.jit.out"

    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    if [ ! -s "$cj" ]; then
        reason="cache mode produced no stdout to compare against"
    elif [ ! -s "$nj" ]; then
        reason="native mode produced no stdout"
    elif ! cmp -s "$nj" "$cj"; then
        reason="native '$(head -1 "$nj")' != cache '$(head -1 "$cj")': a bridged call returned the wrong answer"
    elif ! cmp -s "$nn" "$cj"; then
        reason="native no-jit '$(head -1 "$nn")' != cache '$(head -1 "$cj")'"
    fi
    record "$name" "$reason" "out='$(head -1 "$cj")'"
}

case_native_bound() {
    local name=native_bound reason="" n
    n=$(grep -hF "$NOBIND" "$NOUT" "$NERR" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$n" != "0" ]; then
        reason="$n imports did not bind against the virtual libSystem"
    elif grep -Fq "$M0_SUMMARY" "$NOUT" "$NERR"; then
        reason="the M0 unresolved-import summary is still printed"
    fi
    record "$name" "$reason" "unbound=$n"
}

case_bridge_probe_heap() {
    local name=bridge_probe_heap reason="" line
    if [ -z "$PROBE_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    line="$(probe_group "$TMP/probe_native.jit.out" heap)"
    if [ -z "$line" ]; then
        reason="the guest never printed a heap line"
    elif [ "$line" != "heap ok" ]; then
        reason="$line: the guest could not read back memory the host heap handed it, so the address model is not what the bridge assumes"
    fi
    record "$name" "$reason" "${line:-no line}"
}

case_bridge_probe_env() {
    local name=bridge_probe_env reason="" line
    if [ -z "$PROBE_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    line="$(probe_group "$TMP/probe_native.jit.out" env)"
    if [ -z "$line" ]; then
        reason="the guest never printed an env line"
    elif [ "$line" != "env ok" ]; then
        reason="$line: the guest could not read $PROBE_VAR through a pointer into the host's own environment, so the address model is not what the bridge assumes"
    fi
    record "$name" "$reason" "${line:-no line}"
}

case_bridge_probe_native() {
    local name=bridge_probe_native reason="" rc_jit rc_nojit
    local jo="$TMP/probe_native.jit.out" je="$TMP/probe_native.jit.err"
    local no="$TMP/probe_native.nojit.out" ne="$TMP/probe_native.nojit.err"

    if [ -z "$PROBE_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    rc_jit=$(cat "$TMP/probe_native.jit.rc")
    rc_nojit=$(cat "$TMP/probe_native.nojit.rc")

    reason="$(native_run_reason "$rc_jit" "$jo" "$je")"
    if [ -z "$reason" ]; then
        reason="$(native_run_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$TMP/probe.want"; then
            reason="stdout is not the six all-ok lines: got '$(tr '\n' ' ' < "$jo")'"
        elif ! cmp -s "$no" "$TMP/probe.want"; then
            reason="no-jit stdout is not the six all-ok lines: got '$(tr '\n' ' ' < "$no")'"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit"
}

case_bridge_probe_cache() {
    local name=bridge_probe_cache reason="" rc
    local co="$TMP/probe_cache.out" ce="$TMP/probe_cache.err"
    local nj="$TMP/probe_native.jit.out"

    if [ -z "$PROBE_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_probe "$co" "$ce" -cache
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="cache-mode exit $rc, want 0"
    elif ! cmp -s "$co" "$TMP/probe.want"; then
        reason="cache mode is not the six all-ok lines either: got '$(tr '\n' ' ' < "$co")'"
    elif ! cmp -s "$nj" "$co"; then
        reason="native '$(tr '\n' ' ' < "$nj")' != cache '$(tr '\n' ' ' < "$co")'"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridge_unimpl() {
    local name=bridge_unimpl rc reason="" sym imports
    local out="$TMP/bridge_unimpl.out" err="$TMP/bridge_unimpl.err"

    if [ -z "$UNIMPL_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$UNIMPL_BIN"
    rc=$?
    sym=$(bridge_sym "$out" "$err")
    imports="$(nm -u "$UNIMPL_BIN" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    if [ -n "$imports" ] && [ "$imports" != "$UNIMPL_SYM " ]; then
        reason="the fixture imports '$imports' rather than $UNIMPL_SYM alone, so the symbol the bridge line names proves nothing"
    elif [ "$rc" -ne 72 ]; then
        reason="exit $rc, want 72"
    elif grep -Fq "$NOBIND" "$out" "$err"; then
        reason="an import went unresolved: $(grep -hF "$NOBIND" "$out" "$err" | head -1)"
    elif ! grep -qE "$BRIDGE_RE" "$out" "$err"; then
        reason="no 'ocerz: bridge: <dylib> <sym> not implemented' line"
    elif [ "$(grep -hE "$BRIDGE_RE" "$out" "$err" | head -1 | awk '{print $3}')" != "$LIB" ]; then
        reason="bridge named library $(grep -hE "$BRIDGE_RE" "$out" "$err" | head -1 | awk '{print $3}'), want $LIB"
    elif [ "$sym" != "$UNIMPL_SYM" ]; then
        reason="bridge named $sym, want $UNIMPL_SYM, the fixture's only import"
    fi
    record "$name" "$reason" "exit=$rc${sym:+ sym=$sym}"
}

bridge_stopped_reason() {
    if grep -Fq "$NOBIND" "$@" 2>/dev/null; then
        echo "an import went unresolved: $(grep -hF "$NOBIND" "$@" | head -1)"
    elif grep -qE "$BRIDGE_RE" "$@" 2>/dev/null; then
        echo "stopped at an unbridged export: $(grep -hE "$BRIDGE_RE" "$@" | head -1)"
    else
        echo ""
    fi
}

case_bridge_fault_native() {
    local name=bridge_fault_native rc reason=""
    local out="$TMP/badptr_native.out" err="$TMP/badptr_native.err"

    if [ -z "$BADPTR_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$BADPTR_BIN"
    rc=$?
    reason="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$reason" ]; then
        :
    elif ! grep -Fq "$BADPTR_MARK" "$out"; then
        reason="the guest never reached the bad call"
    elif ! grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        reason="no 'BRIDGE-FAULT ... inside a bridged call' report"
    elif ! grep -Fq "call=$LIB:$BADPTR_SYM" "$out" "$err"; then
        reason="the report does not name $LIB:$BADPTR_SYM"
    elif ! grep -Fq "sig='$BADPTR_SIG'" "$out" "$err"; then
        reason="the report does not name the signature $BADPTR_SIG"
    elif ! grep -qE 'host_fn=0x[0-9a-f]+ depth=[1-9]' "$out" "$err"; then
        reason="the report does not name the host function address and a crossing depth"
    elif ! grep -Fq "fault_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        reason="the report does not name $BAD_GUEST_ADDR as the faulting address"
    elif ! grep -qE 'host_pc=0x[0-9a-f]+' "$out" "$err"; then
        reason="the report does not name the host instruction pointer"
    elif ! grep -Fq "the guest passed a bad pointer to $BADPTR_SYM (guest addr $BAD_GUEST_ADDR)" "$out" "$err"; then
        reason="the report does not put $BAD_GUEST_ADDR in guest space and blame the guest's pointer"
    elif [ "$rc" -ne "$BRIDGE_FAULT_STATUS" ]; then
        reason="exit $rc, want $BRIDGE_FAULT_STATUS"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridge_fault_not_guest() {
    local name=bridge_fault_not_guest rc reason=""
    local out="$TMP/badptr_knobs.out" err="$TMP/badptr_knobs.err"
    local co="$TMP/after_knobs.out" ce="$TMP/after_knobs.err"

    if [ -z "$BADPTR_BIN" ] || [ -z "$AFTER_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" env OCERZ_FAULTLOG=1 OCERZ_SIGTRACE=1 \
        "$OCERZ" -native "$BADPTR_BIN"
    rc=$?
    run_bounded "$co" "$ce" env OCERZ_FAULTLOG=1 OCERZ_SIGTRACE=1 \
        "$OCERZ" -native "$AFTER_BIN"
    reason="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$reason" ]; then
        :
    elif ! grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        reason="the knobs changed the report away from a bridged-call fault"
    elif ! grep -Fq "$FAULTLOG_MARK" "$co" "$ce" || ! grep -Fq "$SIGTRACE_MARK" "$co" "$ce"; then
        reason="neither knob spoke for a plain guest fault at the same address, so their silence below proves nothing"
    elif grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        reason="the fault is still reported as a guest crash at an unrelated rip: $(grep -hF "$GUEST_CRASH" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$FAULTLOG_MARK" "$out" "$err"; then
        reason="OCERZ_FAULTLOG shows $BAD_GUEST_ADDR still went down the guest attribution path"
    elif grep -Fq "$SIGTRACE_MARK" "$out" "$err"; then
        reason="OCERZ_SIGTRACE shows the fault was still offered to the guest at $BAD_GUEST_ADDR"
    elif grep -qE "$WILD_RE" "$out" "$err"; then
        reason="a recovery path took the fault instead: $(grep -hE "$WILD_RE" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$BADPTR_PAST" "$out"; then
        reason="the guest carried on past a fault taken inside a native frame"
    elif [ "$rc" -ne "$BRIDGE_FAULT_STATUS" ]; then
        reason="exit $rc, want $BRIDGE_FAULT_STATUS"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridge_fault_cache() {
    local name=bridge_fault_cache rc rc_nojit reason=""
    local out="$TMP/badptr_cache.out" err="$TMP/badptr_cache.err"
    local no="$TMP/badptr_cache.nojit.out" ne="$TMP/badptr_cache.nojit.err"

    if [ -z "$BADPTR_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -cache "$BADPTR_BIN"
    rc=$?
    run_bounded "$no" "$ne" "$OCERZ" -cache -no-jit "$BADPTR_BIN"
    rc_nojit=$?
    if grep -qE "$BRIDGE_FAULT_RE" "$out" "$err" "$no" "$ne"; then
        reason="cache mode printed a bridged-call report, and cache mode crosses no bridge"
    elif ! grep -Fq "$BADPTR_MARK" "$out"; then
        reason="the guest never reached the bad call"
    elif grep -Fq "$BADPTR_PAST" "$out"; then
        reason="the guest carried on past its own fault"
    elif ! grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        reason="no guest-crash report: a plain guest fault stopped being reported as one"
    elif ! grep -Fq "guest_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        reason="the guest-crash report does not name $BAD_GUEST_ADDR as the address the guest dereferenced"
    elif [ "$rc" -ne "$GUEST_FAULT_STATUS" ]; then
        reason="exit $rc, want $GUEST_FAULT_STATUS"
    elif [ "$rc_nojit" -ne "$rc" ]; then
        reason="no-jit exit $rc_nojit, jit exit $rc: the two engines no longer agree on a guest fault"
    elif ! grep -Fq "$GUEST_CRASH" "$no" "$ne"; then
        reason="no-jit printed no guest-crash report"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridge_frame_lowered() {
    local name=bridge_frame_lowered rc reason=""
    local out="$TMP/after_native.out" err="$TMP/after_native.err"

    if [ -z "$AFTER_BIN" ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$AFTER_BIN"
    rc=$?
    reason="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$reason" ]; then
        :
    elif ! grep -Fq "$AFTER_MARK" "$out"; then
        reason="the bridged call did not return the answer the fixture expected"
    elif grep -Fq "$AFTER_PAST" "$out"; then
        reason="the guest carried on past its own fault"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        reason="a fault in guest code was blamed on a crossing that had already returned: $(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1)"
    elif ! grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        reason="no guest-crash report for a fault the guest took in its own code"
    elif ! grep -Fq "guest_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        reason="the guest-crash report does not name $BAD_GUEST_ADDR"
    elif [ "$rc" -ne "$GUEST_FAULT_STATUS" ]; then
        reason="exit $rc, want $GUEST_FAULT_STATUS"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bridgelog() {
    local name=bridgelog rc rc_plain reason="" n sym
    local lo="$TMP/bridgelog.out" le="$TMP/bridgelog.err"
    local po="$TMP/bridgelog_plain.out" pe="$TMP/bridgelog_plain.err"

    run_bounded "$po" "$pe" "$OCERZ" -v -native "$DYN" "$BRIDGELOG_KERNEL" "$SCALE"
    rc_plain=$?
    run_bounded "$lo" "$le" env OCERZ_BRIDGELOG=1 \
        "$OCERZ" -v -native "$DYN" "$BRIDGELOG_KERNEL" "$SCALE"
    rc=$?
    n=$(grep -cE "$BRIDGELOG_RE" "$le" 2>/dev/null | tr -d ' ')
    sym=$(grep -E "$BRIDGELOG_RE" "$le" 2>/dev/null | head -1 | awk '{print $4}')

    reason="$(native_run_reason "$rc_plain" "$po" "$pe")"
    if [ -n "$reason" ]; then
        reason="without OCERZ_BRIDGELOG: $reason"
    elif [ "$n" = "0" ]; then
        reason="OCERZ_BRIDGELOG printed no 'ocerz: BRIDGELOG[pid] <lib> <sym> <sig>' line"
    elif [ -n "$(grep -E "$BRIDGELOG_RE" "$le" | awk -v l="$LIB" '$3 != l' | head -1)" ]; then
        reason="a log line names library $(grep -E "$BRIDGELOG_RE" "$le" | awk -v l="$LIB" '$3 != l' | head -1 | awk '{print $3}'), want $LIB"
    elif [ -n "$(nm -u "$DYN" 2>/dev/null)" ] &&
         ! nm -u "$DYN" 2>/dev/null | awk '{print $1}' | grep -Fqx "$sym"; then
        reason="the log names $sym, which is not in the fixture's import table"
    elif [ -z "$(grep -E "$BRIDGELOG_RE" "$le" | awk -v s="$BRIDGELOG_SYM" -v g="$BRIDGELOG_SIG" '$4 == s && $5 == g' | head -1)" ]; then
        reason="no logged crossing is '$BRIDGELOG_SYM $BRIDGELOG_SIG', which the $BRIDGELOG_KERNEL kernel must make"
    elif [ "$rc" -ne "$rc_plain" ]; then
        reason="exit $rc with OCERZ_BRIDGELOG set, $rc_plain without"
    elif ! cmp -s "$lo" "$po"; then
        reason="stdout differs with OCERZ_BRIDGELOG set: the diagnostic changed what it was watching"
    fi
    record "$name" "$reason" "exit=$rc lines=$n"
}

callback_fixture_missing() {
    local name="$1" bin="$2"
    if [ -n "$bin" ]; then
        return 1
    fi
    if [ "$X86_CLANG" -ne 1 ]; then
        echo "SKIP $name (no x86_64 clang toolchain)"
    else
        record "$name" "the fixture did not compile although x86_64 clang builds a trivial program: $( (grep -m1 -i 'error' "$TMP/$name.cc.log" || head -1 "$TMP/$name.cc.log") 2>/dev/null | cut -c1-160)"
    fi
    return 0
}

callback_run_reason() {
    local rc="$1" reason
    shift
    reason="$(native_run_reason "$rc" "$@")"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -qE "$BRIDGE_FAULT_RE" "$@" 2>/dev/null; then
        echo "$reason; $(grep -hE "$BRIDGE_FAULT_RE" "$@" | head -1 | cut -c1-120)"
    elif grep -Fq "$GUEST_CRASH" "$@" 2>/dev/null; then
        echo "$reason; $(grep -hF "$GUEST_CRASH" "$@" | head -1 | cut -c1-120)"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason: still running after ${NATIVE_TIMEOUT}s, so a callback never came back"
    else
        echo "$reason"
    fi
}

case_callback() {
    local name="$1" bin="$2" tag="$3" bits="$4" reason="" rc_jit rc_nojit rc_cache line
    local cache_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(head -1 "$jo")"

    reason="$(callback_run_reason "$rc_jit" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed, where $bits"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi

    if [ -z "$reason" ]; then
        reason="$(callback_run_reason "$rc_nojit" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(head -1 "$no")': the guest's own checks failed, where $bits"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin"
        rc_cache=$?
        if [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(head -1 "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': a callback crossing changed what the guest computed"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

callback_fault_reason() {
    local rc="$1" out="$2" err="$3" stopped
    stopped="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$stopped" ]; then
        echo "$stopped"
    elif ! grep -Fq "$CB_FAULT_MARK" "$out"; then
        echo "the guest never reached its qsort call"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        echo "the comparator's own read of $BAD_GUEST_ADDR was reported as a fault inside a bridged call ($(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-80)): M5 clears the thread's bridge frame while guest code runs precisely so that a fault in a callback is the guest's, and the frame was still raised when the comparator faulted"
    elif grep -Fq "$CB_FAULT_PAST" "$out"; then
        echo "qsort returned without the comparator faulting, so the callback never ran the guest's comparator"
    elif [ "$rc" -eq 124 ]; then
        echo "still running after ${NATIVE_TIMEOUT}s: the fault inside the callback hung instead of stopping the process"
    elif ! grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        echo "no guest-crash report for a fault the guest took in its own comparator"
    elif ! grep -Fq "guest_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        echo "the guest-crash report does not name $BAD_GUEST_ADDR, the address the comparator read"
    elif [ "$rc" -ne "$GUEST_FAULT_STATUS" ]; then
        echo "exit $rc, want $GUEST_FAULT_STATUS"
    else
        echo ""
    fi
}

case_callback_guest_fault() {
    local name=callback_guest_fault reason="" rc_jit rc_nojit
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"

    if callback_fixture_missing "$name" "$CB_FAULT_BIN"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$CB_FAULT_BIN"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$CB_FAULT_BIN"
    rc_nojit=$?
    reason="$(callback_fault_reason "$rc_jit" "$jo" "$je")"
    if [ -z "$reason" ]; then
        reason="$(callback_fault_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit no-jit exit=$rc_nojit"
}

attach_count() {
    local n
    n=$(grep -cE "$1" "$2" 2>/dev/null)
    echo "${n:-0}"
}

attach_stall() {
    local tag="$1" err="$2" call n_call n_back n_in n_sig n_wait
    call="$(grep -hE "^$tag: calling " "$err" 2>/dev/null | tail -1 | sed "s/^$tag: calling //")"
    n_call=$(attach_count "^$tag: calling " "$err")
    n_back=$(attach_count "^$tag: returned from " "$err")
    n_in=$(attach_count "^$tag: work entered$" "$err")
    n_sig=$(attach_count "^$tag: signalling$" "$err")
    n_wait=$(attach_count "^$tag: wait returned$" "$err")
    if [ "$n_call" -eq 0 ]; then
        echo "the guest never reached its first dispatch call"
    elif [ "$n_back" -lt "$n_call" ]; then
        case $call in
            dispatch_apply_f)
                echo "dispatch_apply_f never returned: it waits for every iteration, so an iteration it handed to a libdispatch worker never came back" ;;
            dispatch_sync_f)
                if [ "$n_in" -lt "$n_call" ]; then
                    echo "dispatch_sync_f never returned, and its work function never entered guest code"
                else
                    echo "dispatch_sync_f never returned, although its work function entered guest code"
                fi ;;
            *)
                echo "$call never returned" ;;
        esac
    elif [ "$call" != "dispatch_async_f" ]; then
        echo "$call returned and the process still did not exit"
    elif [ "$n_in" -lt "$n_call" ]; then
        echo "the work function never entered guest code on the libdispatch worker, so the main thread waits in dispatch_semaphore_wait for a signal that cannot come"
    elif [ "$n_sig" -lt "$n_in" ]; then
        echo "the work function entered guest code on the worker but never reached its dispatch_semaphore_signal"
    elif [ "$n_wait" -lt "$n_sig" ]; then
        echo "the worker signalled, but the main thread's dispatch_semaphore_wait never returned"
    else
        echo "every dispatch call and every wait returned, and the process still did not exit"
    fi
}

attach_run_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" reason
    reason="$(callback_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -Fq "$ATTACH_REFUSED" "$out" "$err" 2>/dev/null; then
        echo "$reason; the callback was refused rather than given a personality: $(grep -hF "$ATTACH_REFUSED" "$out" "$err" | head -1 | cut -c1-240)"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason; $(attach_stall "$tag" "$err")"
    else
        echo "$reason"
    fi
}

case_attach() {
    local name="$1" bin="$2" tag="$3" bits="$4" note="${5:-}"
    local reason="" rc_jit rc_nojit rc_cache line threads cache_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$ATTACH_TIMEOUT

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(head -1 "$jo")"
    threads="$(grep -hE "^$tag: threads=" "$je" 2>/dev/null | head -1 | sed "s/^$tag: //")"

    reason="$(attach_run_reason "$rc_jit" "$tag" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed, where $bits"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi

    if [ -z "$reason" ]; then
        reason="$(attach_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(head -1 "$no")': the guest's own checks failed, where $bits"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    if [ -n "$reason" ] && [ -n "$note" ]; then
        reason="$reason. $note"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin"
        rc_cache=$?
        if [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(head -1 "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': a work function run on an attached thread computed something the real libdispatch's threads did not"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'${threads:+ $threads}$cache_note"
}

attach_fault_reason() {
    local rc="$1" out="$2" err="$3" stopped
    stopped="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$stopped" ]; then
        echo "$stopped"
    elif ! grep -Fq "$AT_FAULT_MARK" "$out"; then
        echo "the guest never reached its dispatch_async_f call"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        echo "the work function's own read of $BAD_GUEST_ADDR on a libdispatch worker was reported as a fault inside a bridged call ($(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-80)): the worker was running guest code when it faulted, so a bridged-call report means the attached thread's bridge frame was not cleared while its callback ran"
    elif grep -Fq "$ATTACH_REFUSED" "$out" "$err"; then
        echo "the work function was refused rather than given a personality: $(grep -hF "$ATTACH_REFUSED" "$out" "$err" | head -1 | cut -c1-240)"
    elif ! grep -Fq "$AT_FAULT_WORKER" "$out"; then
        if [ "$rc" -eq 124 ]; then
            echo "still running after ${NATIVE_TIMEOUT}s, and the work function never entered guest code on the worker"
        else
            echo "exit $rc, and the work function never entered guest code on the worker"
        fi
    elif grep -Fq "$AT_FAULT_SURVIVED" "$out"; then
        echo "the work function read $BAD_GUEST_ADDR on the worker without faulting"
    elif grep -Fq "$AT_FAULT_PAST" "$out"; then
        echo "the main thread carried on past a fault its work function took on the worker"
    elif grep -qE "$WILD_RE" "$out" "$err"; then
        echo "exit $rc: a recovery path took the worker's fault instead of reporting a guest crash: $(grep -hE "$WILD_RE" "$out" "$err" | head -1 | cut -c1-120)"
    elif [ "$rc" -eq 124 ]; then
        echo "still running after ${NATIVE_TIMEOUT}s: the work function reached its read of $BAD_GUEST_ADDR on the worker and the process never stopped, so the fault was swallowed or only the worker was stopped, and the main thread waits forever for a signal the worker never sends"
    elif ! grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        echo "no guest-crash report for a fault the guest took in its own work function on an attached thread"
    elif ! grep -Fq "guest_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        echo "the guest-crash report does not name $BAD_GUEST_ADDR, the address the work function read"
    elif [ "$rc" -ne "$GUEST_FAULT_STATUS" ]; then
        echo "exit $rc, want $GUEST_FAULT_STATUS"
    else
        echo ""
    fi
}

case_attach_guest_fault() {
    local name=attach_guest_fault reason="" rc_jit rc_nojit
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local NATIVE_TIMEOUT=$ATTACH_TIMEOUT

    if callback_fixture_missing "$name" "$AT_FAULT_BIN"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$AT_FAULT_BIN"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$AT_FAULT_BIN"
    rc_nojit=$?
    reason="$(attach_fault_reason "$rc_jit" "$jo" "$je")"
    if [ -z "$reason" ]; then
        reason="$(attach_fault_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit no-jit exit=$rc_nojit"
}

thread_guard_reason() {
    local imports
    imports="$(nm -u "$1" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    if [ -n "$imports" ] && ! printf '%s\n' $imports | grep -Fqx "$STACK_GUARD_SYM"; then
        echo "the fixture imports '${imports% }' and not $STACK_GUARD_SYM, so the stack protector was not emitted and the case proves nothing about the virtual libSystem's data export"
    fi
}

thread_guard_hint() {
    if grep -Fq "$NOBIND$STACK_GUARD_SYM " "$@" 2>/dev/null; then
        echo ": the virtual libSystem does not export $STACK_GUARD_SYM, the data symbol every stack-protected x86_64 program imports"
    fi
}

thread_stall() {
    local tag="$1" err="$2" mode="${3:-native}" n_create n_created n_in n_out n_wait n_waited n_join n_joined
    n_create=$(attach_count "^$tag: calling pthread_create$" "$err")
    n_created=$(attach_count "^$tag: returned from pthread_create$" "$err")
    n_in=$(attach_count "^$tag: thread entered$" "$err")
    n_out=$(attach_count "^$tag: thread leaving$" "$err")
    n_wait=$(attach_count "^$tag: waiting$" "$err")
    n_waited=$(attach_count "^$tag: wait returned$" "$err")
    n_join=$(attach_count "^$tag: calling pthread_join$" "$err")
    n_joined=$(attach_count "^$tag: returned from pthread_join$" "$err")
    if [ "$n_create" -eq 0 ]; then
        echo "the guest never reached its first pthread_create"
    elif [ "$n_created" -lt "$n_create" ]; then
        echo "pthread_create never returned: $n_created of $n_create calls came back"
    elif [ "$n_in" -lt "$n_created" ]; then
        if [ "$mode" = cache ]; then
            echo "only $n_in of the $n_created threads pthread_create started entered their start routine$( [ "$n_joined" -lt "$n_join" ] && echo ", and the main thread waits in pthread_join for one that never ran")"
        else
            echo "only $n_in of the $n_created threads pthread_create started entered their start routine, so a host thread never reached guest code through the callback trampoline$( [ "$n_joined" -lt "$n_join" ] && echo ", and the main thread waits in pthread_join for it")"
        fi
    elif [ "$n_out" -lt "$n_in" ]; then
        echo "$((n_in - n_out)) of $n_in start routines entered guest code and never returned, so a guest thread is blocked, most likely in a mutex or condition wait that nothing wakes"
    elif [ "$n_waited" -lt "$n_wait" ]; then
        echo "every start routine returned, and the main thread's own condition wait never did"
    elif [ "$n_joined" -lt "$n_join" ]; then
        echo "pthread_join never returned although every start routine did, so a thread never finished leaving the trampoline and exiting"
    else
        echo "every thread was created, ran and was joined, and the process still did not exit"
    fi
}

thread_run_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" reason
    reason="$(callback_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -Fq "$NOBIND$STACK_GUARD_SYM " "$out" "$err" 2>/dev/null; then
        echo "$reason$(thread_guard_hint "$out" "$err")"
    elif grep -Fq "$ATTACH_REFUSED" "$out" "$err" 2>/dev/null; then
        echo "$reason; a start routine was refused rather than given a personality: $(grep -hF "$ATTACH_REFUSED" "$out" "$err" | head -1 | cut -c1-240)"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason; $(thread_stall "$tag" "$err")"
    else
        echo "$reason"
    fi
}

case_thread() {
    local name="$1" bin="$2" tag="$3" bits="$4" note="${5:-}"
    local reason="" rc_jit rc_nojit rc_cache line cache_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$THREAD_TIMEOUT

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(head -1 "$jo")"

    reason="$(thread_guard_reason "$bin")"
    if [ -z "$reason" ]; then
        reason="$(thread_run_reason "$rc_jit" "$tag" "$jo" "$je")"
        if grep -q "^$tag bad:" "$jo"; then
            reason="'$line': the guest's own checks failed, where $bits"
        elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
            reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
        fi
    fi

    if [ -z "$reason" ]; then
        reason="$(thread_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(head -1 "$no")': the guest's own checks failed, where $bits"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    if [ -n "$reason" ] && [ -n "$note" ]; then
        reason="$reason. $note"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin"
        rc_cache=$?
        if [ "$rc_cache" -eq 124 ]; then
            reason="cache mode still running after ${NATIVE_TIMEOUT}s, which crosses no bridge and attaches nothing: $(thread_stall "$tag" "$ce" cache)"
        elif [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(head -1 "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': a start routine run on a thread native pthread_create started computed something the guest's own threads did not in cache mode"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

thread_fault_reason() {
    local rc="$1" out="$2" err="$3" stopped
    stopped="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$stopped" ]; then
        echo "$stopped$(thread_guard_hint "$out" "$err")"
    elif ! grep -Fq "$TH_FAULT_MARK" "$out"; then
        echo "the guest never reached its pthread_create call"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        echo "the start routine's own read of $BAD_GUEST_ADDR was reported as a fault inside a bridged call ($(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-80)): the thread native pthread_create started was running guest code when it faulted, so a bridged-call report means that thread's bridge frame was raised while its start routine ran"
    elif grep -Fq "$ATTACH_REFUSED" "$out" "$err"; then
        echo "the start routine was refused rather than given a personality: $(grep -hF "$ATTACH_REFUSED" "$out" "$err" | head -1 | cut -c1-240)"
    elif grep -Fq "$TH_FAULT_NOCREATE" "$out"; then
        echo "pthread_create failed, so no thread ever ran the start routine"
    elif ! grep -Fq "$TH_FAULT_THREAD" "$out"; then
        if [ "$rc" -eq 124 ]; then
            echo "still running after ${NATIVE_TIMEOUT}s, and the start routine never entered guest code on the new thread"
        else
            echo "exit $rc, and the start routine never entered guest code on the new thread"
        fi
    elif grep -Fq "$TH_FAULT_SURVIVED" "$out"; then
        echo "the start routine read $BAD_GUEST_ADDR without faulting"
    elif grep -Fq "$TH_FAULT_PAST" "$out"; then
        echo "pthread_join returned and the main thread carried on past a fault its start routine took"
    elif grep -qE "$WILD_RE" "$out" "$err"; then
        echo "exit $rc: a recovery path took the thread's fault instead of reporting a guest crash: $(grep -hE "$WILD_RE" "$out" "$err" | head -1 | cut -c1-120)"
    elif [ "$rc" -eq 124 ]; then
        echo "still running after ${NATIVE_TIMEOUT}s: the start routine reached its read of $BAD_GUEST_ADDR and the process never stopped, so the fault was swallowed or only the thread was stopped, and the main thread waits in pthread_join for a thread that will not finish"
    elif ! grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        echo "no guest-crash report for a fault the guest took in its own start routine"
    elif ! grep -Fq "guest_addr=$BAD_GUEST_ADDR" "$out" "$err"; then
        echo "the guest-crash report does not name $BAD_GUEST_ADDR, the address the start routine read"
    elif [ "$rc" -ne "$GUEST_FAULT_STATUS" ]; then
        echo "exit $rc, want $GUEST_FAULT_STATUS"
    else
        echo ""
    fi
}

case_thread_guest_fault() {
    local name=thread_guest_fault reason="" rc_jit rc_nojit
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local NATIVE_TIMEOUT=$THREAD_TIMEOUT

    if callback_fixture_missing "$name" "$TH_FAULT_BIN"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$TH_FAULT_BIN"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$TH_FAULT_BIN"
    rc_nojit=$?
    reason="$(thread_guard_reason "$TH_FAULT_BIN")"
    if [ -z "$reason" ]; then
        reason="$(thread_fault_reason "$rc_jit" "$jo" "$je")"
    fi
    if [ -z "$reason" ]; then
        reason="$(thread_fault_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit no-jit exit=$rc_nojit"
}

tlv_import_reason() {
    local bin="$1" lib="${2:-}" imports allowed sym stray=""
    imports="$(nm -u "$bin" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    if [ -z "$imports" ]; then
        return
    fi
    allowed=" $TLV_BRIDGED $TLV_BOOTSTRAP_SYM $STACK_GUARD_SYM "
    if [ -n "$lib" ]; then
        allowed="$allowed$(nm -gU "$lib" 2>/dev/null | awk '{print $3}' | tr '\n' ' ')"
    fi
    for sym in $imports; do
        case "$allowed" in
            *" $sym "*) ;;
            *) stray="$stray $sym" ;;
        esac
    done
    case " $imports" in
        *" $TLV_BOOTSTRAP_SYM "*) ;;
        *)
            echo "$(basename "$bin") imports '${imports% }' and not $TLV_BOOTSTRAP_SYM, so its thread-local variables were not compiled as descriptors bound to that thunk and the case proves nothing about them"
            return ;;
    esac
    case " $imports" in
        *" $STACK_GUARD_SYM "*) ;;
        *)
            echo "$(basename "$bin") imports '${imports% }' and not $STACK_GUARD_SYM, so the stack protector was not emitted and the fixture no longer binds the data export every real program imports beside its thread-local variables"
            return ;;
    esac
    if [ -n "$stray" ]; then
        echo "$(basename "$bin") imports$stray, which the bridge does not implement, so a failure would be about those imports and not about thread-local variables"
    fi
}

tlv_run_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" reason
    reason="$(native_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -Fq "$NOBIND$TLV_BOOTSTRAP_SYM " "$out" "$err" 2>/dev/null; then
        echo "$reason: the virtual libSystem does not export $TLV_BOOTSTRAP_SYM, the thunk every thread-local descriptor is bound to, so the program never ran"
    elif grep -Fq "${NOBIND}_tlvdep_" "$out" "$err" 2>/dev/null; then
        echo "$reason: nothing bound from $TLV_DEP_NAME, so the loader never found the dylib the fixture links against, installed as @executable_path/$TLV_DEP_NAME beside it, and no thread-local variable was reached"
    elif grep -Fq "$NOBIND$STACK_GUARD_SYM " "$out" "$err" 2>/dev/null; then
        echo "$reason$(thread_guard_hint "$out" "$err")"
    elif grep -Fq "$TLV_UNRESOLVED" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hF "$TLV_UNRESOLVED" "$out" "$err" | head -1 | cut -c1-160): ocerz could not answer the thunk for that descriptor, which is how a descriptor the loader never registered looks"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$GUEST_CRASH" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hF "$GUEST_CRASH" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$ATTACH_REFUSED" "$out" "$err" 2>/dev/null; then
        echo "$reason; a start routine was refused rather than given a personality: $(grep -hF "$ATTACH_REFUSED" "$out" "$err" | head -1 | cut -c1-240)"
    elif [ "$rc" -eq 124 ] && grep -q "^$tag: calling pthread_create$" "$err" 2>/dev/null; then
        echo "$reason; $(thread_stall "$tag" "$err")"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason: still running after ${NATIVE_TIMEOUT}s, and the last progress note was '$(grep -h "^$tag: " "$err" 2>/dev/null | tail -1)'"
    else
        echo "$reason"
    fi
}

tlv_engines_reason() {
    local rc_jit="$1" rc_nojit="$2" tag="$3" bits="$4" jo="$5" je="$6" no="$7" ne="$8" reason line
    line="$(head -1 "$jo")"
    reason="$(tlv_run_reason "$rc_jit" "$tag" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed, where $bits"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi
    if [ -z "$reason" ]; then
        reason="$(tlv_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(head -1 "$no")': the guest's own checks failed, where $bits"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    echo "$reason"
}

tlv_cache_reason() {
    local tag="$1" jo="$2" co="$3" ce="$4" rc
    shift 4
    run_bounded "$co" "$ce" "$OCERZ" -cache "$@"
    rc=$?
    if [ "$rc" -eq 124 ] && grep -q "^$tag: calling pthread_create$" "$ce" 2>/dev/null; then
        echo "cache mode still running after ${NATIVE_TIMEOUT}s, which crosses no bridge: $(thread_stall "$tag" "$ce" cache)"
    elif [ "$rc" -eq 124 ]; then
        echo "cache mode still running after ${NATIVE_TIMEOUT}s, which crosses no bridge"
    elif [ "$rc" -ne 0 ]; then
        echo "cache-mode exit $rc, want 0: '$(head -1 "$co")'"
    elif ! cmp -s "$jo" "$co"; then
        echo "native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': the blocks ocerz hands out gave the guest something the real tlv_get_addr did not"
    fi
}

case_tlv() {
    local name="$1" bin="$2" tag="$3" bits="$4" lib="${5:-}"
    local reason="" rc_jit rc_nojit line cache_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$TLV_TIMEOUT

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(head -1 "$jo")"

    reason="$(tlv_import_reason "$bin" "$lib")"
    if [ -z "$reason" ] && [ -n "$lib" ]; then
        reason="$(tlv_import_reason "$lib")"
    fi
    if [ -z "$reason" ]; then
        reason="$(tlv_engines_reason "$rc_jit" "$rc_nojit" "$tag" "$bits" "$jo" "$je" "$no" "$ne")"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        reason="$(tlv_cache_reason "$tag" "$jo" "$co" "$ce" "$bin")"
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

tlv_measured() {
    local out="$1" err="$2" rc
    shift 2
    if [ ! -x "$MEASURE_BIN" ]; then
        run_bounded "$out" "$err" "$@"
        return $?
    fi
    run_bounded "$out" "$err" "$MEASURE_BIN" -l "$@"
    rc=$?
    if [ "$rc" -eq 124 ]; then
        pkill -KILL -f "$TLV_CHURN_BIN" 2>/dev/null
    fi
    return $rc
}

tlv_footprint() {
    awk '/ peak memory footprint$/ { print $1; found = 1; exit }
         / maximum resident set size$/ && rss == "" { rss = $1 }
         END { if (!found && rss != "") print rss }' "$1" 2>/dev/null
}

case_tlv_churn() {
    local name=tlv_thread_churn tag=tlv_thread_churn bits="$1"
    local reason="" rc_jit rc_nojit rc_base line t0 secs_jit secs_nojit foot foot_base grew limit
    local cache_note="" mem_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local bo="$TMP/$name.base.out" be="$TMP/$name.base.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$TLV_TIMEOUT

    if callback_fixture_missing "$name" "$TLV_CHURN_BIN"; then
        return
    fi
    t0=$SECONDS
    tlv_measured "$jo" "$je" "$OCERZ" -v -native "$TLV_CHURN_BIN" "$TLV_CHURN_THREADS"
    rc_jit=$?
    secs_jit=$((SECONDS - t0))
    t0=$SECONDS
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$TLV_CHURN_BIN" "$TLV_CHURN_THREADS"
    rc_nojit=$?
    secs_nojit=$((SECONDS - t0))
    line="$(head -1 "$jo")"

    reason="$(tlv_import_reason "$TLV_CHURN_BIN")"
    if [ -z "$reason" ]; then
        reason="$(tlv_engines_reason "$rc_jit" "$rc_nojit" "$tag" "$bits" "$jo" "$je" "$no" "$ne")"
    fi
    if [ -n "$reason" ]; then
        :
    elif [ "$secs_jit" -gt "$TLV_CHURN_BUDGET" ] || [ "$secs_nojit" -gt "$TLV_CHURN_BUDGET" ]; then
        reason="$TLV_CHURN_THREADS threads took ${secs_jit}s under the JIT and ${secs_nojit}s under -no-jit, over the ${TLV_CHURN_BUDGET}s budget that is half the run's ${TLV_TIMEOUT}s bound: a cost that grows with every thread created, such as blocks never freed or a table searched from the start of the process, looks like this before it looks like a timeout"
    else
        tlv_measured "$bo" "$be" "$OCERZ" -v -native "$TLV_CHURN_BIN" "$TLV_CHURN_BASE"
        rc_base=$?
        foot="$(tlv_footprint "$je")"
        foot_base="$(tlv_footprint "$be")"
        if [ "$rc_base" -ne 0 ] || ! grep -q "^$tag ok" "$bo"; then
            reason="the $TLV_CHURN_BASE-thread run the footprint is measured against did not pass: exit $rc_base, '$(head -1 "$bo")'"
        elif [ -z "$foot" ] || [ -z "$foot_base" ]; then
            mem_note=" footprint=unmeasured"
        else
            grew=$((foot - foot_base))
            limit=$(((TLV_CHURN_THREADS - TLV_CHURN_BASE) * TLV_CHURN_BLOCK / 4))
            mem_note=" footprint=$((grew / 1048576))MB"
            if [ "$grew" -gt "$limit" ]; then
                reason="peak memory footprint $((foot / 1048576)) MB with $TLV_CHURN_THREADS threads against $((foot_base / 1048576)) MB with $TLV_CHURN_BASE, $((grew / 1048576)) MB more where $((limit / 1048576)) MB is allowed: more than a quarter of a $((TLV_CHURN_BLOCK / 1048576)) MB thread-local block is kept for every thread that has gone away, so a thread's blocks are not freed when it exits"
            fi
        fi
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        reason="$(tlv_cache_reason "$tag" "$jo" "$co" "$ce" "$TLV_CHURN_BIN" "$TLV_CHURN_THREADS")"
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line' secs=$secs_jit/$secs_nojit$mem_note$cache_note"
}

signal_import_reason() {
    local bin="$1" imports allowed sym stray=""
    imports="$(nm -u "$bin" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    allowed=" $SIGNAL_BRIDGED $STACK_GUARD_SYM "
    for sym in $imports; do
        case "$allowed" in
            *" $sym "*) ;;
            *) stray="$stray $sym" ;;
        esac
    done
    if [ -n "$stray" ]; then
        echo "$(basename "$bin") imports$stray, which the bridge does not implement, so a failure would be about those imports and not about signals"
    fi
}

signal_status() {
    local tag="$1" file="$2" line
    line="$(grep -E "^$tag (ok|bad:)" "$file" 2>/dev/null | head -1)"
    if [ -z "$line" ]; then
        line="$(head -1 "$file" 2>/dev/null)"
    fi
    echo "$line"
}

signal_stall() {
    local tag="$1" err="$2" last
    last="$(grep -h "^$tag: " "$err" 2>/dev/null | tail -1 | sed "s/^$tag: //")"
    case $last in
        "")
            echo "the guest never wrote its first progress note" ;;
        raising*|calling*|blocking*|unblocking*)
            echo "the last progress note was '$last', so that call never returned: a handler entered at its return that never came back, or a delivery that repeats forever, looks like this" ;;
        *)
            echo "the last progress note was '$last'" ;;
    esac
}

signal_run_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" reason
    reason="$(native_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -Fq "$NOBIND$STACK_GUARD_SYM " "$out" "$err" 2>/dev/null; then
        echo "$reason$(thread_guard_hint "$out" "$err")"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$GUEST_CRASH" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hF "$GUEST_CRASH" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$ATTACH_REFUSED" "$out" "$err" 2>/dev/null; then
        echo "$reason; guest code was refused rather than given a personality: $(grep -hF "$ATTACH_REFUSED" "$out" "$err" | head -1 | cut -c1-240)"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason: still running after ${NATIVE_TIMEOUT}s, and $(signal_stall "$tag" "$err")"
    elif [ "$rc" -eq 158 ] || [ "$rc" -eq 159 ]; then
        echo "$reason: the process was ended by $([ "$rc" -eq 158 ] && echo SIGUSR1 || echo SIGUSR2)'s default action, so a signal the guest had a handler for was never handed to it; $(signal_stall "$tag" "$err")"
    elif [ "$rc" -gt 128 ] && [ "$rc" -lt 160 ]; then
        echo "$reason: the process was ended by signal $((rc - 128)); $(signal_stall "$tag" "$err")"
    else
        echo "$reason"
    fi
}

case_signal() {
    local name="$1" bin="$2" tag="$3" bits="$4" note="${5:-}"
    local reason="" rc_jit rc_nojit rc_cache line cache_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$SIGNAL_TIMEOUT

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin" 2>/dev/null
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin" 2>/dev/null
    rc_nojit=$?
    line="$(signal_status "$tag" "$jo")"

    reason="$(signal_import_reason "$bin")"
    if [ -z "$reason" ]; then
        reason="$(signal_run_reason "$rc_jit" "$tag" "$jo" "$je")"
        if grep -q "^$tag bad:" "$jo"; then
            reason="'$line': the guest's own checks failed, where $bits"
        elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
            reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
        fi
    fi

    if [ -z "$reason" ]; then
        reason="$(signal_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(signal_status "$tag" "$no")': the guest's own checks failed, where $bits"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    if [ -n "$reason" ] && [ -n "$note" ]; then
        reason="$reason. $note"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin" 2>/dev/null
        rc_cache=$?
        if [ "$rc_cache" -eq 124 ]; then
            reason="cache mode still running after ${NATIVE_TIMEOUT}s, which crosses no bridge, and $(signal_stall "$tag" "$ce")"
        elif [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(signal_status "$tag" "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': a signal delivered at a crossing did something a signal delivered at a syscall did not"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

signal_default_reason() {
    local rc="$1" out="$2" err="$3" stopped
    stopped="$(bridge_stopped_reason "$out" "$err")"
    if [ -n "$stopped" ]; then
        echo "$stopped"
    elif ! grep -Fq "$SIG_DEFAULT_WINCH" "$out"; then
        echo "exit $rc, and the guest never reached its raise of SIGWINCH"
    elif ! grep -Fq "$SIG_DEFAULT_MARK" "$out"; then
        echo "exit $rc inside raise(SIGWINCH): SIGWINCH is ignored by default, so a signal with no handler was given the default action of one that ends the process"
    elif grep -Fq "$SIG_DEFAULT_PAST" "$out"; then
        echo "raise(SIGTERM) returned with no handler installed and the guest carried on to exit $rc: the default action, which ends the process, was never taken"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err"; then
        echo "a bridged-call fault instead of the default action: $(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-120)"
    elif grep -Fq "$GUEST_CRASH" "$out" "$err"; then
        echo "a guest-crash report instead of the default action: $(grep -hF "$GUEST_CRASH" "$out" "$err" | head -1 | cut -c1-120)"
    elif [ "$rc" -eq 124 ]; then
        echo "still running after ${NATIVE_TIMEOUT}s: raise(SIGTERM) neither returned nor ended the process"
    elif [ "$rc" -ne "$SIG_DEFAULT_STATUS" ]; then
        echo "exit $rc, want $SIG_DEFAULT_STATUS, the status of a process SIGTERM's default action ended"
    else
        echo ""
    fi
}

case_signal_default() {
    local name=signal_default reason="" rc_jit rc_nojit rc_cache cache_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$SIGNAL_TIMEOUT

    if callback_fixture_missing "$name" "$SIG_DEFAULT_BIN"; then
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$SIG_DEFAULT_BIN" 2>/dev/null
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$SIG_DEFAULT_BIN" 2>/dev/null
    rc_nojit=$?

    reason="$(signal_import_reason "$SIG_DEFAULT_BIN")"
    if [ -z "$reason" ]; then
        reason="$(signal_default_reason "$rc_jit" "$jo" "$je")"
    fi
    if [ -z "$reason" ]; then
        reason="$(signal_default_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$SIG_DEFAULT_BIN" 2>/dev/null
        rc_cache=$?
        if [ "$rc_cache" -ne "$rc_jit" ]; then
            reason="cache-mode exit $rc_cache, native exit $rc_jit: the two modes no longer end a process the same way when a signal with no handler arrives"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': the two modes stopped the guest at different places"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit no-jit exit=$rc_nojit$cache_note"
}

cf_import_reason() {
    local bin="$1" need="${2:-}" imports allowed sym stray=""
    imports="$(nm -u "$bin" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    if [ -z "$imports" ]; then
        return
    fi
    allowed=" $CF_BRIDGED $CF_EXPORTS $STACK_GUARD_SYM "
    for sym in $imports; do
        case "$allowed" in
            *" $sym "*) ;;
            *) stray="$stray $sym" ;;
        esac
    done
    if [ -n "$need" ]; then
        case " $imports" in
            *" $need "*) ;;
            *)
                echo "$(basename "$bin") imports '${imports% }' and not $need, so its CFSTR literals were not compiled as constant strings whose isa binds to that data export, and the case proves nothing about it"
                return ;;
        esac
    fi
    if [ -n "$stray" ]; then
        echo "$(basename "$bin") imports$stray, which neither the virtual CoreFoundation exports nor the bridge implements, so a failure would be about those imports and not about CoreFoundation"
    fi
}

cf_status() {
    local tag="$1" file="$2" line
    line="$(grep -E "^$tag (ok|bad:)" "$file" 2>/dev/null | head -1)"
    if [ -z "$line" ]; then
        line="$(tail -1 "$file" 2>/dev/null)"
    fi
    echo "$line"
}

cf_bits() {
    local tag="$1" file="$2" group line out=""
    shift 2
    while [ $# -ge 2 ]; do
        group="$1"
        line="$(grep -E "^$tag $group bad:" "$file" 2>/dev/null | head -1)"
        if [ -n "$line" ]; then
            out="$out${out:+; }'$line', where $2"
        fi
        shift 2
    done
    if [ -z "$out" ]; then
        out="no group line reports a failure although the last line does, so the bookkeeping in the fixture is wrong"
    fi
    echo "$out"
}

cf_stall() {
    local tag="$1" err="$2" hang="${3:-}" last
    last="$(grep -h "^$tag: " "$err" 2>/dev/null | tail -1 | sed "s/^$tag: //")"
    case $last in
        "")
            echo "the fixture never wrote its first progress note" ;;
        "calling CFRunLoopRunInMode"|"timer fired"|"source performed"|stopping)
            if [ -n "$hang" ]; then
                echo "the last progress note was '$last', so CFRunLoopRunInMode never returned although the fixture bounds it at ${CF_RUN_SECS}s: the main thread is stuck in native code or in a callout that never came back"
            else
                echo "the last progress note was '$last', so it stopped inside CFRunLoopRunInMode"
            fi ;;
        "returned from CFRunLoopRunInMode")
            echo "the last progress note was '$last', so the run loop returned and the fixture stopped while removing, invalidating or releasing what it had added" ;;
        *)
            echo "the last progress note was '$last', so the fixture stopped inside that group of checks" ;;
    esac
}

cf_run_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" reason
    reason="$(native_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -hF "$NOBIND" "$out" "$err" 2>/dev/null | grep -Fq " in $CF_FRAMEWORK"; then
        echo "$reason: the virtual CoreFoundation does not export that name, so the program never ran"
    elif grep -Fq "$NOBIND$STACK_GUARD_SYM " "$out" "$err" 2>/dev/null; then
        echo "$reason$(thread_guard_hint "$out" "$err")"
    elif grep -qE "$CF_STRUCT_RE" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hE "$CF_STRUCT_RE" "$out" "$err" | head -1 | cut -c1-200): the bridge refused a structure of function pointers rather than hand native code a word it could not convert"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-120)$([ "$tag" != cf_basic ] && echo ", which is also what native code jumping to a guest function pointer left unconverted in a callbacks structure or a context looks like"); $(cf_stall "$tag" "$err")"
    elif grep -Fq "$GUEST_CRASH" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hF "$GUEST_CRASH" "$out" "$err" | head -1 | cut -c1-120); $(cf_stall "$tag" "$err")"
    elif grep -q "^ocerz: abi: " "$out" "$err" 2>/dev/null; then
        echo "$reason; a callback crossing was refused: $(grep -h "^ocerz: abi: " "$out" "$err" | head -1 | cut -c1-240)"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason: still running after ${NATIVE_TIMEOUT}s, and $(cf_stall "$tag" "$err" hang)"
    elif [ "$rc" -gt 128 ] && [ "$rc" -lt 160 ]; then
        echo "$reason: the process was ended by signal $((rc - 128)); $(cf_stall "$tag" "$err")"
    else
        echo "$reason"
    fi
}

cf_arm64_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" line
    shift 4
    line="$(cf_status "$tag" "$out")"
    if grep -q "^$tag bad:" "$out" 2>/dev/null; then
        echo "the arm64 build, run directly against the host's own CoreFoundation, fails its own checks, so the fixture expects something CoreFoundation does not do and the native run can prove nothing either way: $(cf_bits "$tag" "$out" "$@")"
    elif [ "$rc" -eq 124 ]; then
        echo "the arm64 build, run directly on the host, was still running after ${NATIVE_TIMEOUT}s, so the fixture hangs against the real CoreFoundation, and $(cf_stall "$tag" "$err" hang)"
    elif [ "$rc" -ne 0 ] || ! grep -q "^$tag ok" "$out" 2>/dev/null; then
        echo "the arm64 build, run directly on the host, exited $rc with '${line:-nothing}' and no '$tag ok' line, so the fixture itself is broken, and $(cf_stall "$tag" "$err")"
    fi
}

case_cf() {
    local name="$1" bin="$2" arm="$3" tag="$4" need="$5" note="$6"
    local reason="" rc_arm rc_jit rc_nojit rc_cache line cache_note=""
    local ao="$TMP/$name.arm64.out" ae="$TMP/$name.arm64.err"
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$CF_TIMEOUT
    shift 6

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    if [ -z "$arm" ]; then
        record "$name" "the x86_64 fixture compiled and its arm64 build did not, which leaves the case without its host oracle: $( (grep -m1 -i 'error' "$TMP/$name.arm64.cc.log" || head -1 "$TMP/$name.arm64.cc.log") 2>/dev/null | cut -c1-160)"
        return
    fi
    reason="$(cf_import_reason "$bin" "$need")"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi
    run_bounded "$ao" "$ae" "$arm"
    rc_arm=$?
    reason="$(cf_arm64_reason "$rc_arm" "$tag" "$ao" "$ae" "$@")"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi

    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(cf_status "$tag" "$jo")"

    reason="$(cf_run_reason "$rc_jit" "$tag" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed: $(cf_bits "$tag" "$jo" "$@")"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi

    if [ -z "$reason" ]; then
        reason="$(cf_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(cf_status "$tag" "$no")': the guest's own checks failed: $(cf_bits "$tag" "$no" "$@")"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    if [ -z "$reason" ] && ! cmp -s "$jo" "$ao"; then
        reason="native '$(tr '\n' ' ' < "$jo")' != arm64 '$(tr '\n' ' ' < "$ao")': the guest got answers from the host's own CoreFoundation that a native program calling it directly does not"
    fi
    if [ -n "$reason" ] && [ -n "$note" ]; then
        reason="$reason. $note"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin"
        rc_cache=$?
        if [ "$rc_cache" -eq 124 ]; then
            reason="cache mode still running after ${NATIVE_TIMEOUT}s, which crosses no bridge, and $(cf_stall "$tag" "$ce" hang)"
        elif [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(cf_status "$tag" "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': native mode agrees with the arm64 build and the x86 CoreFoundation in the shared cache answers otherwise, so either the translator ran that framework wrong or the fixture prints something the two builds of CoreFoundation really disagree about"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

objc_import_reason() {
    local bin="$1" kind="$2" imports allowed sym stray="" missing="" why
    local libs="libobjc, Foundation and CoreFoundation" about="Objective-C or formatting"
    shift 2
    imports="$(nm -u "$bin" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    if [ -z "$imports" ]; then
        return
    fi
    allowed=" $CF_BRIDGED $CF_EXPORTS $OBJC_BRIDGED $OBJC_EXPORTS $STACK_GUARD_SYM "
    case $kind in
        classes)
            allowed="$allowed$OBJC_CLASS_EXPORTS "
            about="classes of the guest's" ;;
        view)
            allowed="$allowed$OBJC_CLASS_EXPORTS $APPKIT_EXPORTS "
            libs="libobjc, AppKit, CoreGraphics, Foundation and CoreFoundation"
            about="classes of the guest's or views" ;;
        app)
            allowed="$allowed$OBJC_CLASS_EXPORTS $APPKIT_EXPORTS $APP_EXPORTS "
            libs="libobjc, AppKit, CoreGraphics, Foundation and CoreFoundation"
            about="the application's identity" ;;
        blocks)
            allowed="$allowed$OBJC_CLASS_EXPORTS $BLOCK_EXPORTS "
            libs="libSystem, libobjc, Foundation and CoreFoundation"
            about="blocks" ;;
        dl)
            allowed="$allowed$OBJC_CLASS_EXPORTS $DL_EXPORTS $DL_BRIDGED "
            libs="libSystem, libobjc, Foundation and CoreFoundation"
            about="loading code at run time" ;;
    esac
    for sym in $imports; do
        case "$allowed" in
            *" $sym "*) ;;
            *) stray="$stray $sym" ;;
        esac
    done
    for sym in "$@"; do
        case " $imports" in
            *" $sym "*) ;;
            *) missing="$missing $sym" ;;
        esac
    done
    if [ -n "$missing" ]; then
        set -- $missing
        case $1 in
            _objc_msgSend) why="no message was sent through objc_msgSend at all" ;;
            _objc_autoreleasePoolPush) why="@autoreleasepool was not compiled into calls to the pool" ;;
            "$CF_CLASS_SYM") why="its @\"...\" literals were not compiled as constant strings whose isa binds to that data export" ;;
            _OBJC_CLASS_*_NSConstant*) why="its @42 and @3.5 literals were not compiled as constant objects whose isa binds to Foundation's constant number classes" ;;
            _CFStringCreateWithFormat|_CFStringAppendFormat) why="the CoreFoundation format functions were never called" ;;
            _NSLog) why="NSLog was never called" ;;
            ___sprintf_chk|___snprintf_chk) why="the fortified translation unit did not call the _chk entry points" ;;
            _objc_msgSendSuper2) why="no message was sent to super" ;;
            _OBJC_METACLASS_*_NSObject|__objc_empty_cache) why="the classes it defines were not laid down with a metaclass and a cache that bind to the native runtime's" ;;
            _objc_storeWeak|_objc_loadWeakRetained) why="no weak reference was stored or read" ;;
            _objc_getProperty|_objc_setProperty_atomic) why="the atomic property's accessors were not synthesized as calls handed the ivar's offset" ;;
            _OBJC_CLASS_*_NSView|_OBJC_METACLASS_*_NSView) why="the view class was not compiled as a subclass of the host's NSView" ;;
            _NSRectFill|_CGContextFillRect) why="-drawRect: did not fill through AppKit and CoreGraphics" ;;
            _objc_msgSend_stret) why="no NSRect came back from a message as a returned structure" ;;
            _class_getInstanceSize) why="the fixture never asked the runtime for an instance size" ;;
            _NSApplicationMain) why="the application was not started through NSApplicationMain" ;;
            _CFBundleGetMainBundle|_OBJC_CLASS_*_NSBundle) why="the main bundle was never asked for through CoreFoundation and Foundation both" ;;
            __NSGet*) why="the crt_externs functions were never called" ;;
            __Block_*|__NSConcrete*Block|_objc_retainBlock) why="no block was copied, captured or laid down on the stack through the block runtime" ;;
            _dispatch_*|_qsort_b|_bsearch_b) why="the block entry points it names were never called" ;;
            _OBJC_CLASS_*_NSItemProvider|_OBJC_CLASS_*_NSBlockOperation) why="the Foundation classes that call blocks were never used" ;;
            _dl*|__dyld_*) why="the dynamic-loading API was not called through the virtual libSystem's exports" ;;
            *) why="the unfortified translation unit did not call the plain entry points" ;;
        esac
        echo "$(basename "$bin") imports '${imports% }' and not$missing, so $why, and the case proves nothing about it"
        return
    fi
    if [ -n "$stray" ]; then
        echo "$(basename "$bin") imports$stray, which neither the virtual $libs export nor the bridge implements, so a failure would be about those imports and not about $about"
    fi
}

objc_messages() {
    local kind="$1" tag="$2" file="$3"
    case $kind in
        nslog) sed -nE "s/$NSLOG_PREFIX_RE//p" "$file" 2>/dev/null ;;
        prefix) grep "^$tag stderr " "$file" 2>/dev/null ;;
    esac
}

objc_stall() {
    local tag="$1" err="$2" hang="${3:-}" last
    last="$(grep -h "^$tag: " "$err" 2>/dev/null | tail -1 | sed "s/^$tag: //")"
    case $last in
        "")
            echo "the fixture never wrote its first progress note" ;;
        "calling NSLog")
            if [ -n "$hang" ]; then
                echo "the last progress note was '$last', so one of the four NSLog calls never returned"
            else
                echo "the last progress note was '$last', so it stopped inside one of the four NSLog calls"
            fi ;;
        "returned from NSLog")
            echo "the last progress note was '$last', so every NSLog call returned and the fixture stopped while checking what they left behind" ;;
        "calling NSApplicationMain")
            echo "the last progress note was '$last', so NSApplicationMain never delivered applicationDidFinishLaunching: to the guest's delegate" ;;
        terminating)
            echo "the last progress note was '$last', so every check ran and the process did not end through -terminate: and exit" ;;
        *)
            echo "the last progress note was '$last', so the fixture stopped inside that group of calls" ;;
    esac
}

objc_run_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" reason lib
    reason="$(native_run_reason "$rc" "$out" "$err")"
    lib="$(grep -hF "$NOBIND" "$out" "$err" 2>/dev/null | head -1 | sed 's/.* in //')"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -hF "$NOBIND" "$out" "$err" 2>/dev/null | grep -Fq -e " in $OBJC_LIB" -e " in $FOUNDATION_FRAMEWORK" -e " in $CF_FRAMEWORK" \
            -e " in $APPKIT_FRAMEWORK" -e " in $CG_FRAMEWORK"; then
        if grep -Fq "vdylib: built $lib " "$out" "$err" 2>/dev/null; then
            echo "$reason: the virtual library named there does not export that name, so the program never ran"
        else
            echo "$reason: native mode synthesized no $lib at all, which is what an API database without a file for it looks like, so the program never ran"
        fi
    elif grep -Fq "$NOBIND$STACK_GUARD_SYM " "$out" "$err" 2>/dev/null; then
        echo "$reason$(thread_guard_hint "$out" "$err")"
    elif grep -qE "$OBJC_HANDLER_RE" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hE "$OBJC_HANDLER_RE" "$out" "$err" | head -1 | cut -c1-160): the database makes that export a special record and this build of ocerz has no handler of that name, so the two are out of step; $(objc_stall "$tag" "$err")"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-120); $(objc_stall "$tag" "$err")"
    elif grep -Fq "$GUEST_CRASH" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hF "$GUEST_CRASH" "$out" "$err" | head -1 | cut -c1-120); $(objc_stall "$tag" "$err")"
    elif grep -q "^ocerz: abi: " "$out" "$err" 2>/dev/null; then
        echo "$reason; a crossing was refused: $(grep -h "^ocerz: abi: " "$out" "$err" | head -1 | cut -c1-240)"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason: still running after ${NATIVE_TIMEOUT}s, and $(objc_stall "$tag" "$err" hang)"
    elif [ "$rc" -gt 128 ] && [ "$rc" -lt 160 ]; then
        echo "$reason: the process was ended by signal $((rc - 128)); $(objc_stall "$tag" "$err")"
    else
        echo "$reason; $(objc_stall "$tag" "$err")"
    fi
}

objc_arm64_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" kind="$5" line want got
    shift 5
    line="$(cf_status "$tag" "$out")"
    if grep -q "^$tag bad:" "$out" 2>/dev/null; then
        echo "the arm64 build, run directly against the host's own frameworks, fails its own checks, so the fixture expects something they do not do and the native run can prove nothing either way: $(cf_bits "$tag" "$out" "$@")"
    elif [ "$rc" -eq 124 ]; then
        echo "the arm64 build, run directly on the host, was still running after ${NATIVE_TIMEOUT}s, so the fixture hangs against the real frameworks, and $(objc_stall "$tag" "$err" hang)"
    elif [ "$rc" -ne 0 ] || ! grep -q "^$tag ok" "$out" 2>/dev/null; then
        echo "the arm64 build, run directly on the host, exited $rc with '${line:-nothing}' and no '$tag ok' line, so the fixture itself is broken, and $(objc_stall "$tag" "$err")"
    elif [ "$kind" = nslog ] || [ "$kind" = prefix ]; then
        want="$(grep -o ' lines=[0-9]*' "$out" 2>/dev/null | head -1 | cut -d= -f2)"
        got="$(objc_messages "$kind" "$tag" "$err" | wc -l | tr -d ' ')"
        if [ -z "$want" ] || [ "$got" != "$want" ]; then
            echo "the arm64 build, run directly on the host, left $got message lines on stderr where its status line says ${want:-nothing}, so the host is not writing them where the case looks, and every stderr comparison would pass on nothing"
        fi
    fi
}

objc_view_unavailable() {
    local tag="$1" rc="$2" out="$3" err="$4" line
    line="$(grep -m1 "^$tag unavailable " "$out" 2>/dev/null)"
    if [ -n "$line" ]; then
        echo "the arm64 build, run directly on the host, could not draw offscreen here: ${line#"$tag unavailable "}"
    elif [ "$rc" -ne 0 ] && ! grep -q "^$tag ok" "$out" 2>/dev/null && grep -qE "$VIEW_NO_SERVER_RE" "$err" 2>/dev/null; then
        echo "the arm64 build, run directly on the host, exited $rc naming the window server: $(grep -hE "$VIEW_NO_SERVER_RE" "$err" | head -1 | cut -c1-160)"
    fi
}

objc_view_repeat_reason() {
    local arm="$1" first="$2" run=2 rc
    while [ "$run" -le "$VIEW_RUNS" ]; do
        run_bounded "$first.$run" "$first.$run.err" "$arm"
        rc=$?
        if [ "$rc" -ne 0 ] || ! cmp -s "$first" "$first.$run"; then
            echo "the arm64 build, run directly on the host, wrote '$(tr '\n' ' ' < "$first")' on its first run and '$(tr '\n' ' ' < "$first.$run")' with exit $rc on run $run of $VIEW_RUNS, so what it draws or counts is not deterministic on this host and no comparison with it can mean anything"
            return
        fi
        run=$((run + 1))
    done
}

case_objc() {
    local name="$1" bin="$2" arm="$3" tag="$4" need="$5" kind="$6" note="$7"
    local reason="" rc_arm rc_jit rc_nojit rc_cache line cache_note=""
    local ao="$TMP/$name.arm64.out" ae="$TMP/$name.arm64.err"
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local what="stderr lines"
    local NATIVE_TIMEOUT=$OBJC_TIMEOUT
    shift 7

    if [ "$kind" = nslog ]; then
        what="NSLog messages with the prefix removed"
    fi
    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    if [ -z "$arm" ]; then
        record "$name" "the x86_64 fixture compiled and its arm64 build did not, which leaves the case without its host oracle: $( (grep -m1 -i 'error' "$TMP/$name.arm64.cc.log" || head -1 "$TMP/$name.arm64.cc.log") 2>/dev/null | cut -c1-160)"
        return
    fi
    reason="$(objc_import_reason "$bin" "$kind" $need)"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi
    run_bounded "$ao" "$ae" "$arm"
    rc_arm=$?
    if [ "$kind" = view ]; then
        reason="$(objc_view_unavailable "$tag" "$rc_arm" "$ao" "$ae")"
        if [ -n "$reason" ]; then
            echo "SKIP $name ($reason)"
            return
        fi
    fi
    reason="$(objc_arm64_reason "$rc_arm" "$tag" "$ao" "$ae" "$kind" "$@")"
    if [ -z "$reason" ] && [ "$kind" = view ]; then
        reason="$(objc_view_repeat_reason "$arm" "$ao")"
    fi
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi
    objc_messages "$kind" "$tag" "$ae" > "$ae.msg"

    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(cf_status "$tag" "$jo")"
    objc_messages "$kind" "$tag" "$je" > "$je.msg"
    objc_messages "$kind" "$tag" "$ne" > "$ne.msg"

    reason="$(objc_run_reason "$rc_jit" "$tag" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed: $(cf_bits "$tag" "$jo" "$@")"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi

    if [ -z "$reason" ]; then
        reason="$(objc_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(cf_status "$tag" "$no")': the guest's own checks failed: $(cf_bits "$tag" "$no" "$@")"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        elif ! cmp -s "$je.msg" "$ne.msg"; then
            reason="native jit '$(tr '\n' '|' < "$je.msg")' and no-jit '$(tr '\n' '|' < "$ne.msg")' $what differ"
        fi
    fi
    if [ -z "$reason" ] && ! cmp -s "$jo" "$ao"; then
        reason="native '$(tr '\n' ' ' < "$jo")' != arm64 '$(tr '\n' ' ' < "$ao")': the guest got answers from the host's own frameworks that a native program calling them directly does not"
    elif [ -z "$reason" ] && ! cmp -s "$je.msg" "$ae.msg"; then
        reason="native '$(tr '\n' '|' < "$je.msg")' != arm64 '$(tr '\n' '|' < "$ae.msg")' in the $what: the host's frameworks were handed arguments a native program calling them directly does not hand them"
    fi
    if [ -n "$reason" ] && [ -n "$note" ]; then
        reason="$reason. $note"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin"
        rc_cache=$?
        objc_messages "$kind" "$tag" "$ce" > "$ce.msg"
        if [ "$rc_cache" -eq 124 ]; then
            reason="cache mode still running after ${NATIVE_TIMEOUT}s, which crosses no bridge, and $(objc_stall "$tag" "$ce" hang)"
        elif [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(cf_status "$tag" "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': native mode agrees with the arm64 build and the x86 frameworks in the shared cache answer otherwise, so either the translator ran them wrong or the fixture prints something the two builds really disagree about"
        elif ! cmp -s "$je.msg" "$ce.msg"; then
            reason="native '$(tr '\n' '|' < "$je.msg")' != cache '$(tr '\n' '|' < "$ce.msg")' in the $what: native mode agrees with the arm64 build and the x86 frameworks in the shared cache write otherwise"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

app_nslog_name() {
    grep -E "$NSLOG_PREFIX_RE" "$1" 2>/dev/null | head -1 | sed -E 's/^[0-9-]+ [0-9:.]+ (.*)\[[0-9]+:[0-9a-fx]+\] .*/\1/'
}

app_unavailable() {
    local rc="$1" out="$2" err="$3" line
    line="$(grep -m1 "^app_bundle unavailable " "$out" 2>/dev/null)"
    if [ -n "$line" ]; then
        echo "the arm64 build, run directly on the host, has no application to run here: ${line#app_bundle unavailable }"
    elif [ "$rc" -ne 0 ] && ! grep -q "^app_bundle ok" "$out" 2>/dev/null && grep -qE "$VIEW_NO_SERVER_RE" "$err" 2>/dev/null; then
        echo "the arm64 build, run directly on the host, exited $rc naming the window server: $(grep -hE "$VIEW_NO_SERVER_RE" "$err" | head -1 | cut -c1-160)"
    fi
}

case_app_bundle() {
    local name=app_bundle tag=app_bundle reason="" rc_arm rc_jit rc_nojit rc_cache line cache_note="" got f
    local ao="$TMP/$name.arm64.out" ae="$TMP/$name.arm64.err"
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$OBJC_TIMEOUT
    local what="NSLog messages with the prefix removed"
    local bits=(
        bundle "bit 0 is NSBundle's main bundle nil or its bundleIdentifier not the Info.plist's, 1 objectForInfoDictionaryKey: not reading the test's own key, 2 bundlePath not the .app holding the executable, 3 pathForResource:ofType: not finding greeting.txt under the bundle's Resources or its text wrong, 4 CFBundleGetMainBundle not the same bundle by identifier and key, 5 principalClass not NSApplication"
        crt "bit 0 is _NSGetExecutablePath failing with a buffer that fits or changing the size it was handed, 1 the path not NSBundle's executablePath, 2 a buffer too small not returning -1 with the size the path needs, 3 _NSGetArgc or _NSGetArgv not the argc and the very argv main was handed, 4 *_NSGetEnviron() not the environ variable or a setenv not seen through it exactly once, 5 *_NSGetProgname() not the pointer getprogname returns, 6 _NSGetMachExecuteHeader not the guest's own _mh_execute_header"
        process "bit 0 is NSProcessInfo's processName not the executable's name, 1 getprogname not the last component of argv[0], 2 NSProcessInfo's arguments not argc long, ending in the extra argument and starting with the executable, 3 CFProcessPath present in the guest's environment, 4 NSRunningApplication's currentApplication not registered under the bundle identifier or not this process"
        app "bit 0 is applicationWillFinishLaunching: and applicationDidFinishLaunching: not delivered once each and in order with NSApp as the notification's object, 1 NSApp not the shared NSApplication main made, 2 the delegate not the guest's, 3 NSApp not running, 4 the activation policy not the accessory one LSUIElement asks for"
    )

    if callback_fixture_missing "$name" "$APP_BUNDLE_BIN"; then
        return
    fi
    if [ -z "$APP_BUNDLE_ARM64" ]; then
        record "$name" "the x86_64 fixture compiled and its arm64 build did not, which leaves the case without its host oracle: $( (grep -m1 -i 'error' "$TMP/$name.arm64.cc.log" || head -1 "$TMP/$name.arm64.cc.log") 2>/dev/null | cut -c1-160)"
        return
    fi
    if [ -z "$APP_BUNDLE_EXE" ]; then
        record "$name" "lipo would not join the two builds into the bundle's one executable: $(head -1 "$TMP/$name.lipo.log" 2>/dev/null | cut -c1-160)"
        return
    fi
    reason="$(objc_import_reason "$APP_BUNDLE_BIN" app _NSApplicationMain _CFBundleGetMainBundle "_OBJC_CLASS_\$_NSBundle" \
        __NSGetExecutablePath __NSGetArgv __NSGetEnviron __NSGetMachExecuteHeader)"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi

    run_bounded "$ao" "$ae" "$APP_BUNDLE_EXE" "$APP_ARG"
    rc_arm=$?
    reason="$(app_unavailable "$rc_arm" "$ao" "$ae")"
    if [ -n "$reason" ]; then
        echo "SKIP $name ($reason)"
        return
    fi
    reason="$(objc_arm64_reason "$rc_arm" "$tag" "$ao" "$ae" "" "${bits[@]}")"
    got="$(app_nslog_name "$ae")"
    if [ -z "$reason" ] && [ "$got" != "$APP_NAME" ]; then
        reason="the arm64 build, run directly on the host, logged under '${got:-nothing}' rather than '$APP_NAME', so the host does not name an application after its executable the way this case expects, and the process-name check would pass or fail on nothing"
    fi
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi
    objc_messages nslog "$tag" "$ae" > "$ae.msg"

    run_bounded "$jo" "$je" "$OCERZ" -v -native "$APP_BUNDLE_EXE" "$APP_ARG"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$APP_BUNDLE_EXE" "$APP_ARG"
    rc_nojit=$?
    line="$(cf_status "$tag" "$jo")"
    objc_messages nslog "$tag" "$je" > "$je.msg"
    objc_messages nslog "$tag" "$ne" > "$ne.msg"

    reason="$(objc_run_reason "$rc_jit" "$tag" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed: $(cf_bits "$tag" "$jo" "${bits[@]}")"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi
    if [ -z "$reason" ]; then
        reason="$(objc_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(cf_status "$tag" "$no")': the guest's own checks failed: $(cf_bits "$tag" "$no" "${bits[@]}")"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        elif ! cmp -s "$je.msg" "$ne.msg"; then
            reason="native jit '$(tr '\n' '|' < "$je.msg")' and no-jit '$(tr '\n' '|' < "$ne.msg")' $what differ"
        fi
    fi
    for f in "$je" "$ne"; do
        got="$(app_nslog_name "$f")"
        if [ -z "$reason" ] && [ "$got" != "$APP_NAME" ]; then
            reason="NSLog in native mode named the process '${got:-nothing}' where the arm64 build's named it '$APP_NAME', so the host's frameworks took the name of the process ocerz is and not of the application it runs"
        fi
    done
    if [ -z "$reason" ] && ! cmp -s "$jo" "$ao"; then
        reason="native '$(tr '\n' ' ' < "$jo")' != arm64 '$(tr '\n' ' ' < "$ao")': the guest got answers from the host's own frameworks that a native application asking them directly does not"
    elif [ -z "$reason" ] && ! cmp -s "$je.msg" "$ae.msg"; then
        reason="native '$(tr '\n' '|' < "$je.msg")' != arm64 '$(tr '\n' '|' < "$ae.msg")' in the $what"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$APP_BUNDLE_EXE" "$APP_ARG"
        rc_cache=$?
        objc_messages nslog "$tag" "$ce" > "$ce.msg"
        got="$(app_nslog_name "$ce")"
        if [ "$rc_cache" -eq 124 ]; then
            reason="cache mode still running after ${NATIVE_TIMEOUT}s, which crosses no bridge, and $(objc_stall "$tag" "$ce" hang)"
        elif [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(cf_status "$tag" "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': native mode agrees with the arm64 build and the x86 frameworks in the shared cache answer otherwise"
        elif ! cmp -s "$je.msg" "$ce.msg" || [ "$got" != "$APP_NAME" ]; then
            reason="native '$(tr '\n' '|' < "$je.msg")' != cache '$(tr '\n' '|' < "$ce.msg")' logged under '${got:-nothing}': native mode agrees with the arm64 build and cache mode logs otherwise"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

dl_cache_lines() {
    grep -E "^dl_basic ($DL_CACHE_GROUPS) " "$1" 2>/dev/null
}

case_dl_basic() {
    local name=dl_basic tag=dl_basic bin="$DL_BASIC_BIN" arm="$DL_BASIC_ARM64"
    local reason="" rc_arm rc_jit rc_nojit rc_cache line cache_note=""
    local ao="$TMP/$name.arm64.out" ae="$TMP/$name.arm64.err"
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$DL_TIMEOUT

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    if [ -z "$arm" ]; then
        record "$name" "the x86_64 fixture compiled and its arm64 build did not, which leaves the case without its host oracle: $( (grep -m1 -i 'error' "$TMP/$name.arm64.cc.log" || head -1 "$TMP/$name.arm64.cc.log") 2>/dev/null | cut -c1-160)"
        return
    fi
    reason="$(objc_import_reason "$bin" dl $DL_NEED)"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi
    run_bounded "$ao" "$ae" "$arm"
    rc_arm=$?
    reason="$(objc_arm64_reason "$rc_arm" "$tag" "$ao" "$ae" dl "$@")"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi

    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(cf_status "$tag" "$jo")"

    reason="$(objc_run_reason "$rc_jit" "$tag" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed: $(cf_bits "$tag" "$jo" "$@")"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi
    if [ -z "$reason" ]; then
        reason="$(objc_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(cf_status "$tag" "$no")': the guest's own checks failed: $(cf_bits "$tag" "$no" "$@")"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    if [ -z "$reason" ] && ! cmp -s "$jo" "$ao"; then
        reason="native '$(tr '\n' ' ' < "$jo")' != arm64 '$(tr '\n' ' ' < "$ao")': the guest's loads, lookups and image list answered otherwise than the host's own dyld answers the same program"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin"
        rc_cache=$?
        if [ "$rc_cache" -eq 124 ]; then
            reason="cache mode still running after ${NATIVE_TIMEOUT}s, and $(objc_stall "$tag" "$ce" hang)"
        elif [ "$(dl_cache_lines "$jo")" != "$(dl_cache_lines "$co")" ]; then
            reason="native '$(dl_cache_lines "$jo" | tr '\n' ' ')' != cache '$(dl_cache_lines "$co" | tr '\n' ' ')' on the groups cache mode answers the way dyld does, exit $rc_cache there"
        fi
        cache_note=" cache=$(dl_cache_lines "$co" | wc -l | tr -d ' ') groups"
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

dl_refusal_reason() {
    local out="$1" label want
    shift
    while [ $# -ge 2 ]; do
        label="$1"
        want="$2"
        if ! grep "^dl_refusals text $label " "$out" 2>/dev/null | grep -Fq -- "$want"; then
            echo "the $label refusal read '$(grep -m1 "^dl_refusals text $label " "$out" 2>/dev/null | cut -d' ' -f4- | cut -c1-240)', which does not say '$want'"
            return
        fi
        shift 2
    done
}

case_dl_refusals() {
    local name=dl_refusals tag=dl_refusals bin="$DL_REFUSALS_BIN" bits="$1"
    local reason="" rc_jit rc_nojit line imports sym stray="" dir
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local NATIVE_TIMEOUT=$DL_TIMEOUT

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    dir="$(cd "$(dirname "$bin")" && pwd -P)"
    imports="$(nm -u "$bin" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    for sym in $imports; do
        case " $DL_EXPORTS $DL_BRIDGED $STACK_GUARD_SYM " in
            *" $sym "*) ;;
            *) stray="$stray $sym" ;;
        esac
    done
    if [ -n "$stray" ]; then
        record "$name" "$(basename "$bin") imports$stray, which neither the virtual libSystem exports nor the bridge implements, so a failure would be about those imports and not about loading code"
        return
    fi
    run_bounded "$jo" "$je" "$OCERZ" -v -native "$bin"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin"
    rc_nojit=$?
    line="$(cf_status "$tag" "$jo")"
    reason="$(native_run_reason "$rc_jit" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed, where $bits"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    elif [ -z "$reason" ]; then
        reason="$(dl_refusal_reason "$jo" \
            arm64 "$DL_REFUSED" arm64 "has no x86_64 slice" \
            hostlib "$DL_REFUSED" hostlib "'/usr/lib/libz.1.dylib' is in the host's shared cache" \
            bare "$DL_REFUSED" bare "'/usr/lib/libz.1.dylib'" \
            missingdep "Library not loaded: @rpath/libdlgone.dylib" missingdep "Referenced from: $dir/libdlneedsgone.dylib" \
            missingsym "Symbol not found: _dl_vanishing" missingsym "Referenced from: $dir/libdlneedsym.dylib" \
            missingsym "Expected in: @rpath/libdlweak.dylib" \
            preflight "$DL_REFUSED")"
    fi
    if [ -z "$reason" ]; then
        reason="$(native_run_reason "$rc_nojit" "$no" "$ne")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit and no-jit stdout differ: '$(tr '\n' ' ' < "$jo" | cut -c1-200)' against '$(tr '\n' ' ' < "$no" | cut -c1-200)'"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'"
}

case_env_native() {
    local name=env_native rc reason="" out="$TMP/env_native.out" err="$TMP/env_native.err"
    run_bounded "$out" "$err" env OCERZ_MODE=native "$OCERZ" -v "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    reason="$(native_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ] && ! cmp -s "$out" "$NOUT"; then
        reason="OCERZ_MODE=native stdout differs from the -native run"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_flag_beats_env() {
    local name=flag_beats_env rc reason="" out="$TMP/flag_beats_env.out" err="$TMP/flag_beats_env.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" env OCERZ_MODE=native "$OCERZ" -v -cache "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="-cache did not map the shared cache"
    elif [ -s "$CACHE_OUT" ] && ! cmp -s "$out" "$CACHE_OUT"; then
        reason="stdout differs from the plain cache-mode run"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_last_flag_native() {
    local name=last_flag_native rc reason="" out="$TMP/last_flag_native.out" err="$TMP/last_flag_native.err"
    run_bounded "$out" "$err" "$OCERZ" -v -cache -native "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    reason="$(native_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ] && ! cmp -s "$out" "$NOUT"; then
        reason="'-cache -native' stdout differs from the -native run"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_last_flag_cache() {
    local name=last_flag_cache rc reason="" out="$TMP/last_flag_cache.out" err="$TMP/last_flag_cache.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native -cache "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="'-native -cache' exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="'-native -cache' did not map the shared cache"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_bad_mode() {
    local name=bad_mode rc reason="" out="$TMP/bad_mode.out" err="$TMP/bad_mode.err"
    run_bounded "$out" "$err" env OCERZ_MODE=hybrid "$OCERZ" "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 64 ]; then
        reason="exit $rc, want 64"
    elif ! grep -q 'ocerz:' "$out" "$err"; then
        reason="refused without a named message"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_empty_mode() {
    local name=empty_mode rc reason="" out="$TMP/empty_mode.out" err="$TMP/empty_mode.err"
    if [ "$CACHE_OK" -ne 1 ]; then
        echo "SKIP $name (shared cache not mappable here)"; return
    fi
    run_bounded "$out" "$err" env OCERZ_MODE= "$OCERZ" -v "$DYN" "$KERNEL" "$SCALE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0"
    elif ! cache_line_seen "$out" "$err"; then
        reason="an empty OCERZ_MODE did not fall back to cache mode"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_float() {
    local name=native_float rc reason="" src="$TMP/fp.c" bin="$TMP/fp"
    local out="$TMP/native_float.out" err="$TMP/native_float.err"
    local cout="$TMP/cache_float.out"
    cat > "$src" <<'EOC'
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
    double a = atof("3.5");
    double b = atof("-0.25");
    char *end = 0;
    double c = strtod("2.5e3xyz", &end);
    int ok = (a == 3.5) && (b == -0.25) && (c == 2500.0) && end && strcmp(end, "xyz") == 0;
    write(1, ok ? "fp ok\n" : "fp bad\n", ok ? 6 : 7);
    return ok ? 0 : 1;
}
EOC
    if ! clang -arch x86_64 -O1 -fno-stack-protector -o "$bin" "$src" >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -native "$bin"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0; a double crossed the bridge wrongly"
    elif ! grep -q 'fp ok' "$out"; then
        reason="the guest computed the wrong value from a bridged double"
    elif [ "$CACHE_OK" -eq 1 ]; then
        run_bounded "$cout" "$TMP/cache_float.err" "$OCERZ" "$bin"
        cmp -s "$out" "$cout" || reason="native and cache disagree on a bridged double"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_classic_bind() {
    local name=native_classic_bind rc reason="" src="$TMP/classic.c" bin="$TMP/classic"
    local out="$TMP/native_classic.out" err="$TMP/native_classic.err"
    local cout="$TMP/cache_classic.out"
    cat > "$src" <<'EOC'
#include <string.h>
#include <unistd.h>
static const char *pick(int k) { return k ? "classic" : "chained"; }
int main(int argc, char **argv)
{
    const char *s = pick(argc > 0);
    int ok = strlen(s) == 7 && strcmp(s, "classic") == 0;
    write(1, ok ? "classic ok\n" : "classic bad\n", ok ? 11 : 12);
    return ok ? 0 : 1;
}
EOC
    if ! clang -arch x86_64 -O1 -mmacosx-version-min=10.14 -o "$bin" "$src" >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    if ! otool -l "$bin" 2>/dev/null | grep -q 'LC_DYLD_INFO'; then
        record "$name" "the toolchain linked the fixture with chained fixups rather than classic binds, so it no longer tests dyld_stub_binder" ""
        return
    fi
    if ! nm -u "$bin" 2>/dev/null | grep -qx 'dyld_stub_binder'; then
        record "$name" "the fixture does not import dyld_stub_binder, so it no longer tests the export" ""
        return
    fi
    run_bounded "$out" "$err" "$OCERZ" -native "$bin"
    rc=$?
    if grep -Fq "$NOBIND" "$out" "$err"; then
        reason="exit $rc: $(grep -hF "$NOBIND" "$out" "$err" | head -1)"
    elif [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0"
    elif ! grep -q 'classic ok' "$out"; then
        reason="the classic-bind guest ran but computed the wrong answer"
    elif [ "$CACHE_OK" -eq 1 ]; then
        run_bounded "$cout" "$TMP/cache_classic.err" "$OCERZ" -cache "$bin"
        cmp -s "$out" "$cout" || reason="native and cache disagree on a classic-bind guest"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_exit() {
    local name=native_exit reason="" src="$TMP/exit_paths.c" bin="$TMP/exit_paths"
    cat > "$src" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static void first(void) { fputs(" first", stdout); }
static void second(void) { fputs(" second", stdout); }
int main(int argc, char **argv)
{
    atexit(first);
    atexit(second);
    fputs("buffered", stdout);
    if (argc > 1 && argv[1][0] == 'e')
        exit(3);
    if (argc > 1 && argv[1][0] == 'u')
        _exit(4);
    return 2;
}
EOC
    if ! clang -arch x86_64 -O1 -o "$bin" "$src" >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    local how rc crc detail=""
    for how in return e u; do
        local out="$TMP/native_exit.$how.out" cout="$TMP/cache_exit.$how.out"
        run_bounded "$out" "$TMP/native_exit.$how.err" "$OCERZ" -native "$bin" "$how"
        rc=$?
        detail="$detail $how=$rc"
        if [ -n "$reason" ]; then
            continue
        fi
        if [ "$CACHE_OK" -eq 1 ]; then
            run_bounded "$cout" "$TMP/cache_exit.$how.err" "$OCERZ" -cache "$bin" "$how"
            crc=$?
            if [ "$rc" -ne "$crc" ]; then
                reason="$how: native exit $rc, cache exit $crc"
            elif ! cmp -s "$out" "$cout"; then
                reason="$how: native wrote '$(cat "$out")', cache wrote '$(cat "$cout")'"
            fi
        fi
    done
    if [ -z "$reason" ]; then
        if ! grep -qx 'buffered second first' "$TMP/native_exit.return.out"; then
            reason="returning from main wrote '$(cat "$TMP/native_exit.return.out")', want the buffered text then both atexit handlers in reverse order"
        elif ! grep -qx 'buffered second first' "$TMP/native_exit.e.out"; then
            reason="exit wrote '$(cat "$TMP/native_exit.e.out")', want the buffered text then both atexit handlers in reverse order"
        elif [ -s "$TMP/native_exit.u.out" ]; then
            reason="_exit wrote '$(cat "$TMP/native_exit.u.out")', want nothing: it runs no handlers and flushes nothing"
        fi
    fi
    record "$name" "$reason" "exits:$detail"
}

case_native_constructors() {
    local name=native_constructors reason="" dir="$TMP/ctors"
    mkdir -p "$dir"
    cat > "$dir/libdep.c" <<'EOC'
#include <stdio.h>
int dep_value;
__attribute__((constructor)) static void dep_init(void) { dep_value = 7; fputs("dep ", stdout); }
EOC
    cat > "$dir/main.cc" <<'EOC'
#include <cstdio>
extern "C" int dep_value;
struct Global {
    int seen;
    Global() : seen(dep_value) { std::fputs("static ", stdout); }
};
static Global g;
__attribute__((constructor(200))) static void late(void) { std::fputs("late ", stdout); }
__attribute__((constructor(101))) static void early(void) { std::fputs("early ", stdout); }
int main()
{
    std::printf("main seen=%d dep=%d\n", g.seen, dep_value);
    return 0;
}
EOC
    if ! clang -arch x86_64 -O1 -dynamiclib -install_name @rpath/libdep.dylib -o "$dir/libdep.dylib" "$dir/libdep.c" >/dev/null 2>&1 ||
       ! clang++ -arch x86_64 -O1 -std=c++17 -fno-exceptions -Wl,-rpath,@loader_path -o "$dir/ctors" "$dir/main.cc" "$dir/libdep.dylib" >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    if nm -u "$dir/ctors" 2>/dev/null | grep -Eq '__ZNSt|__ZNKSt'; then
        record "$name" "the fixture imports libc++, which native mode does not synthesize" ""
        return
    fi
    local out="$TMP/native_ctors.out" cout="$TMP/cache_ctors.out" rc crc
    run_bounded "$out" "$TMP/native_ctors.err" "$OCERZ" -native "$dir/ctors"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        reason="exit $rc, want 0: $(head -c 200 "$TMP/native_ctors.err")"
    elif ! grep -Eqx 'dep (static early late|early late static|static late early|early static late) main seen=7 dep=7' "$out"; then
        reason="native wrote '$(cat "$out")': the dylib's constructor must run first and main must see what every constructor wrote"
    elif [ "$CACHE_OK" -eq 1 ]; then
        run_bounded "$cout" "$TMP/cache_ctors.err" "$OCERZ" -cache "$dir/ctors"
        crc=$?
        if [ "$crc" -ne "$rc" ] || ! cmp -s "$out" "$cout"; then
            reason="native wrote '$(cat "$out")' exit $rc, cache wrote '$(cat "$cout")' exit $crc"
        fi
    fi
    record "$name" "$reason" "exit=$rc out='$(cat "$out" 2>/dev/null)'"
}

case_native_unbound() {
    local name=native_unbound rc reason="" src="$TMP/unbound.c" bin="$TMP/unbound"
    local out="$TMP/native_unbound.out" err="$TMP/native_unbound.err"
    cat > "$src" <<'EOC'
#include <zlib.h>
int main(void) { return zlibVersion()[0] == 0; }
EOC
    if ! clang -arch x86_64 -fno-stack-protector -o "$bin" "$src" -lz >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$bin"
    rc=$?
    if [ "$rc" -ne 71 ]; then
        reason="exit $rc, want 71"
    elif ! grep -Fq "${NOBIND}_zlibVersion" "$out" "$err"; then
        reason="no 'no bridge for _zlibVersion' line"
    elif ! grep -Fq "$M0_SUMMARY" "$out" "$err"; then
        reason="no unresolved-import summary line"
    fi
    record "$name" "$reason" "exit=$rc"
}

case_native_static() {
    local name=native_static rc reason="" out="$TMP/native_static.out" err="$TMP/native_static.err"
    run_bounded "$out" "$err" "$OCERZ" -v -native "$STATIC"
    rc=$?
    if [ "$rc" -eq 71 ] || [ "$rc" -eq 72 ]; then
        reason="exit $rc: reached the loader instead of refusing a static image"
    elif [ "$rc" -ne 64 ]; then
        reason="exit $rc, want 64"
    elif ! grep -q 'ocerz:' "$out" "$err"; then
        reason="refused without a named message"
    fi
    record "$name" "$reason" "exit=$rc"
}

sys_api_file() {
    ls runtime/apis/macos/*/libSystem.B.dylib.api 2>/dev/null | tail -1
}

sys_import_reason() {
    local bin="$1" need="$2" imports sym stray="" missing="" api kind
    api="$(sys_api_file)"
    if [ -z "$api" ]; then
        echo "no libSystem database under runtime/apis to check the imports of $(basename "$bin") against"
        return
    fi
    imports=" $(nm -u "$bin" 2>/dev/null | awk '{print $1}' | tr '\n' ' ') "
    for sym in $need; do
        case "$imports" in
            *" $sym "*) ;;
            *) missing="$missing $sym" ;;
        esac
    done
    if [ -n "$missing" ]; then
        echo "$(basename "$bin") does not import$missing, so the case would not exercise what it is for"
        return
    fi
    for sym in $imports; do
        kind="$(awk -v s="$sym" '$2 == s { print $1; exit }' "$api")"
        if [ "$kind" = stub ]; then
            stray="$stray $sym"
        fi
    done
    if [ -n "$stray" ]; then
        echo "$(basename "$bin") imports$stray, which $(basename "$api") still stubs, so a failure would be about those imports and not about what the case is for"
    fi
}

sys_status() {
    local tag="$1" file="$2" line
    line="$(grep -E "^$tag (ok|bad:)" "$file" 2>/dev/null | head -1)"
    if [ -z "$line" ]; then
        line="$(tail -1 "$file" 2>/dev/null)"
    fi
    echo "$line"
}

sys_bits() {
    local tag="$1" file="$2" group line out=""
    shift 2
    while [ $# -ge 2 ]; do
        group="$1"
        line="$(grep -E "^$tag $group bad:" "$file" 2>/dev/null | head -1)"
        if [ -n "$line" ]; then
            out="$out${out:+; }'$line', where $2"
        fi
        shift 2
    done
    if [ -z "$out" ]; then
        out="no group line reports a failure although the last line does, so the bookkeeping in the fixture is wrong"
    fi
    echo "$out"
}

sys_stall() {
    local tag="$1" err="$2" last
    last="$(grep -h "^$tag: " "$err" 2>/dev/null | tail -1 | sed "s/^$tag: //")"
    if [ -z "$last" ]; then
        echo "the fixture never wrote its first progress note"
    else
        echo "the last progress note was '$last', so it stopped inside that group of checks"
    fi
}

sys_run_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" reason
    reason="$(native_run_reason "$rc" "$out" "$err")"
    if [ -z "$reason" ]; then
        echo ""
    elif grep -q '^ocerz: bridge: .* refused$' "$out" "$err" 2>/dev/null; then
        echo "$reason; ocerz refused a call: $(grep -h '^ocerz: bridge: .* refused$' "$out" "$err" | head -1 | cut -c1-240)"
    elif grep -qE "$BRIDGE_FAULT_RE" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hE "$BRIDGE_FAULT_RE" "$out" "$err" | head -1 | cut -c1-120); $(sys_stall "$tag" "$err")"
    elif grep -Fq "$GUEST_CRASH" "$out" "$err" 2>/dev/null; then
        echo "$reason; $(grep -hF "$GUEST_CRASH" "$out" "$err" | head -1 | cut -c1-120); $(sys_stall "$tag" "$err")"
    elif [ "$rc" -eq 124 ]; then
        echo "$reason: still running after ${NATIVE_TIMEOUT}s, and $(sys_stall "$tag" "$err")"
    elif [ "$rc" -gt 128 ] && [ "$rc" -lt 160 ]; then
        echo "$reason: the process was ended by signal $((rc - 128)); $(sys_stall "$tag" "$err")"
    else
        echo "$reason; $(sys_stall "$tag" "$err")"
    fi
}

sys_arm64_reason() {
    local rc="$1" tag="$2" out="$3" err="$4" line
    shift 4
    line="$(sys_status "$tag" "$out")"
    if grep -q "^$tag bad:" "$out" 2>/dev/null; then
        echo "the arm64 build, run directly on the host, fails its own checks, so the fixture expects something the host does not do and the native run can prove nothing either way: $(sys_bits "$tag" "$out" "$@")"
    elif [ "$rc" -eq 124 ]; then
        echo "the arm64 build, run directly on the host, was still running after ${NATIVE_TIMEOUT}s, and $(sys_stall "$tag" "$err")"
    elif [ "$rc" -ne 0 ] || ! grep -q "^$tag ok" "$out" 2>/dev/null; then
        echo "the arm64 build, run directly on the host, exited $rc with '${line:-nothing}' and no '$tag ok' line, so the fixture itself is broken, and $(sys_stall "$tag" "$err")"
    fi
}

sys_modes_reason() {
    local log="$1" kinds="$2" kind lines bad
    if [ -z "$kinds" ]; then
        return
    fi
    if [ ! -s "$log" ]; then
        echo "no child wrote to the mode log, so nothing shows the children ran under ocerz in native mode"
        return
    fi
    bad="$(grep -v ' mode=native exe=ocerz$' "$log" | head -1)"
    if [ -n "$bad" ]; then
        echo "a child reported '$bad': it did not come up under ocerz in native mode"
        return
    fi
    for kind in $kinds; do
        lines="$(grep -c "^$kind mode=" "$log")"
        if [ "$lines" -ne 1 ]; then
            echo "the mode log holds $lines lines for the $kind child, want exactly one"
            return
        fi
    done
}

case_sys() {
    local name="$1" bin="$2" arm="$3" tag="$4" need="$5" kinds="$6" note="$7"
    local reason="" rc_arm rc_jit rc_nojit rc_cache line cache_note=""
    local ao="$TMP/$name.arm64.out" ae="$TMP/$name.arm64.err"
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local log="$TMP/$name.modes"
    local NATIVE_TIMEOUT=$SYS_TIMEOUT
    shift 7

    if callback_fixture_missing "$name" "$bin"; then
        return
    fi
    if [ -z "$arm" ]; then
        record "$name" "the x86_64 fixture compiled and its arm64 build did not, which leaves the case without its host oracle: $( (grep -m1 -i 'error' "$TMP/$name.arm64.cc.log" || head -1 "$TMP/$name.arm64.cc.log") 2>/dev/null | cut -c1-160)"
        return
    fi
    reason="$(sys_import_reason "$bin" "$need")"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi
    run_bounded "$ao" "$ae" "$arm" "$SYS_WORK"
    rc_arm=$?
    reason="$(sys_arm64_reason "$rc_arm" "$tag" "$ao" "$ae" "$@")"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi

    rm -f "$log"
    run_bounded "$jo" "$je" env SYS_MODE_LOG="$log" "$OCERZ" -v -native "$bin" "$SYS_WORK"
    rc_jit=$?
    run_bounded "$no" "$ne" "$OCERZ" -v -native -no-jit "$bin" "$SYS_WORK"
    rc_nojit=$?
    line="$(sys_status "$tag" "$jo")"

    reason="$(sys_run_reason "$rc_jit" "$tag" "$jo" "$je")"
    if grep -q "^$tag bad:" "$jo"; then
        reason="'$line': the guest's own checks failed: $(sys_bits "$tag" "$jo" "$@")"
    elif [ -z "$reason" ] && ! grep -q "^$tag ok" "$jo"; then
        reason="exit 0 without a '$tag ok' status line: got '${line:-nothing}'"
    fi
    if [ -z "$reason" ]; then
        reason="$(sys_run_reason "$rc_nojit" "$tag" "$no" "$ne")"
        if grep -q "^$tag bad:" "$no"; then
            reason="no-jit: '$(sys_status "$tag" "$no")': the guest's own checks failed: $(sys_bits "$tag" "$no" "$@")"
        elif [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    if [ -z "$reason" ] && ! grep -v "^$tag x86 " "$jo" | cmp -s - "$ao"; then
        reason="native '$(grep -v "^$tag x86 " "$jo" | tr '\n' ' ')' != arm64 '$(tr '\n' ' ' < "$ao")': the guest got answers a native program making the same calls does not"
    fi
    if [ -z "$reason" ]; then
        reason="$(sys_modes_reason "$log" "$kinds")"
    fi
    if [ -n "$reason" ] && [ -n "$note" ]; then
        reason="$reason. $note"
    fi

    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$bin" "$SYS_WORK"
        rc_cache=$?
        if [ "$rc_cache" -eq 124 ]; then
            reason="cache mode still running after ${NATIVE_TIMEOUT}s, and $(sys_stall "$tag" "$ce")"
        elif [ "$rc_cache" -ne 0 ]; then
            reason="cache-mode exit $rc_cache, want 0: '$(sys_status "$tag" "$co")'"
        elif ! cmp -s "$jo" "$co"; then
            reason="native '$(tr '\n' ' ' < "$jo")' != cache '$(tr '\n' ' ' < "$co")': native mode agrees with the arm64 build and cache mode, where the same calls go through the x86 libc to the syscall layer, answers otherwise"
        fi
    fi
    record "$name" "$reason" "exit=$rc_jit out='$line'$cache_note"
}

sys_refused_reason() {
    local rc="$1" out="$2" err="$3" msg="$4" past="$5"
    if ! grep -q " start$" "$out" 2>/dev/null; then
        echo "exit $rc before the fixture's first line"
    elif grep -q "$past" "$out" 2>/dev/null; then
        echo "exit $rc, and the fixture printed '$(grep "$past" "$out" | head -1)': the call was performed rather than refused"
    elif ! grep -Fxq "$msg" "$err" 2>/dev/null; then
        echo "exit $rc without the refusal line '$msg': got '$(grep -h '^ocerz: ' "$err" 2>/dev/null | grep -v '^ocerz: [a-z]*: ' | head -1 | cut -c1-200)'"
    elif [ "$rc" -ne "$SYS_REFUSED_STATUS" ]; then
        echo "exit $rc, want $SYS_REFUSED_STATUS after the refusal line"
    fi
}

case_sys_jmp_refused() {
    local name=sys_jmp_refused reason="" rc_jit rc_nojit rc_cache cache_note=""
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$SYS_TIMEOUT

    if callback_fixture_missing "$name" "$SYS_REFUSE_BIN"; then
        return
    fi
    reason="$(sys_import_reason "$SYS_REFUSE_BIN" '_setjmp _longjmp _qsort')"
    if [ -z "$reason" ]; then
        run_bounded "$jo" "$je" "$OCERZ" -native "$SYS_REFUSE_BIN"
        rc_jit=$?
        reason="$(sys_refused_reason "$rc_jit" "$jo" "$je" "$SYS_REFUSE_MSG" " jumped")"
    fi
    if [ -z "$reason" ]; then
        run_bounded "$no" "$ne" "$OCERZ" -native -no-jit "$SYS_REFUSE_BIN"
        rc_nojit=$?
        reason="$(sys_refused_reason "$rc_nojit" "$no" "$ne" "$SYS_REFUSE_MSG" " jumped")"
        if [ -n "$reason" ]; then
            reason="no-jit: $reason"
        elif ! cmp -s "$jo" "$no"; then
            reason="native jit '$(tr '\n' ' ' < "$jo")' and no-jit '$(tr '\n' ' ' < "$no")' stdout differ"
        fi
    fi
    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$SYS_REFUSE_BIN"
        rc_cache=$?
        if [ "$rc_cache" -ne 0 ] || ! grep -qx 'sys_jmp_refused jumped r=5 calls=3' "$co"; then
            reason="cache mode, where qsort is translated x86 code and the jump skips nothing native, exited $rc_cache with '$(tr '\n' ' ' < "$co")', want the jump performed"
        fi
    fi
    record "$name" "$reason" "exit=${rc_jit:-} no-jit exit=${rc_nojit:-}$cache_note"
}

case_sys_fork_callback() {
    local name=sys_fork_callback reason="" rc_arm rc_jit rc_nojit rc_cache cache_note=""
    local ao="$TMP/$name.arm64.out" ae="$TMP/$name.arm64.err"
    local jo="$TMP/$name.jit.out" je="$TMP/$name.jit.err"
    local no="$TMP/$name.nojit.out" ne="$TMP/$name.nojit.err"
    local co="$TMP/$name.cache.out" ce="$TMP/$name.cache.err"
    local NATIVE_TIMEOUT=$SYS_TIMEOUT

    if callback_fixture_missing "$name" "$SYS_FORKCB_BIN"; then
        return
    fi
    if [ -z "$SYS_FORKCB_ARM64" ]; then
        record "$name" "the x86_64 fixture compiled and its arm64 build did not, which leaves the case without its host oracle"
        return
    fi
    reason="$(sys_import_reason "$SYS_FORKCB_BIN" '_fork _qsort')"
    if [ -z "$reason" ]; then
        run_bounded "$ao" "$ae" "$SYS_FORKCB_ARM64"
        rc_arm=$?
        if [ "$rc_arm" -ne 0 ] || ! grep -qx 'sys_fork_callback parent saw=8 sorted=1' "$ao"; then
            reason="the arm64 build, run directly on the host, exited $rc_arm with '$(tr '\n' ' ' < "$ao")', so the fixture itself is broken"
        fi
    fi
    if [ -z "$reason" ]; then
        run_bounded "$jo" "$je" "$OCERZ" -native "$SYS_FORKCB_BIN"
        rc_jit=$?
        reason="$(sys_refused_reason "$rc_jit" "$jo" "$je" "$SYS_FORKCB_MSG" " parent ")"
    fi
    if [ -z "$reason" ]; then
        run_bounded "$no" "$ne" "$OCERZ" -native -no-jit "$SYS_FORKCB_BIN"
        rc_nojit=$?
        if [ "$rc_nojit" -ne 0 ] || ! cmp -s "$no" "$ao"; then
            reason="no-jit, where no translated frame lies beneath the comparator, exited $rc_nojit with '$(tr '\n' ' ' < "$no")', want the fork performed and arm64's '$(tr '\n' ' ' < "$ao")'"
        fi
    fi
    if [ -n "$reason" ]; then
        :
    elif [ "$CACHE_OK" -ne 1 ]; then
        cache_note=" cache=skipped"
    else
        run_bounded "$co" "$ce" "$OCERZ" -cache "$SYS_FORKCB_BIN"
        rc_cache=$?
        if [ "$rc_cache" -ne 0 ] || ! cmp -s "$co" "$ao"; then
            reason="cache mode exited $rc_cache with '$(tr '\n' ' ' < "$co")', want arm64's '$(tr '\n' ' ' < "$ao")'"
        fi
    fi
    record "$name" "$reason" "exit=${rc_jit:-} no-jit exit=${rc_nojit:-}$cache_note"
}

build_fixtures
build_callback_fixtures
build_attach_fixtures
build_thread_fixtures
build_tlv_fixtures
build_signal_fixtures
build_cf_fixtures
build_objc_fixtures
build_objc_class_fixtures
build_app_fixtures
build_block_fixtures
build_dl_fixtures
build_sys_fixtures

if [ -n "$PROBE_BIN" ]; then
    run_probe "$TMP/probe_native.jit.out" "$TMP/probe_native.jit.err" -v -native
    echo $? > "$TMP/probe_native.jit.rc"
    run_probe "$TMP/probe_native.nojit.out" "$TMP/probe_native.nojit.err" -v -native -no-jit
    echo $? > "$TMP/probe_native.nojit.rc"
fi

for k in $KERNELS; do
    case_native_dyn "$k"
    case_cache_dyn "$k"
    case_native_matches_cache "$k"
done
case_native_bound
case_bridge_probe_heap
case_bridge_probe_env
case_bridge_probe_native
case_bridge_probe_cache
case_bridge_unimpl
case_bridge_fault_native
case_bridge_fault_not_guest
case_bridge_fault_cache
case_bridge_frame_lowered
case_bridgelog
case_callback callback_qsort "$CB_QSORT_BIN" qsort \
    "bit 0 is the first ascending sort, 1 the descending sort and 2 the second ascending sort coming out other than -150..149 in order, 3 a comparator that never ran, 4 a comparator entered on a misaligned stack"
case_callback callback_nested_bridge "$CB_NESTED_BIN" nested \
    "bit 0 is strings out of order after a comparator built on bridged strcmp, 1 a result that is not a permutation of the input, 2 a comparator that never ran"
case_callback callback_bsearch "$CB_BSEARCH_BIN" bsearch \
    "bit 0 is a present key not found at its own element, 1 an absent key found, 2 a search of zero elements finding something, 3 a comparator that never ran, 4 the key pointer arriving changed, 5 an element pointer off an element boundary, 6 a comparator entered on a misaligned stack"
case_callback callback_recursion "$CB_RECURSION_BIN" recursion \
    "bit 0 is a level not entered exactly once, 1 a level not left exactly once, 2 the nesting not reaching depth 24, 3 a level running the other comparator, 4 a caller's elements changing under a nested level, 5 a level's array coming back unsorted, 6 a comparator entered on a misaligned stack, 7 the levels entered or left out of order"
case_callback_guest_fault
case_attach attach_async "$AT_ASYNC_BIN" attach_async \
    "bit 0 is a null global queue or semaphore, 1 a dispatch_semaphore_wait returning non-zero, 2 a work function not run exactly once for its submission, 3 a work function handed a context the guest never submitted, 4 a work function computing a value other than the main thread's, 5 a work function run on the calling thread instead of a libdispatch worker, 6 a work function entered on a misaligned stack" \
    "attach_async is the first guest code in the project to run on a thread the guest never created: a libdispatch worker has no guest cpu until the callback dispatcher attaches a personality to it, so a failure here means native code cannot call guest code on any thread of its own"
case_attach attach_apply "$AT_APPLY_BIN" attach_apply \
    "bit 0 is a null global queue, 1 an iteration never run, 2 an iteration run more than once, 3 an iteration's value or the checksum differing from the main thread's, 4 an iteration handed the wrong context, 5 an iteration index past the end, 6 an iteration entered on a misaligned stack, 7 every iteration run on the calling thread, so no worker was attached and nothing was tested"
case_attach attach_nested_bridge "$AT_NESTED_BIN" attach_nested \
    "bit 0 is a null global queue or semaphore, 1 the work function not run exactly once with its own context or the wait failing, 2 the work function run on the calling thread, 3 a wrong total from bridged strlen on the worker, 4 a wrong answer from bridged strcmp on the worker, 5 strings out of order after qsort on the worker, 6 a result that is not a permutation, 7 a comparator that never ran, 8 a comparator run on a thread other than the work function's, 9 a work function or comparator entered on a misaligned stack"
case_attach attach_sync "$AT_SYNC_BIN" attach_sync \
    "bit 0 is a null global queue, 1 the work function not run exactly once before each dispatch_sync_f returned, 2 a wrong context, 3 the work run on a thread other than the caller's, so reusing the caller's cpu went untested, 4 the work function's frame not just below its caller's on the caller's own guest stack, which means a second personality was built for a thread that already had a cpu, 5 the caller's own state changing across dispatch_sync_f, 6 a work function entered on a misaligned stack"
case_attach_guest_fault
case_thread thread_create_join "$TH_CREATE_BIN" thread_create_join \
    "bit 0 is a pthread_create returning non-zero, 1 a pthread_join returning non-zero, 2 a start routine not run exactly once or handed an argument the creator never passed, 3 a value from pthread_join other than the one its start routine returned, which has bits set above bit 32 so a result cut to 32 bits shows here, 4 pthread_self in a thread differing from the pthread_t pthread_create handed back, 5 pthread_self in a thread equal to the main thread's, 6 pthread_self changing within a thread or on the main thread, 7 two threads alive at once sharing a pthread_t, 8 a start routine entered on a misaligned stack" \
    "thread_create_join is the plainest guest thread there is: native pthread_create starts a host thread that must enter the callback trampoline and be given a guest personality before the start routine runs, so a failure here means no guest program that creates a thread can run in native mode"
case_thread thread_mutex "$TH_MUTEX_BIN" thread_mutex \
    "bit 0 is a pthread_create or pthread_join failing or a thread's argument or result not its own, 1 pthread_mutex_init, a lock, an unlock or pthread_mutex_destroy returning non-zero, 2 a thread not completing all its rounds, 3 a final count other than 200000, so increments were lost and the statically initialized mutex did not exclude, 4 a thread finding another already inside the critical section, 5 the tally kept under the mutex from pthread_mutex_init coming out wrong, 6 a start routine entered on a misaligned stack"
case_thread thread_cond "$TH_COND_BIN" thread_cond \
    "bit 0 is a pthread_create or pthread_join failing or the consumer's result not its own argument, 1 a lock, unlock, wait, signal, broadcast or pthread_cond_init returning non-zero or a thread handed the wrong argument, 2 other than 1000 values received, one never received or one with a sequence number out of range, 3 a value received twice, 4 values received out of order, 5 a value whose payload does not match its sequence number, 6 a thread finding another holding the mutex when it took it, including on return from pthread_cond_wait, 7 the consumer waiting fewer times than it asked, so a value was in the slot before the producer could have been asked for it, 8 a start routine entered on a misaligned stack"
case_thread thread_bridged_work "$TH_WORK_BIN" thread_bridged_work \
    "bit 0 is a pthread_create or pthread_join failing, a thread not run exactly once or a thread's argument or result not its own, 1 bridged strlen or memcpy giving a wrong length or wrong bytes on a thread, 2 malloc failing or heap memory not reading back on a thread, 3 qsort on a thread leaving elements out of order or not the thread's own, 4 a comparator that never ran or one handed an element no thread owns, 5 a comparator run on a thread other than the one that called qsort or handed another thread's element, 6 a thread's checksum differing from the one the main thread computes without bridged calls, 7 a start routine or comparator entered on a misaligned stack"
case_thread_guest_fault
case_tlv tlv_main "$TLV_MAIN_BIN" tlv_main \
    "bit 0 is the int's first read not its initializer, 1 the double's first read not its initializer, 2 an address differing between accesses, whether taken in main or returned from another function, 3 the int not holding what the calls added to it, read in main or through another function, 4 the same for the double, 5 a write through the address or made directly in main not seen the other way, 6 a thread-local access inside a function disturbing one of its six 64-bit argument registers or losing the increment it made, 7 the two variables sharing an address or off their alignment"
case_tlv tlv_bss "$TLV_BSS_BIN" tlv_bss \
    "bit 0 is a byte of the 1 MB array with no initializer reading non-zero on first touch, which nonzero= counts, 1 a second variable with no initializer reading non-zero on first touch, 2 the initialized array before them not holding its initializer, 3 a byte of the 1 MB array not reading back what was written, 4 writing the 1 MB array changing the initialized array or the second variable, 5 an address changing between accesses, 6 the thread-local variables overlapping each other or ordinary globals, 7 an ordinary global written before the first touch not reading back what was written"
case_tlv tlv_layout "$TLV_LAYOUT_BIN" tlv_layout \
    "bits 0 to 11 are the initializers of the char, short, int, signed char, long long, double, float, struct, int array, pointer, 16-byte-aligned array and trailing char, in that order, where a pointer not pointing at its string means the rebase inside __thread_data was lost, 12 a variable with no initializer reading non-zero, 13 a variable off its type's alignment, 14 two variables overlapping or all of them spanning more than a page, 15 a variable not reading back the new value written to it after every variable was written, so two of them share storage, 16 an address changing between accesses"
case_tlv tlv_threads "$TLV_THREADS_BIN" tlv_threads \
    "bit 0 is a pthread_create or pthread_join failing, a thread not run exactly once or handed another thread's argument, 1 the main thread's own first read not the initializers or its thread-local array not zero, 2 a thread's first read not the initializers, so it saw the main thread's or an earlier thread's values, 3 a thread's thread-local array not zero on first touch, 4 a thread, the main thread among them, not reading back the values it just wrote or seeing an address change, 5 a thread's values changing while it waited and the others wrote theirs, 6 two of the seven threads alive at once sharing the address of a thread-local variable, 7 the main thread's values changed by the threads, 8 a lock, unlock, wait or broadcast returning non-zero, 9 a start routine entered on a misaligned stack"
case_tlv_churn \
    "bit 0 is a pthread_create or pthread_join failing, a thread not run exactly once or handed another thread's argument, 1 the main thread's own first touch or read-back failing, 2 a thread starting from anything but the initializers, which stale= counts and which is a block reused without being initialized again or a table left behind by a thread that went away, 3 a thread's 2 MB thread-local array not zero where the thread before it marked it, 4 a thread not reading back its own values and page marks, 5 the main thread's values changed by the threads, 6 a start routine entered on a misaligned stack"
case_tlv tlv_dylib "$TLV_DYLIB_BIN" tlv_dylib \
    "bit 0 is a pthread_create or pthread_join failing, a thread not run exactly once or handed another thread's argument, 1 the main thread's first read of the dylib's variables or the main image's not the initializers or the dylib's thread-local array not zero, 2 a thread's first read of them not the initializers, 3 a thread's copy of the dylib's array not zero, 4 the main image reaching the dylib's int through its thread-local import at an address other than the one the dylib's accessor returns, or reading another value there, 5 the dylib's int and the main image's sharing an address or a write to one showing in the other, so the two images share a block, 6 a thread not reading back its own values in both images, 7 a thread's values changing while the others wrote theirs, 8 two threads alive at once sharing an address in either image, 9 the main thread's values changed by the threads, 10 a lock, unlock, wait or broadcast returning non-zero, 11 a start routine entered on a misaligned stack" \
    "$TLV_DYLIB_LIB"
case_signal signal_sigaction "$SIG_ACTION_BIN" signal_sigaction \
    "bit 0 is a sigaction call returning non-zero, 1 raise returning other than 0, 2 the SA_SIGINFO handler not run exactly once by the time raise returned, 3 the handler handed a signal number other than SIGUSR1, 4 a null siginfo or one whose si_signo is not SIGUSR1, 5 a null ucontext, 6 the checksum over the values held across raise differing from one computed without a signal, so a callee-saved register or stack slot the caller kept live changed under the handler, 7 the pattern on the caller's own stack changing across raise, 8 the old action from the second sigaction not naming the first handler, 9 that old action without SA_SIGINFO, 10 that old action without the first handler's mask, 11 a query with no new action not reporting the second handler with an empty mask and no SA_SIGINFO, 12 the second raise not running the second handler exactly once, or running the first again, 13 a handler entered on a misaligned stack" \
    "signal_sigaction is the plainest delivery there is: a handler installed with sigaction and raised on the same thread, so a failure here means no guest program that handles a signal can run in native mode"
case_signal signal_signal "$SIG_SIGNAL_BIN" signal_signal \
    "bit 0 is the first signal(SIGUSR2, h) returning other than SIG_DFL, 1 the second returning other than h, 2 the first raise not returning 0 with the handler run once, 3 the second raise not returning 0 with the handler run twice, so it did not stay installed, 4 the handler handed a signal number other than SIGUSR2, 5 signal(SIGUSR1, SIG_IGN) not returning SIG_DFL, or the raise of the ignored SIGUSR1 returning non-zero or running the handler, 6 restoring the defaults not handing back SIG_IGN and h, 7 the handler entered on a misaligned stack"
case_signal signal_mask "$SIG_MASK_BIN" signal_mask \
    "bit 0 is sigemptyset, sigaddset, sigdelset, sigfillset or sigismember called as a function failing or building a set other than the macros build, 1 sigaddset called as a function accepting signal 99, 2 sigaction or a sigprocmask or pthread_sigmask call returning non-zero, 3 a raise of the blocked SIGUSR1 returning non-zero, 4 the handler running while SIGUSR1 was blocked, on the raise or on the query after it, 5 a query not reporting SIGUSR1 blocked, 6 the handler not run exactly once by the time the unblocking call returned, or not three times in all, 7 SIGUSR1 or SIGUSR2 blocked before a round began or after it ended, so the mask was not put back when the handler returned, 8 the handler's own pthread_sigmask query failing or finding SIGUSR1 or its sa_mask's SIGUSR2 open while it ran, 9 the handler handed another signal or entered on a misaligned stack"
case_signal signal_altstack "$SIG_ALTSTACK_BIN" signal_altstack \
    "bit 0 is the first sigaltstack query failing, not reporting SS_DISABLE or reporting SS_ONSTACK, 1 malloc failing or installing or querying the alternate stack returning non-zero, 2 the query not reading back the installed ss_sp and ss_size, 3 the query of the installed stack reporting SS_DISABLE or SS_ONSTACK, 4 sigaction or a raise returning non-zero, 5 the SA_ONSTACK handler not run twice or the other not once, 6 the SA_ONSTACK handler's local outside the alternate stack, 7 the handler without SA_ONSTACK running on the alternate stack, 8 a query after both handlers returned failing or still reporting SS_ONSTACK, 9 disabling the alternate stack failing or the query after it not reporting SS_DISABLE, 10 the SA_ONSTACK handler running on the disabled alternate stack, 11 a handler entered on a misaligned stack"
case_signal signal_pthread_kill "$SIG_PTKILL_BIN" signal_pthread_kill \
    "bit 0 is sigaction returning non-zero, 1 pthread_kill on the main thread not returning 0 with the handler already run once on that thread, 2 pthread_create or pthread_join failing, the worker never reporting that it was running, or its result not its own argument, 3 pthread_kill on the worker returning non-zero, 4 the handler not run exactly once for the worker, or the worker giving up after SIGNAL_WAIT_SECS seconds without it, 5 the handler run on a thread other than the worker, or the worker's pthread_self differing from the pthread_t pthread_create handed back, 6 the handler for the worker's signal run on the main thread, 7 the handler handed another signal or entered on a misaligned stack" \
    "signal_pthread_kill is the only signal case whose delivery cannot happen at the sender's own crossing: the signal has to reach the worker through the host and be delivered at the worker's next crossing, which the worker's wait makes by calling getpid on every turn"
case_signal signal_kill "$SIG_KILL_BIN" signal_kill \
    "bit 0 is sigaction returning non-zero, 1 kill with signal 0 failing or running the handler, 2 kill(getpid(), SIGUSR1) returning non-zero, 3 the handler not run within SIGNAL_WAIT_SECS seconds or run more than once, 4 the handler run on a thread other than the main thread, the only one the guest has, 5 the handler handed another signal or entered on a misaligned stack"
case_signal signal_errors "$SIG_ERRORS_BIN" signal_errors \
    "bits 0 to 5 are sigaction on SIGKILL, SIGSTOP, 0, 200 and NSIG and a query of SIGKILL with no new action, in that order, not returning -1 with errno EINVAL, 6 sigaltstack with a stack one byte short of MINSIGSTKSZ not returning -1 with errno ENOMEM, 7 raise or kill of signal 200 not returning -1 with EINVAL, 8 raise or kill of signal 0 failing, 9 pthread_kill of 200 or NSIG not returning EINVAL, 10 pthread_kill of signal 0 failing, 11 the handler run by any of them"
case_signal_default
case_signal signal_handler_bridge "$SIG_HANDLER_BIN" signal_handler_bridge \
    "bit 0 is sigaction or a raise from main returning non-zero, 1 the two handlers not each run once per raise by the time it returned, or a raise returning with a handler still in progress, 2 bridged strlen inside the handler giving a wrong length, 3 bridged memcpy inside the handler returning the wrong pointer or copying the wrong bytes, 4 bridged strcmp inside the handler giving a wrong answer, 5 a write from inside a handler writing short, 6 the raise of SIGUSR2 inside the SIGUSR1 handler returning non-zero or not running its handler nested inside the first before it returned, 7 a handler handed another signal or entered on a misaligned stack"
case_cf cf_basic "$CF_BASIC_BIN" "$CF_BASIC_ARM64" cf_basic "$CF_CLASS_SYM" \
    "cf_basic is the plainest use of a framework there is, with no guest code for CoreFoundation to call, so a failure here means no guest program that uses CoreFoundation at all can run in native mode" \
    string "bit 0 is CFStringCreateWithCString failing or handing back something whose type is not CFStringGetTypeID, 1 CFStringGetLength other than the 20 UTF-16 units of the text, 2 a CFStringGetCharacterAtIndex result other than the expected UniChar, where several lie above 0xff and two are a surrogate pair, 3 CFStringGetCString into a large buffer not returning exactly true or not reproducing the UTF-8 bytes, 4 CFStringGetCString into an 8-byte buffer not returning exactly false, 5 CFStringGetCString into ASCII not returning exactly false, 6 CFStringCreateWithBytes over UTF-16LE bytes not exactly equal to the matching CFSTR literal, 7 CFStringCreateWithBytes over the first five UTF-8 bytes not exactly equal to the literal Hello, 8 kCFStringEncodingUnicode bytes not read in host order with isExternalRepresentation false and big-endian with it true, which is a Boolean argument that did not arrive" \
    literal "bit 0 is an ASCII or a UTF-16 CFSTR literal whose type is not CFStringGetTypeID, which is an isa not bound to the native $CF_CLASS_SYM, 1 CFEqual between the literal and the same text created at run time not exactly true in either order, 2 their CFHash values differing, 3 CFEqual against a literal differing only in case not exactly false, 4 CFRetain of a literal handing back another pointer, 5 the UTF-16 literal not 9 units long or its first or last character wrong, 6 the UTF-16 literal not exactly equal to, or hashing differently from, the same text created at run time, 7 CFStringGetCStringPtr on the literal returning a pointer to other text, 8 CFStringCreateCopy of the literal failing or not exactly equal to it" \
    mutable "bit 0 is CFStringCreateMutable failing or the string not 9 units long after three appends, 1 its last character or its text wrong, 2 CFStringCompare not returning exactly -1 and 1 for strings in and out of order, which a CFComparisonResult cut to 32 bits does not, 3 a case-insensitive compare not 0 or a case-sensitive one not -1, 4 kCFCompareNumerically not putting file10 after file9 or a plain compare not putting it before, 5 CFStringHasPrefix not exactly true and false, 6 CFStringHasSuffix not exactly true and false, 7 CFStringGetIntValue wrong for -123456, 2147483647 or a number after spaces, 8 CFStringGetDoubleValue wrong for 3.25 or -0.5e3, 9 a CFStringCreateMutableCopy of a literal not reading Hello world after an append, or the literal changing with it, 10 CFStringGetMaximumSizeForEncoding under 30 bytes for ten units of UTF-8" \
    split "bit 0 is CFStringCreateArrayBySeparatingStrings not handing back an array of four, 1 an element other than a, bb, the empty string and ccc in that order, 2 CFStringCreateByCombiningStrings not producing a::bb::::ccc, 3 a separator that does not occur not giving an array holding only the whole string" \
    array "bit 0 is CFArrayCreate with kCFTypeArrayCallBacks not handing back an array of three, 1 CFArrayGetValueAtIndex handing back a pointer other than the object inserted, 2 two reads of the same index handing back different pointers, 3 an element not retained exactly once by the array, 4 a mutable array's order wrong after an append, an insert, a set and a remove, or the array not empty after CFArrayRemoveAllValues, 5 the references the mutable array holds not what those edits leave, 6 CFArrayCreateCopy not exactly equal to the original, or a mutable copy changed by an append or a set still equal to it, 7 an element's retain count not back where it started once every array was released" \
    dict "bit 0 is CFDictionaryCreate not handing back a dictionary of three, 1 a lookup through a separately created key not handing back the very value stored, 2 CFDictionaryContainsKey not exactly true and false, 3 CFDictionaryGetValueIfPresent not exactly true with the stored value, 4 the same call for an absent key not exactly false or writing its out pointer, 5 CFDictionaryGetKeysAndValues pairing a key with a value other than its own, 6 a dictionary with kCFCopyStringDictionaryKeyCallBacks not finding its value under a mutable key's old text, or finding it under the new, once the key was changed, so the key was not copied, 7 CFDictionaryAddValue replacing an existing value or CFDictionarySetValue not replacing it, 8 a value not retained exactly once while held or not released on removal, or the dictionary's count wrong, 9 CFDictionaryCreateCopy not exactly equal to the original, or a mutable copy with an extra key still equal" \
    number "bit 0 is an SInt32 CFNumber not reading back -123456789 with exactly true or not of CFNumberGetTypeID, 1 an SInt64 CFNumber not reading back all 64 bits with exactly true, 2 a Float64 CFNumber not reading back exactly, 3 CFNumberGetType not answering kCFNumberSInt32Type, kCFNumberSInt64Type and kCFNumberFloat64Type, 4 a lossy read of the SInt64 into an SInt32 not returning exactly false, 5 CFNumberCompare not returning exactly 1, -1, 0 and 1, 6 kCFNumberNaN, kCFNumberPositiveInfinity or kCFNumberNegativeInfinity not reading back as NaN and the two infinities, 7 CFBooleanGetValue not exactly true for kCFBooleanTrue and false for kCFBooleanFalse, 8 the two booleans the same object or not of CFBooleanGetTypeID, 9 kCFNull null, not exactly equal to itself or of the boolean type" \
    data "bit 0 is CFDataCreate not handing back 64 bytes of CFDataGetTypeID, 1 CFDataGetBytePtr null, the caller's own buffer or other bytes, 2 CFRetain handing back another pointer, the retain count not one higher after it or not back after CFRelease, 3 an empty CFData not zero bytes long, 4 CFGetAllocator not kCFAllocatorSystemDefault for data created with kCFAllocatorDefault, 5 CFCopyDescription not handing back a non-empty string"
case_cf cf_callbacks "$CF_CALLBACKS_BIN" "$CF_CALLBACKS_ARM64" cf_callbacks "" \
    "cf_callbacks is the first case in which the bridge converts function pointers held in a structure rather than passed in a register, so a failure here means no guest program that gives a collection callbacks of its own can run in native mode" \
    array "bit 0 is the mutable array not holding six elements with the keys its edits leave, in order, and the same pointer on every read, 1 the array not holding the stand-in a retain callback handed back, so a guest callback's pointer result did not reach CoreFoundation, 2 retain and release not called exactly six and zero times by the appends and eight and two times after the remove, set and insert, or an element's own count wrong after them, 3 a callback handed an allocator other than kCFAllocatorMalloc, 4 CFEqual against an array of equal elements not exactly true after exactly six calls of equal, 5 CFEqual against an array differing at its fourth element not exactly false after exactly four, which is also what a guest Boolean handed back without being narrowed looks like, 6 equal handed its arguments in the wrong order, 7 CFCopyDescription not calling copyDescription once per element or not containing the text those calls returned, 8 CFArrayCreateMutableCopy not holding six elements or not retaining each of them once more, 9 an element's count not back at zero after the last release, release not called once per remaining element when the array was freed, retains and releases not balancing, a callback handed a value that is no element, or a callback entered on a misaligned stack" \
    mixed "bit 0 is kCFTypeArrayCallBacks not a version-0 structure with all four words set, 1 the arrays not holding what was put in them, 2 the native retain word in a structure with a guest equal word not retaining each string once per array holding it, 3 CFEqual between two arrays with the mixed callbacks not exactly true after exactly two calls of the guest equal, 4 CFEqual against an array with the unmodified callbacks not exactly false or calling the guest equal, 5 CFEqual not exactly false once one string was changed, 6 CFCopyDescription through the native copyDescription word not containing the strings, 7 a string's retain count not back after the arrays were released, or a callback entered on a misaligned stack" \
    dict "bit 0 is a dictionary with guest key callbacks not holding eight entries, 1 a key looked up through a copy at another address not finding its value, or the guest equal not called for it, 2 an absent key reported present, 3 CFDictionaryGetValueIfPresent not exactly true with the stored value, 4 a removed key still counted or present, 5 CFDictionaryGetKeysAndValues handing back a key pointer other than the one stored or a value other than its own, or the guest hash called fewer times than there are keys, 6 a hash or equal callback entered on a misaligned stack" \
    apply "bit 0 is CFDictionaryApplyFunction over the C-string dictionary not calling the applier once per entry with its own context and each key's own value, 1 the checksum over those calls wrong, 2 the same over a dictionary of CFString keys and CFNumber values, whose applier makes bridged calls, 3 that checksum wrong, 4 an applier, hash or equal callback entered on a misaligned stack"
case_cf cf_runloop "$CF_RUNLOOP_BIN" "$CF_RUNLOOP_ARM64" cf_runloop "" \
    "cf_runloop is the first case whose guest code is called from a framework's event loop rather than from a call made for the purpose, which is how every event reaches an application, so a failure here means no event-driven guest program can run in native mode" \
    loop "bit 0 is CFRunLoopGetCurrent not the main run loop on the main thread, 1 creating the timer, observer or source failing, or a context not retained exactly once on creation, 2 CFRunLoopTimerGetNextFireDate not the date CFRunLoopTimerSetNextFireDate set, 3 CFRunLoopRunInMode not returning kCFRunLoopRunStopped, 4 the timer not firing exactly three times with its own timer and info, or found invalid inside its callout, 5 the source's perform not run exactly once between each pair of fires, 6 the observer not seeing exactly one entry, first, at least one wait and nothing else, or handed another observer or info, 7 schedule not called once when the source was added, or cancel not once when it was removed and not again on invalidation, 8 a source callout handed another info, run loop or mode, or a callout run on another run loop, 9 CFRunLoopTimerIsValid not exactly true before and after the timer's removal and exactly false after its invalidation, 10 the timer's context not retained and released four times, once per callout and once for the timer, by the time it was invalidated, 11 the observer's context not retained and released exactly once, 12 the source's context the same, or a context callback handed an info the fixture never gave, 13 a callback entered on a misaligned stack"
case_objc objc_foundation "$OBJC_FOUNDATION_BIN" "$OBJC_FOUNDATION_ARM64" objc_foundation \
    "_objc_msgSend _objc_autoreleasePoolPush $CF_CLASS_SYM _OBJC_CLASS_\$_NSConstantIntegerNumber _OBJC_CLASS_\$_NSConstantDoubleNumber" "" \
    "objc_foundation is the plainest use of Objective-C there is, messages to Foundation's own classes with no class of the guest's, so a failure here means no guest program that sends a message can run in native mode" \
    string "bit 0 is stringWithUTF8String: returning nil or a string other than 15 UTF-16 units long, 1 a characterAtIndex: result other than the expected unichar, all three of which lie above 0xff, 2 UTF8String not reproducing the UTF-8 bytes, 3 isEqual: between the created string and the @\"...\" literal of the same text not exactly YES in both directions, or against another string not exactly NO, 4 the two hashing differently or the literal not 15 units long, 5 uppercaseString giving other text or changing the original, 6 substringWithRange:, which takes an NSRange by value, not giving the three-letter word, equal to the same word created separately, 7 rangeOfString: not returning {10, 3}, 8 rangeOfString: for absent text not returning exactly {NSNotFound, 0}, which a location cut to 32 bits does not" \
    tagged "bit 0 is the five-letter string nil or not a tagged pointer in either convention, so the fixture no longer tests one, 1 the long string nil or tagged, 2 the tagged string's length, last character or UTF8String wrong, 3 isEqual: between the tagged string and its literal not exactly YES both ways, or their hashes differing, 4 uppercaseString of the tagged string wrong, 5 rangeOfString: on the tagged string not {2, 2}, 6 the long string's length or UTF8String wrong, 7 isEqual: or the hash between the long string and its literal disagreeing, 8 rangeOfString: on the long string not {34, 6}, 9 stringByAppendingString: of the two not an untagged string holding both, with hasPrefix: and hasSuffix: exactly YES" \
    number "bit 0 is numberWithInt: not reading back -123456 as an int and as a double, 1 numberWithDouble: not reading back -2.75 or its intValue not -2, 2 numberWithLongLong: not reading back all 64 bits, 3 the @42 literal not reading back 42 as an int and as a double, 4 the @3.5 literal not reading back 3.5 or its intValue not 3, 5 the literals' classes not the NSConstantIntegerNumber and NSConstantDoubleNumber NSClassFromString names, which is an isa not bound to Foundation's own class, 6 isKindOfClass: NSNumber not exactly YES for either literal, 7 isEqual: between @42 and a created 42 not exactly YES both ways, 8 compare: not returning exactly -1 and 1 for numbers out of and in order, 9 compare: not NSOrderedSame for equal doubles or not NSOrderedDescending for 42 against 3.5" \
    collection "bit 0 is the @[...] literal nil or not holding three elements, 1 objectAtIndex: handing back a pointer other than the object inserted, 2 a second read or a subscript handing back another pointer, or the element not isEqual: to the object inserted, 3 containsObject: not exactly YES for an equal string at another address or not exactly NO for an absent one, 4 indexOfObject: not 1 for the number or not NSNotFound for an absent object, 5 the @{...} literal nil or not holding three entries, 6 objectForKey: through a literal key or a key created separately not handing back the very value stored, 7 two lookups of the long key, one through a key at another address, not both handing back the very value stored, or that value not isEqual: to an equal string, 8 an absent key found or a subscript lookup handing back another pointer" \
    describe "bit 0 is description nil or empty or its UTF8String null, 1 the description not containing the text of one of the elements" \
    range "bit 0 is rangeValue on valueWithRange: not handing back the location above 32 bits and the length, 1 NSStringFromRange not giving {123456789012, 42}, 2 NSRangeFromString not giving {7, 9}, 3 NSIntersectionRange or NSUnionRange, which take two NSRanges by value and return one, giving the wrong range"
case_objc objc_variadic "$OBJC_VARIADIC_BIN" "$OBJC_VARIADIC_ARM64" objc_variadic \
    "_objc_msgSend _objc_autoreleasePoolPush _CFStringCreateWithFormat _CFStringAppendFormat" "" \
    "objc_variadic is the first case whose calls have no signature at all past their format or their first object, so a failure here means no guest program that formats a string or builds a collection from a list can run in native mode" \
    format "bit 0 is stringWithFormat: with sixteen integer-class and nine double arguments not giving the text it must, which is what an argument taken from the wrong register or stack slot looks like, 1 initWithFormat: on an allocated string giving the wrong text, 2 stringByAppendingFormat: not keeping the original and adding the formatted tail" \
    objects "bit 0 is arrayWithObjects: with three objects not holding them in order as the very pointers passed, 1 the same with seven objects, most of which x86-64 passes on the stack, 2 dictionaryWithObjectsAndKeys: with three pairs not holding each value under its own key as the very pointer passed, 3 the same with one pair" \
    append "bit 0 is the mutable string not holding the text of all three appendFormat: calls, one of them with ten doubles" \
    cfformat "bit 0 is CFStringCreateWithFormat with a %@ among seven integer-class and ten double arguments giving the wrong text, 1 CFStringAppendFormat giving the wrong text"
case_objc native_printf "$NATIVE_PRINTF_BIN" "$NATIVE_PRINTF_ARM64" native_printf \
    "_sprintf _snprintf ___sprintf_chk ___snprintf_chk" prefix \
    "native_printf is printf itself, so a failure here means no guest program that prints a formatted line can run in native mode" \
    stdio "bit 0 is printf with ten integer-class and nine double arguments not returning 152, 1 printf with * widths and precisions not returning 68, 2 fprintf to stdout not returning 77, 3 fprintf to stderr not returning 141; the text of those lines is compared rather than checked" \
    buffers "bit 0 is snprintf not writing the text it must or not returning its length, 1 snprintf into a 12-byte buffer not returning the 20 the whole text needs or not truncating to 11 characters, 2 sprintf not writing the text it must or not returning its length, 3 asprintf failing, writing other text or not returning its length, 4 %p of an address not 0x followed by something other than 0, 5 %p of a null pointer not 0x0" \
    chk "bit 0 is __sprintf_chk not returning the length of the text it wrote, 1 __snprintf_chk not returning more than its 12-byte buffer holds or not truncating to 11 characters; the text of both is compared rather than checked" \
    fd "bit 0 is dprintf to stdout not returning 67, 1 dprintf to stderr not returning 37"
case_objc native_nslog "$NATIVE_NSLOG_BIN" "$NATIVE_NSLOG_ARM64" native_nslog \
    "_objc_msgSend _objc_autoreleasePoolPush _NSLog" nslog \
    "native_nslog is the one call nearly every Cocoa program makes, so a failure here means no such program can log in native mode" \
    log "bit 0 is the checksum of six 64-bit values and a double held across the four NSLog calls differing from one computed without them, so a callee-saved register or stack slot changed under a veneer, 1 the string or number passed as %@ changed by the calls"
case_objc objc_classes "$OBJC_CLASSES_BIN" "$OBJC_CLASSES_ARM64" objc_classes \
    "_objc_msgSend _objc_autoreleasePoolPush _objc_msgSendSuper2 _OBJC_METACLASS_\$_NSObject __objc_empty_cache _objc_storeWeak _objc_loadWeakRetained _objc_getProperty _objc_setProperty_atomic" classes \
    "objc_classes is the plainest class a guest can define, a subclass of NSObject that only Foundation uses, so a failure here means no guest program that defines a class of its own can run in native mode" \
    load "bit 0 is OcerzShape's +load not run before main, 1 the three +load methods not run once each in the order OcerzShape, OcerzSquare, then the NSString category, although OcerzSquare comes first in the image's list, 2 a +initialize run before the first message to its class, 3 the first message to OcerzShape failing, not running its +initialize exactly once or running OcerzSquare's, 4 the first OcerzSquare not running its own +initialize exactly once or running OcerzShape's again, 5 a later message running either +initialize again, or either one handed a class other than its own" \
    object "bit 0 is the class method factory returning nil or an instance whose class is not OcerzShape, or whose superclass, or the class's, is not the native NSObject, 1 isKindOfClass: NSObject or isMemberOfClass: OcerzShape not exactly YES, or isKindOfClass: NSString not exactly NO, 2 NSStringFromClass not naming OcerzShape or NSClassFromString not finding the class, so it is not registered with the native runtime under its name, 3 the int, double or string the designated initializer set not reading back through the properties and from the ivars alike, 4 the int or double setter not writing what the getter and the ivar read back, 5 the atomic string property, whose accessors are objc_getProperty and objc_setProperty_atomic handed the ivar's offset, not storing and returning the very string set, 6 the weak property not returning the object assigned through it or the ivar under it not holding that object, 7 valueForKey: not reaching the guest's getters for the int, the double and the string, 8 setValue:forKey: not reaching the int and double setters with the unboxed values, or disturbing the weak property" \
    describe "bit 0 is %@ not producing the text the guest's -description returns, 1 %@ not calling -description exactly once, 2 -description sent directly or -debugDescription, which native NSObject answers by calling it, giving other text, 3 the description of an NSArray of two instances not containing each element's own, 4 -description not called exactly five times over all of those, one per element for the array" \
    equality "bit 0 is -isEqual: between two distinct instances with equal fields not exactly YES both ways, or against an unequal instance or a string not exactly NO, 1 -hash differing for equal instances or equal for instances differing only in a field folded in above bit 32, 2 NSSet not collapsing the equal instances into one, not finding a third equal one or member: handing back an object never put in, 3 NSSet never calling the guest's -hash or -isEqual:, 4 NSMutableDictionary not keeping the equal keys as one holding the second value, not finding it through a third equal instance or finding a value for an unequal key, 5 the dictionary's key not a copy made by the guest's -copyWithZone: that is an OcerzShape equal to the original, 6 an NSMutableSet of twelve distinct instances not holding twelve after an equal thirteenth was added; order= is the enumeration order, which a hash cut to 32 bits moves" \
    sort "bit 0 is sortedArrayUsingSelector: not handing back six elements, 1 the order by sides not 543687, which is what a -compare: result read as 4294967295 instead of -1 looks like, 2 the guest's -compare: never called, 3 the unsorted array changed, 4 -compare: sent from guest code not returning exactly -1, 1 and 0" \
    subclass "bit 0 is an OcerzSquare or its class not reporting OcerzShape as the superclass, 1 isKindOfClass: OcerzShape not exactly YES, isMemberOfClass: OcerzShape not exactly NO or NSClassFromString not finding OcerzSquare, 2 the ivars OcerzShape's initializer set through super, or OcerzSquare's own, not holding their values once both initializers ran, 3 writing OcerzSquare's ivars changing OcerzShape's, 4 writing OcerzShape's through its setters changing OcerzSquare's, 5 valueForKey: on the subclass's accessor-less ivars, read at the native runtime's offsets, or on an inherited getter, disagreeing with guest code, 6 setValue:forKey: on those ivars not seen by guest code reading them at the guest's offsets, or changing OcerzShape's, 7 the overriding -description not building on [super description], 8 the overriding -isEqual: not calling super once per call or not telling squares apart by side, 9 respondsToSelector: not exactly YES for an inherited and an own method" \
    category "bit 0 is the category's method on an @\"...\" literal giving the wrong text, 1 the same on a string created at run time, 2 the three-letter string not tagged, or the method on it giving the wrong text, 3 the same on an NSMutableString, 4 the category's 64-bit result differing from a checksum computed without it on any of three strings, 5 the category's class method not reached through NSString or NSMutableString, 6 respondsToSelector: or instancesRespondToSelector: not exactly YES for the category's methods on strings or not exactly NO on NSNumber" \
    protocol "bit 0 is @protocol(OcerzNamed) nil or conformsToProtocol: on OcerzShape or an instance not exactly YES, 1 the same for OcerzSquare, which inherits the conformance, 2 NSObject or a string conforming, 3 OcerzShape not conforming to NSCopying and NSObject or conforming to NSCoding, 4 NSStringFromProtocol not naming OcerzNamed or NSProtocolFromString not returning the very protocol @protocol(OcerzNamed) refers to, 5 NSProtocolFromString(@\"NSCopying\") not the very protocol @protocol(NSCopying) refers to, so the image's copy was not replaced by the runtime's, 6 respondsToSelector: on an instance not exactly YES for a required and an adopted method or not exactly NO for an unimplemented optional one and an unknown selector, 7 instancesRespondToSelector: and respondsToSelector: on the class not telling instance methods from class methods" \
    lifetime "bit 0 is an instance, its name or a weak reference to either not alive while the strong reference was held, 1 the weak reference not nil once the last strong reference and the pool were gone, or the guest's -dealloc not run exactly once, 2 the name not still set inside -dealloc or still alive afterwards, so .cxx_destruct did not release the strong ivar, 3 a weak property not reading nil through the accessor and the ivar once the object it held was deallocated, 4 an OcerzSquare not running its own -dealloc and then OcerzShape's once each, 5 an instance held only by an NSMutableArray deallocated while held or not after removeAllObjects" \
    perform "bit 0 is performSelector:withObject: not returning a new instance built by the guest's -scaledBy: from the boxed factor, 1 performSelector: not returning the atomic name property, 2 performSelector:withObject:withObject: not handing both objects to the guest's method, 3 performSelector:withObject: on the class not reaching the class method, 4 performSelector: on an OcerzSquare not reaching its overriding -description, 5 the three guest methods not called once each"
case_objc objc_view_render "$OBJC_VIEW_RENDER_BIN" "$OBJC_VIEW_RENDER_ARM64" objc_view_render \
    "_objc_msgSend _objc_msgSendSuper2 _OBJC_CLASS_\$_NSView _OBJC_METACLASS_\$_NSView _NSRectFill _CGContextFillRect _objc_msgSend_stret" view \
    "objc_view_render is the first case in which a framework draws through guest code, AppKit calling a view's -drawRect: and -isFlipped overrides, so a failure here means no guest program with a view of its own can draw in native mode" \
    view "bit 0 is the view nil or not an OcerzTestView whose superclass is the native NSView, or NSClassFromString not finding the class, 1 -isFlipped sent from guest code not exactly YES or not running the override once, 2 frame or bounds, which come back as NSRects, not {{5, 7}, {32, 24}} and {{0, 0}, {32, 24}}, 3 NSStringFromRect not giving {{5, 7}, {32, 24}}, 4 -drawRect: called before anything was rendered" \
    render "bit 0 is the bitmap from bitmapImageRepForCachingDisplayInRect: not 32 by 24 points at a whole backing scale, 1 the device RGB bitmap not 32 by 24 pixels at 32 bits, 2 -drawRect: not called exactly once per cacheDisplayInRect:toBitmapImageRep:, 3 a dirty rect, handed to the guest by value, other than the bounds, 4 -drawRect: called on an object other than the view, 5 no current NSGraphicsContext or CGContext inside -drawRect:, or a context that is not flipped, 6 AppKit never calling the -isFlipped override" \
    pixels "bit 0 is a corner of the device bitmap not the background color, 1 the NSRectFill rect not red at its corners, 2 red just outside that rect, 3 the background missing where the red rect would be if the view were not flipped, 4 the stroked path missing at a corner or an edge, 5 the stroke's color inside the path or just outside it, 6 the rect filled through CoreGraphics not at its flipped place or found where it would be unflipped, 7 the cached bitmap not red where the red rect is; dev= and rep= are checksums of the two bitmaps' bytes"
case_objc objc_view_ivar "$OBJC_VIEW_IVAR_BIN" "$OBJC_VIEW_IVAR_ARM64" objc_view_ivar \
    "_objc_msgSend _objc_msgSendSuper2 _OBJC_METACLASS_\$_NSView _class_getInstanceSize" view \
    "objc_view_ivar is the first case whose class inherits from one whose native size the compiler did not know, so a failure here means a guest view's own state is not where its code and AppKit both look for it" \
    layout "bit 0 is either view nil, -initWithFrame: not run once per view, or the class or its superclass wrong, 1 the first ivar not at or past the native NSView's instance size or the ivars out of declaration order, so the offsets guest code reads were not slid, 2 the class's instance size not covering its last ivar, 3 the frame read inside -initWithFrame: after the ivars were written not the one passed, 4 either view's frame or bounds wrong after main wrote the second view's ivars, 5 valueForKey:, reading at the native runtime's offset, not finding each view's own int, 6 valueForKey: not finding the double, the color, the NSRect and the trailing byte guest code wrote, 7 setValue:forKey: on one view not seen by guest code or changing the other view" \
    draw "bit 0 is -drawRect: not called exactly once per view, 1 -drawRect: called on the wrong view, 2 the int or the double written in -initWithFrame: reading otherwise in -drawRect:, 3 the color written in -initWithFrame: not the very object -drawRect: read, 4 the NSRect or the trailing byte reading otherwise in -drawRect:, 5 the second view's -drawRect: not seeing every value main wrote into it, 6 -frame inside -drawRect: not each view's own" \
    pixels "bit 0 is a corner of the first view's bitmap not white, 1 the first view's color not at the corners of its box, 2 that color outside its box, 3 the second view's color not at its own box, or the first view's box drawn in it, 4 the second view's color outside its box; dev1= and dev2= are checksums of the two bitmaps' bytes"
case_objc block_dispatch "$BLOCK_DISPATCH_BIN" "$BLOCK_DISPATCH_ARM64" block_dispatch \
    "_dispatch_once _dispatch_sync _dispatch_async _dispatch_after _dispatch_apply __NSConcreteStackBlock __NSConcreteGlobalBlock" blocks \
    "block_dispatch hands libdispatch x86 blocks, so a failure here means no guest program that uses Grand Central Dispatch's block interface can run in native mode" \
    once "bit 0 is a dispatch_once block run other than once across three calls with one predicate, 1 the value it and a capturing block under a second predicate leave not 49, which is also what a second block run under a spent predicate looks like" \
    sync "bit 0 is dispatch_sync to a global queue not running the block with its captured addend before returning, 1 dispatch_sync to a serial queue not running it, 2 a dispatch_sync nested inside a block native code is running not running before the outer one returns, 3 dispatch_barrier_sync not running" \
    async "bit 0 is eight dispatch_async blocks not all signalling the semaphore within the deadline or not each adding its own captured index, 1 a dispatch_group_wait timing out, 2 the ten group blocks not adding up to 450, 3 dispatch_group_notify's block not running" \
    after "bit 0 is the dispatch_after block having run before the call returned, 1 it not running within the deadline" \
    byref "bit 0 is four dispatch_sync blocks updating one __block variable not leaving 99, 1 the dispatch_async block not signalling within the deadline, 2 its update not reaching the variable, 3 the frame's own update through the forwarding pointer lost to a later block" \
    apply "bit 0 is dispatch_apply over a global queue not summing its indices to 499500, 1 dispatch_apply with DISPATCH_APPLY_AUTO not filling every slot of the captured array"
case_objc block_runtime "$BLOCK_RUNTIME_BIN" "$BLOCK_RUNTIME_ARM64" block_runtime \
    "__Block_copy __Block_release __Block_object_assign __Block_object_dispose _qsort_b _bsearch_b _dispatch_block_create" blocks \
    "block_runtime calls libclosure's functions on x86 blocks directly, so a failure here means a guest's own Block_copy and Block_release are wrong in native mode" \
    copy "bit 0 is Block_copy of a stack block failing or handing two blocks one copy, 1 a copied block giving a wrong answer, 2 Block_copy of a heap block not handing back the same pointer, 3 a heap block dead after one of two releases, 4 Block_copy of a global block not the block itself, 5 a block capturing a block not copied to a new address, 6 it giving a wrong answer once the captured block's own reference is gone, which is what a captured block the copy helper did not copy looks like" \
    byref "bit 0 is two heap blocks sharing a __block int not seeing each other's updates, 1 the frame's own update through the forwarding pointer not seen by them, 2 a __block variable holding a block not replaced by the block that swaps it, which is a keep helper that did not run" \
    sort "bit 0 is qsort_b with a block comparator not sorting or never calling it, 1 bsearch_b with a block comparator not finding every element at its own address" \
    result "bit 0 is dispatch_block_create handing back null, 1 the guest calling the native block directly not running it, 2 a second direct call not running it, 3 dispatch_block_wait timing out on it after dispatch_async, 4 the block not having run by then, 5 dispatch_block_perform not running its block, 6 dispatch_block_testcancel wrong for a cancelled and a live block, 7 a cancelled block running when dispatched"
case_objc block_foundation "$BLOCK_FOUNDATION_BIN" "$BLOCK_FOUNDATION_ARM64" block_foundation \
    "_objc_msgSend __Block_object_assign _objc_retainBlock _OBJC_CLASS_\$_NSItemProvider _OBJC_CLASS_\$_NSBlockOperation" blocks \
    "block_foundation hands Foundation x86 blocks and a guest method a native one, so a failure here means no Objective-C program that passes a block to Foundation can run in native mode" \
    enumerate "bit 0 is enumerateObjectsUsingBlock: not stopping after the index where the block set *stop, 1 enumerateKeysAndObjectsUsingBlock: not visiting every pair, 2 indexesOfObjectsPassingTest: not selecting by the BOOL the block returns, 3 a concurrent enumeration on libdispatch's workers not visiting every element once" \
    sort "bit 0 is sortedArrayUsingComparator: not sorting ascending or never calling the block, 1 a descending comparator not sorting descending, which is what an NSComparisonResult read from the wrong width looks like, 2 sortUsingComparator: not sorting by remainder then value" \
    notify "bit 0 is the observer block not run once per post with the notification it was posted with, 1 the guest object it captured dead while the observer lived, 2 the block still run after removeObserver:" \
    lifetime "bit 0 is dispatch_group_wait timing out on the block, 1 the block's three runs through dispatch_sync, the array's copy and dispatch_group_async not all reaching the captured object, 2 the object dead while the block was held, 3 the object alive after the block and the pool were gone, which is a reference a wrapper never gave back" \
    provider "bit 0 is NSItemProvider's completion block not called within the deadline, 1 the guest's -loadDataWithTypeIdentifier:forItemProviderCompletionHandler: not called once or its completion handler, a native block, not reaching the guest's block once, 2 the data not the text the guest's method handed the native block" \
    operation "bit 0 is the operation's completion block not signalling within the deadline, 1 the execution blocks not adding up to 111 or the completion block not run once, 2 the completionBlock getter handing back nil, 3 the block it handed back not the guest's own when called, 4 the property not cleared" \
    xpc "bit 0 is the reply block, or the proxy's error handler, not called within the deadline, 1 the error handler called or the guest's exported object not called once, 2 the reply not the doubled values and their count, which is what a reply block NSXPC could not describe, or one native code was handed as a plain pointer, looks like"
case_app_bundle
case_dl_basic \
    images "bit 0 is _dyld_image_count not above 1 or image 0 without a header, 1 dladdr on a main-image function not naming image 0's header, 2 _dyld_get_prog_image_header not image 0's, 3 image 0's name not the fixture's path, 4 _dyld_get_image_header_containing_address on a main-image function not image 0, 5 an index one past the end answering a header or a name, 6 an image in the list without a header or a name, 7 dyld_image_path_containing_address on a main-image function not the fixture's path, 8 image 0's slide not its header less the 0x100000000 it was linked at" \
    load "bit 0 is dlopen of the guest dylib by absolute path failing, 1 its class not found by NSClassFromString or not answering 99, 2 its NSString category not answering on a literal, 3 nor on a string made at run time, 4 its thread-local variable not reading its initializer 7, 5 a second thread not seeing a fresh 7 or not keeping its own write, 6 the main thread's 8 lost to the other thread's write" \
    order "bit 0 is the dylib's +load and C constructor not each run once, +load first, by the time dlopen returned" \
    sym "bit 0 is dlsym of a function or calling it failing, 1 dlsym of an int not pointing at its initializer 1234, 2 a write through that pointer not seen by the dylib's own code, 3 dlsym of a name the dylib does not export answering something, 4 RTLD_DEFAULT not finding the dylib's function at the address its handle gave, 5 RTLD_SELF from main not finding main's function, 6 RTLD_MAIN_ONLY not finding it, 7 RTLD_MAIN_ONLY finding the dylib's function, 8 RTLD_DEFAULT finding a name nothing exports" \
    dlerror "bit 0 is dlerror answering with nothing failed, 1 dlerror after a missed dlsym not naming the symbol, 2 a second dlerror answering again, 3 dlerror after a failed dlopen not naming the path, 4 a second dlerror answering again, 5 another thread seeing this thread's error or not getting its own, 6 this thread's error lost or replaced by the other's" \
    path "bit 0 is a second dlopen of the same path answering another handle, 1 RTLD_NOLOAD not answering the loaded dylib's handle, 2 RTLD_NOLOAD loading a dylib that was not loaded, 3 dlopen of a missing path answering a handle, 4 dlopen_preflight wrong for a loadable or a missing path, 5 dlclose not answering 0, 6 the dylib gone after dlclose while other opens still held it" \
    rpath "bit 0 is @executable_path, 1 @rpath through the program's LC_RPATH and 2 @loader_path relative to the calling image not reaching the loaded dylib's handle, 3 @rpath to a missing dylib answering a handle" \
    handles "bit 0 is dlsym on dlopen(NULL) not finding main's function, 1 nor the dylib's, which it searches as RTLD_DEFAULT does, 2 an RTLD_FIRST handle not finding the dylib's own function, 3 an RTLD_FIRST handle finding strlen in the dylib's dependencies, 4 an ordinary handle not finding strlen in them, 5 RTLD_NEXT from main finding main's own function, 6 RTLD_NEXT not finding strlen in a later image, 7 dlclose of those handles failing, 8 dlclose of a bogus handle not answering -1 with an error, 9 dlsym on a bogus handle not answering NULL with an error" \
    local "bit 0 is dlopen with RTLD_LOCAL failing, 1 dlsym through its handle failing, 2 RTLD_DEFAULT finding a symbol of an RTLD_LOCAL image, 3 dlopen of it again with RTLD_GLOBAL answering another handle, 4 RTLD_DEFAULT still not finding its symbol after that" \
    strlen "bit 0 is dlsym(RTLD_DEFAULT, strlen) failing, 1 calling it through the pointer not answering 12, 2 the pointer not the address the program's own strlen import was bound to" \
    dladdr "bit 0 is dladdr on the dylib's function failing, 1 its symbol name or address wrong, 2 its file not the dylib's path, 3 _dyld_get_image_header_containing_address disagreeing with dladdr's base, 4 dyld_image_path_containing_address not the dylib's path, 5 dladdr one byte into the function not naming the function's start, 6 dladdr on main's function not naming it, 7 the dylib's image-list entry not named for it or its slide not its header" \
    plugin "bit 0 is dlopen of the bundle failing, 1 its class not found or its superclass not the program's DLHostBase, 2 its override calling super not answering 42, 3 its -describe calling super through a format not answering plugin<host>, 4 isKindOfClass: DLHostBase not YES, 5 dlsym of its class symbol not the class NSClassFromString found" \
    callbacks "bit 0 is the add-image callback not called once per image already loaded when it was registered, 1 not called for the main executable, 2 nor for the dlopened dylib, 3 nor for the bundle, 4 called other than once per image in the list, 5 an image in the list the callback never saw" \
    version "bit 0 is dyld_get_active_platform not macOS, 1 dyld_get_program_min_os_version not the 12.0 the fixture was linked for, 2 dyld_get_program_sdk_version below it, 3 dyld_program_sdk_at_least wrong for 10.14 or 127.0, 4 _dyld_shared_cache_contains_path not true for libSystem, 5 true for the fixture's own dylib, 6 _dyld_is_memory_immutable true for heap memory"
case_dl_refusals \
    "bit 0 is the arm64-only dylib, 1 libz by path, 2 libz by bare name, 3 the dylib with a deleted dependency and 4 the dylib missing a symbol not refused with an error and the image count unchanged, 5 the second attempt at the last not failing with the same message, 6 a plain dylib not loading after those failures, 7 libc.dylib not answering libSystem with the program's own strlen, 8 RTLD_NOLOAD of libSystem not the same handle, 9 CoreFoundation loaded before anything asked, 10 CoreFoundation through its framework symlink not loading as one more image, 11 that image not named for its Versions/A install name, 12 dlopen_preflight wrong for libSystem or libz"
case_env_native
case_flag_beats_env
case_last_flag_native
case_last_flag_cache
case_bad_mode
case_empty_mode
case_native_static
case_native_unbound
case_native_float
case_native_classic_bind
case_native_exit
case_native_constructors
case_sys sys_files "$SYS_FILES_BIN" "$SYS_FILES_ARM64" sys_files "$SYS_FILES_NEED" "" \
    "sys_files is the first case in which a guest's open, fcntl or ioctl crosses at all, so a failure here means native mode cannot open a file with a mode or read a descriptor's flags" \
    open "bit 0 is open with O_CREAT and O_EXCL failing, 1 the file not created with the mode 0640 the optional argument carried, 2 the write to it failing, 3 a second O_EXCL open not failing with EEXIST, 4 open without O_CREAT not reading the file back, 5 open\$NOCANCEL not reading it back, 6 O_TRUNC not emptying it or the rewrite failing, 7 open of a missing path not failing with ENOENT, 8 open\$NOCANCEL with O_CREAT in a missing directory not failing with ENOENT" \
    openat "bit 0 is the directory not opening, 1 openat with O_CREAT not creating the file with mode 0604, 2 the write to it failing, 3 openat\$NOCANCEL not reading it back, 4 a second O_EXCL openat not failing with EEXIST, 5 unlinkat failing" \
    fcntl "bit 0 is the file not reopening, 1 F_GETFL not answering O_RDWR without O_NONBLOCK, 2 F_SETFL not setting O_NONBLOCK, 3 fcntl\$NOCANCEL not clearing it again, 4 F_SETFD not setting FD_CLOEXEC, 5 F_DUPFD not handing out a descriptor at or above 50 without FD_CLOEXEC, 6 F_DUPFD_CLOEXEC not handing out one at or above 60 with it, 7 F_GETPATH, a pointer command, not writing the file's real path, 8 F_PREALLOCATE not filling in fst_bytesalloc, 9 F_NOCACHE failing, 10 F_FULLFSYNC, which takes no argument, failing, 11 fcntl on descriptor -1 not failing with EBADF" \
    lock "bit 0 is the file not opening twice, 1 F_SETLK failing, 2 F_GETLK against the process's own lock not answering F_UNLCK, 3 F_OFD_SETLK failing, 4 F_OFD_GETLK through the second open file description not seeing the first one's lock where it is, 5 a conflicting F_OFD_SETLK not failing with EAGAIN, 6 unlocking not letting it through, 7 F_SETLK not unlocking" \
    ioctl "bit 0 is the pipe or the write to it failing, 1 FIONREAD not answering 7, 2 FIONBIO not making the read end non-blocking, 3 the drained read end not failing with EAGAIN, 4 FIOCLEX, a request carrying no argument, not setting FD_CLOEXEC, 5 FIONCLEX not clearing it, 6 posix_openpt, grantpt or unlockpt failing, 7 the slave not opening, 8 TIOCSWINSZ failing, 9 TIOCGWINSZ not reading back 33 by 101, 10 TIOCGETA or tcgetattr failing, 11 ioctl on descriptor -1 not failing with EBADF" \
    sem_open "bit 0 is sem_open with O_CREAT failing, 1 the semaphore not starting at the value 3 its fourth argument carried, 2 a fourth trywait not failing with EAGAIN, 3 sem_post not raising it, 4 a second O_EXCL sem_open not failing with EEXIST, 5 sem_open without O_CREAT not finding the existing one, 6 sem_close failing, 7 sem_unlink failing, 8 sem_open of the unlinked name not failing with ENOENT" \
    shm_open "bit 0 is shm_open with O_CREAT failing, 1 ftruncate failing, 2 the object not 16384 bytes with the mode 0600 its third argument carried, 3 a second O_EXCL shm_open not failing with EEXIST, 4 shm_open without O_CREAT not finding it, 5 shm_unlink failing, 6 shm_open of the unlinked name not failing with ENOENT" \
    semctl "bit 0 is SETVAL, whose union carries an int, or GETVAL failing, 1 SETALL, whose union carries a pointer, failing, 2 GETALL not reading 7 and 9 back, 3 IPC_STAT not filling in two semaphores and the mode 0600, 4 IPC_SET failing, 5 IPC_STAT not seeing the new mode, 6 GETVAL on the second semaphore or GETNCNT wrong, 7 IPC_RMID failing, 8 GETVAL on the removed set not failing; ran=0 means semget itself failed on every side" \
    ulimit "bit 0 is UL_GETFSIZE not positive, 1 it disagreeing with RLIMIT_FSIZE in 512-byte blocks, 2 UL_SETFSIZE, whose second argument carries the new limit, not setting it, 3 an unknown command not failing with EINVAL"
case_sys sys_mmap "$SYS_MMAP_BIN" "$SYS_MMAP_ARM64" sys_mmap "$SYS_MMAP_NEED" "" \
    "sys_mmap is the case that fails if a guest's mapping escapes the memory ocerz tracks, or if code written into a mapping is not retranslated after the guest changes it" \
    anon "bit 0 is an anonymous mapping failing or not page-aligned, 1 it not zeroed, 2 a pattern not reading back, 3 a MAP_FIXED mapping over its second page not replacing exactly that page with zeros, 4 madvise failing, 5 munmap failing" \
    file "bit 0 is the file not written, 1 a private read-only mapping at an offset not showing the file's bytes, 2 its munmap failing, 3 a store into a private writable mapping reaching the file or not reading back, 4 its munmap failing, 5 msync of a shared mapping failing, 6 the shared store not reaching the file, 7 mlock or munlock failing, 8 its munmap failing" \
    code "bit 0 is the mapping failing, 1 code written into it, made executable and called not returning its value, 2 rewritten code not returning the new value, which is what a translation kept past the protection change looks like, 3 one of five more rewrites returning a stale value, 4 munmap failing, 5 code in a fresh mapping not returning its value" \
    fault "bit 0 is installing the SIGSEGV and SIGBUS handlers failing, 1 mprotect to PROT_NONE failing, 2 a read of the page not caught by the handler, which leaves by siglongjmp, 3 the page not readable again after mprotect, 4 a write to the read-only page not caught, 5 mprotect back to read-write failing, 6 the write not landing afterwards, 7 munmap failing" \
    mach "bit 0 is mach_vm_allocate failing, 1 its memory not zeroed, 2 mach_vm_protect to read-only failing or losing the data, 3 mach_vm_protect back to read-write failing, 4 mach_vm_deallocate failing, 5 vm_allocate failing, 6 vm_protect failing, 7 vm_deallocate failing" \
    host "bit 0 is posix_memalign failing, 1 mprotect of the host heap page to read-only failing, 2 mprotect back to read-write failing, 3 madvise of it failing, 4 task_threads failing, 5 vm_deallocate of the thread list it returned failing"
case_sys sys_jmp "$SYS_JMP_BIN" "$SYS_JMP_ARM64" sys_jmp "$SYS_JMP_NEED" "" \
    "sys_jmp is the case every interpreter and error-recovery library that uses setjmp depends on" \
    value "bit 0 is setjmp's first return not 0, 1 a longjmp from forty frames down not returning 42 there, 2 longjmp with 0 not returning 1, 3 longjmp with -7 not returning it or a volatile local not surviving, 4 a longjmp through a copy of the jmp_buf not arriving" \
    mask "bit 0 is longjmp not restoring the mask setjmp saved, 1 _longjmp restoring one, 2 siglongjmp not restoring the mask sigsetjmp(env, 1) saved, 3 siglongjmp restoring one sigsetjmp(env, 0) did not save, 4 longjmp through a sigjmp_buf that saved a mask not restoring it" \
    handler "bit 0 is sigaction failing, 1 three siglongjmps out of a SIGUSR1 handler not each arriving with the signal unblocked again, 2 two longjmps out of a handler for kill's SIGUSR1 not arriving with it unblocked, 3 sigaction failing, 4 siglongjmp of a buffer that saved no mask not leaving SIGUSR1 blocked as the handler had it" \
    altstack "bit 0 is sigaltstack failing, 1 sigaction failing, 2 three raises not each leaving the handler by siglongjmp, 3 the second or third handler not running on the alternate stack, which is what a jump that left the thread marked as still on it looks like, 4 sigaltstack after a jump still reporting SS_ONSTACK, 5 disabling the stack failing" \
    callback "bit 0 is a setjmp and longjmp inside a qsort comparator, within the same callback, not performed or the sort wrong" \
    x86 "bit 0 is longjmp not restoring MXCSR's rounding control, 1 the x87 control word, 2 division not rounding to nearest afterwards, 3 the direction flag not cleared"
case_sys sys_proc "$SYS_PROC_BIN" "$SYS_PROC_ARM64" sys_proc "$SYS_PROC_NEED" "$SYS_PROC_KINDS" \
    "sys_proc is the case that fails if a program native mode starts does not run under ocerz, or does not get the argv and environment the guest built" \
    fork "bit 0 is the pipe failing, 1 fork failing, 2 the child's line not arriving, 3 the child's own checks - getppid, qsort with a guest comparator, a pthread in the child, the heap - failing, 4 a second fork's child not exiting 17 to wait4, 5 vfork's child not exiting 7, 6 waitpid with no children left not failing with ECHILD" \
    spawn "bit 0 is the pipe failing, 1 posix_spawn_file_actions_init failing, 2 adddup2 failing, 3 addclose failing, 4 posix_spawn with file actions and an environment failing, 5 nothing arriving on the child's stdout, 6 the child not exiting 24, 7 posix_spawnattr_init failing, 8 setflags or setpgroup failing, 9 posix_spawn with attributes failing, 10 that child not exiting 23, 11 posix_spawnp not finding the fixture on PATH, 12 that child not exiting 23, 13 posix_spawnp of a missing name not answering ENOENT, 14 posix_spawn of a missing path not answering ENOENT, 15 posix_spawn of a #! script whose interpreter is the fixture failing, 16 the interpreter not exiting 24" \
    exec "bit 0 is execv, 1 execve with an environment, 2 execve with none, 3 execvp, 4 execvP, 5 execl, 6 execle, 7 execlp and 8 execl with ten arguments, six of them on the stack, not starting the child with the argv the report line shows, 9 execv of a missing path not failing with ENOENT, 10 execvp of a missing name not failing with ENOENT" \
    shell "bit 0 is system(NULL) not reporting a shell, 1 system(\"exit 3\") not returning 3, 2 system running the fixture not returning its exit 25, 3 a shell loop not returning 4, 4 popen for reading not delivering the child's line, 5 pclose not returning its exit 23, 6 popen for writing failing, 7 the sink child not counting 42 bytes, 8 popen r+ not accepting a line, 9 the shell not echoing it back, 10 pclose of it failing, 11 popen with a bad type not failing with EINVAL, 12 pclose of a stream popen never made not failing"
case_sys_jmp_refused
case_sys_fork_callback

echo "----------------------------------------"
echo "native tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
