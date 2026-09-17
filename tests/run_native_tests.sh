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
# The thread, tlv_*, signal_*, cf_* and M10 fixtures alone are built without
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
# The callback, attach, thread, tlv_*, signal_*, cf_* and M10 cases skip where
# there is no x86_64 clang, like the others, but a fixture of theirs that fails
# to compile where a trivial x86_64 program compiles fine is a failure: skipping
# it would hide a broken fixture indefinitely. So is a cf_* or M10 fixture whose
# arm64 build fails to compile where its x86_64 build did, since that leaves the
# case without its host oracle.
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
MEASURE_BIN=/usr/bin/time

unset OCERZ_MODE
unset OCERZ_BRIDGE_PROBE_UNSET
unset OCERZ_BRIDGELOG

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
    local bin="$1" imports allowed sym stray="" missing="" why
    shift
    imports="$(nm -u "$bin" 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
    if [ -z "$imports" ]; then
        return
    fi
    allowed=" $CF_BRIDGED $CF_EXPORTS $OBJC_BRIDGED $OBJC_EXPORTS $STACK_GUARD_SYM "
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
            *) why="the unfortified translation unit did not call the plain entry points" ;;
        esac
        echo "$(basename "$bin") imports '${imports% }' and not$missing, so $why, and the case proves nothing about it"
        return
    fi
    if [ -n "$stray" ]; then
        echo "$(basename "$bin") imports$stray, which neither the virtual libobjc, Foundation and CoreFoundation export nor the bridge implements, so a failure would be about those imports and not about Objective-C or formatting"
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
    elif grep -hF "$NOBIND" "$out" "$err" 2>/dev/null | grep -Fq -e " in $OBJC_LIB" -e " in $FOUNDATION_FRAMEWORK" -e " in $CF_FRAMEWORK"; then
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
    elif [ -n "$kind" ]; then
        want="$(grep -o ' lines=[0-9]*' "$out" 2>/dev/null | head -1 | cut -d= -f2)"
        got="$(objc_messages "$kind" "$tag" "$err" | wc -l | tr -d ' ')"
        if [ -z "$want" ] || [ "$got" != "$want" ]; then
            echo "the arm64 build, run directly on the host, left $got message lines on stderr where its status line says ${want:-nothing}, so the host is not writing them where the case looks, and every stderr comparison would pass on nothing"
        fi
    fi
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
    reason="$(objc_import_reason "$bin" $need)"
    if [ -n "$reason" ]; then
        record "$name" "$reason"
        return
    fi
    run_bounded "$ao" "$ae" "$arm"
    rc_arm=$?
    reason="$(objc_arm64_reason "$rc_arm" "$tag" "$ao" "$ae" "$kind" "$@")"
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

build_fixtures
build_callback_fixtures
build_attach_fixtures
build_thread_fixtures
build_tlv_fixtures
build_signal_fixtures
build_cf_fixtures
build_objc_fixtures

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

echo "----------------------------------------"
echo "native tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
