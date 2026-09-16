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
# all; native_unbound pins it with a two-line program calling getpwnam, which
# is deliberately outside the virtual library's export list. 72 means
# everything bound and the guest ran, and what is missing is the bridge behind
# one export rather than the export itself; xbench_dyn no longer reaches it, so
# bridge_unimpl pins it with a program whose only import is printf, which the
# virtual library exports and the bridge deliberately does not implement -- it
# is variadic, Apple's arm64 passes variadic arguments on the stack where x86-64
# passes them in registers, and no fixed signature can say where the named
# arguments stop. That fixture's one import used to be qsort, until M5 gave a
# callback a way back into guest code. Being the only import, it is also the
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
# The thread and tlv_* fixtures alone are built without -fno-stack-protector, and
# that is deliberate. clang emits the stack protector by default, and a protected
# x86_64 function reads ___stack_chk_guard, which is a data symbol rather than a
# function. Until M7a the virtual libSystem exported only functions, so a
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
# The callback, attach, thread and tlv_* cases skip where there is no x86_64
# clang, like the others, but a fixture of theirs that fails to compile where a
# trivial x86_64 program compiles fine is a failure: skipping it would hide a
# broken fixture indefinitely.
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
UNIMPL_SYM=_printf
BRIDGE_RE='^ocerz: bridge: [^ ]+ [^ ]+ not implemented$'
NOBIND='ocerz: native: no bridge for '
M0_SUMMARY='unresolved imports, no virtual frameworks are implemented yet'
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
int printf(const char *, ...);
int main(void)
{
    printf("unimpl %d\n", 72);
    return 0;
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

case_native_unbound() {
    local name=native_unbound rc reason="" src="$TMP/unbound.c" bin="$TMP/unbound"
    local out="$TMP/native_unbound.out" err="$TMP/native_unbound.err"
    cat > "$src" <<'EOC'
#include <pwd.h>
int main(void) { return getpwnam("root") != 0; }
EOC
    if ! clang -arch x86_64 -fno-stack-protector -o "$bin" "$src" >/dev/null 2>&1; then
        echo "SKIP $name (no x86_64 clang toolchain)"; return
    fi
    run_bounded "$out" "$err" "$OCERZ" -v -native "$bin"
    rc=$?
    if [ "$rc" -ne 71 ]; then
        reason="exit $rc, want 71"
    elif ! grep -Fq "${NOBIND}_getpwnam" "$out" "$err"; then
        reason="no 'no bridge for _getpwnam' line"
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
case_env_native
case_flag_beats_env
case_last_flag_native
case_last_flag_cache
case_bad_mode
case_empty_mode
case_native_static
case_native_unbound
case_native_float

echo "----------------------------------------"
echo "native tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
