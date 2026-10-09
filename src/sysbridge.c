/*
 * libSystem calls a generic crossing cannot make, answered by ocerz: the
 * variadic file and IPC calls, memory mapping, non-local jumps and processes.
 *
 * ---- variadic calls whose optional argument is fixed ----
 * open, openat, their $NOCANCEL forms, open_dprotected_np,
 * openat_dprotected_np, guarded_open_np and guarded_open_dprotected_np, fcntl and fcntl$NOCANCEL, ioctl, sem_open, shm_open,
 * semctl and ulimit are declared with an ellipsis, but what follows the named
 * arguments is not a list: it is one argument, or two for sem_open, whose
 * presence and type the named arguments decide.  x86-64 passes it in the next
 * integer register, where System V puts any next argument, and Apple's arm64
 * passes every variadic argument in an eight-byte stack slot, so no signature
 * can describe the call.  Each handler reads the optional argument out of the
 * register the guest left it in, and only when the call uses it, then calls the
 * host function from C through its real variadic prototype, so the compiler
 * lays the slot out the way the host's va_arg reads it.  The mode of the open
 * family is read only with O_CREAT, and O_TMPFILE where the SDK defines it,
 * and the host is otherwise passed 0, which is what Apple's own open hands the
 * kernel; sem_open reads its mode and initial value, and shm_open its mode, on
 * the same condition.  semctl reads its union semun, as a pointer for IPC_STAT,
 * IPC_SET, GETALL and SETALL and as the raw word otherwise, and ulimit its new
 * limit for UL_SETFSIZE.
 *
 * fcntl's argument is an int for most commands and a pointer for the ones
 * Apple's libc lists as taking one - the locks, including the open file
 * description ones, F_PREALLOCATE, F_PUNCHHOLE, F_SETSIZE, the advisory reads,
 * F_LOG2PHYS, the F_GETPATH family, the code-signature commands, F_TRANSCODEKEY,
 * F_TRIM_ACTIVE_FILE, F_SPECULATIVE_READ, F_CHECK_LV and F_ATTRIBUTION_TAG -
 * and only a pointer is translated.  The word is handed on as a pointer-sized
 * slot either way: the host reads an int command's argument as the low half of
 * it, which is exactly the int the guest passed, whatever x86 left in the upper
 * half.  ioctl's argument is always a pointer-sized word, as Apple's ioctl
 * reads it, and it is translated when the request encodes a direction, IOC_IN
 * or IOC_OUT, and handed on as it is for an _IO request that carries a value or
 * nothing.
 *
 * A structure a pointer command points at crosses unconverted, so its layout
 * has to be the same on both architectures.  Every one was checked by
 * compiling offsetof and sizeof for x86_64 and arm64 and comparing: struct
 * flock, struct flocktimeout, fstore_t, struct radvisory, struct log2phys,
 * fsignatures_t, fpunchhole_t, ftrimactivefile_t, fspecread_t, fchecklv_t,
 * fattributiontag_t, struct winsize, struct termios, struct semid_ds, struct
 * ipc_perm, union semun, struct ifreq and struct ifconf are byte for byte the
 * same, and the ioctl request numbers, which encode the structure's size, agree.
 * No command is refused for its layout, because none differs.  The ones whose
 * structure carries a pointer of its own, the signature blobs, F_RDADVISEV's
 * ranges and SIOCGIFCONF's buffer, are right only where a guest address is a
 * host address, which is the identity map a position-independent guest runs in.
 * F_RDADVISEV arrived with the macOS 27 SDK, so an older SDK - the one a
 * release runner builds with - gets it defined by its number (116) instead.
 *
 * These handlers raise a bridge frame around the host call, because the host
 * dereferences guest pointers there, and a fault is reported naming the call.
 *
 * ---- memory ----
 * mmap, munmap, mprotect and madvise, and mach_vm_allocate, mach_vm_deallocate
 * and mach_vm_protect with their vm_ spellings, which are the same calls on a
 * 64-bit task, go to the entry points src/syscall.c exports for exactly this.
 * Each is the body the cache-mode syscall or Mach trap runs, so guest memory is
 * handed out of the guest arena and accounted for in 4 KB pages, a mapping or
 * protection change retires the translations made from it, a page the guest
 * makes writable after running code from it is watched the way cache mode
 * watches it, and a fixed mapping at an address outside the arena registers it
 * the same way.  Native mode runs in the identity map, so the address the guest
 * gets back is the host address as well.  What native mode adds is memory the
 * syscall path never meets: the guest's heap is the host's, so a guest that
 * protects a page it got from posix_memalign, or deallocates the thread list
 * task_threads handed it, names a range ocerz does not track, and such a range
 * goes to the host kernel after its translations are dropped.  msync, mlock,
 * munlock, minherit and mincore stay ordinary crossings: the syscall layer's own
 * handling of them is to translate the pointer and forward the call, which in
 * the identity map is the crossing itself.  mach_vm_map and mach_vm_remap, with
 * their vm_ spellings, reach the host kernel once the translations of a fixed
 * target range are dropped.
 *
 * mach_vm_region and vm_region_64 are asked about the guest's own memory, and
 * the host kernel answers about host pages: 16 KB of them, which ocerz cannot
 * always give the protection the guest set on one 4 KB page of four.  Electron
 * protects a page read-only, asks mach_vm_region whether it is, and executes an
 * int3 when the answer is anything else, which killed Discord's renderers in
 * native mode as it once did in cache mode (src/syscall.c).  So after the host
 * answers a basic-info query about the process's own task, the region,
 * protection and maximum protection are taken from ocerz's own map when that
 * map has a region holding the address asked about, or else the first region
 * above it; the host's heap lies between ocerz's mappings in the identity map,
 * and a region of it that comes first keeps the host's answer.  The host's
 * region can start lower than ocerz's and still hold the address, since the
 * kernel merges neighbours of one protection that ocerz keeps apart.
 *
 * ---- non-local jumps ----
 * setjmp and its relatives save and restore machine state, and the host's would
 * save arm64 registers where the guest meant its x86 ones, so they work on the
 * guest's cpu.  The jmp_buf is Apple's x86_64 layout as its libplatform lays it
 * out, confirmed by running the x86_64 routines under Rosetta against a filled
 * buffer and reading their code: rbx at 0, rbp at 8, the caller's rsp at 16,
 * r12 through r15 at 24 to 48, the return rip at 56, MXCSR at 72, the x87
 * control word at 76, the signal mask at 80, sigsetjmp's savemask at 84 and the
 * alternate-stack flags at 88.  rbp, rsp and rip are XORed with the pointer
 * token at gs:0x38, as Apple's are; the thread blocks native mode builds hold
 * zero there, so they are stored as they are.  setjmp and sigsetjmp with a
 * nonzero savemask also save the guest's signal mask and whether it is on its
 * alternate stack, through the same entry points sigprocmask and sigaltstack
 * use, and longjmp and siglongjmp of such a buffer put both back, the second the
 * way _sigunaltstack tells the kernel.  A jump restores the callee-saved
 * registers, rsp and rip, reinitializes the x87 unit and loads the saved control
 * word and MXCSR, which sets the host's rounding mode too, clears the direction
 * flag, and returns the value, 1 in place of 0.  Any signal the restored mask
 * unblocks is delivered on top of the state the jump arrived at.  A jump out of
 * a signal handler is an ordinary jump: the handler runs on the same cpu at the
 * same level as the code it interrupted, and the frame it leaves behind on the
 * guest stack is simply abandoned, as it is natively.
 *
 * A jump may not leave native frames behind.  Guest code a native function
 * called back, a qsort comparator, a dispatch work function or a thread's start
 * routine, runs on the same host thread above that function's frames, and a
 * longjmp from there to a setjmp taken before the call would continue the guest
 * with the native frames still underneath it, holding whatever locks and state
 * they held, and with the callback's own run loop running code it was never
 * handed.  ocerz could only make that safe by unwinding the host side to the
 * crossing the setjmp was taken outside, which would skip the native function's
 * own cleanup exactly as the guest's jump skips its frames, and a native
 * function is not written to be abandoned.  So such a jump is refused by name,
 * naming the native call whose frames it would skip, and stops with 72.  To
 * know, setjmp writes the bridge level it runs at (bridge.h) into the unused
 * tail of the buffer at 104, behind a marker at 96, and a jump compares it with
 * the level it runs at: equal is performed, a level still open further out is
 * the refusal above, and anything else is a jump to a setjmp whose call has
 * returned or that another thread took, refused as well.  A buffer without the
 * marker was filled by something other than these setjmps and is jumped to at
 * the outermost level, where no native frames can be underneath, and refused
 * inside a callback, where they can.
 *
 * ---- processes ----
 * fork and vfork are the fork syscall's body, so ocerz's fork handlers run and
 * the child keeps only the forking thread's cpu and starts without translations,
 * and vfork is a fork, as it is in cache mode.  The JIT never calls them from
 * inside a block (vdylib.h), because the child cannot return into a translation
 * it did not inherit, and for the same reason a fork inside a callback is
 * refused by name when a translated block's frame lies underneath the callback
 * on the host stack; a fork a native thread's callback makes with no translated
 * frame below it, or one made from the outermost level, is performed.  The
 * child is a working native-mode process: the bridge, the callback bank and the
 * API database are memory it inherits whole, their locks are put back to their
 * initial state, and a thread it creates is given a guest personality the way
 * the parent's were.  Native mode installs ocerz's fork handlers before any guest
 * code runs, so a guest's pthread_atfork handlers, which are host handlers
 * calling back into the guest, run their prepare step before ocerz takes its
 * locks and their child step after ocerz has released them.
 *
 * execve, execv, execvp, execvP, execl, execle and execlp, and posix_spawn and
 * posix_spawnp, start the new program the way the syscalls do in cache mode,
 * through src/syscall.c: the program ocerz runs is ocerz itself, told with
 * -path which image to load and handed the guest's argv whole, argv[0]
 * included, and a script is started as its #! line's interpreter under ocerz
 * with the script's path after the interpreter's argument.  Its environment is
 * the guest's with OCERZ_MODE=native added, so the child comes up in native mode
 * too, and that holds for a child given no environment at all.  The execl forms
 * build argv out of their variadic arguments up to the null, execle takes the
 * environment after it, and the search forms walk PATH, or the path they are
 * given, the way Apple's libc does, trying each candidate and going on past the
 * errors it goes on past.  posix_spawn's file actions and attributes are the
 * host's own objects, since posix_spawn_file_actions_init and
 * posix_spawnattr_init are ordinary crossings; the actions are handed to the
 * host's posix_spawn as they are, and of the attributes, as in cache mode, only
 * the public flags, the default and blocked signal sets and the process group
 * are carried over, with the signals ocerz needs delivered taken out of the
 * blocked set.
 *
 * The policy for what the child is comes from cache mode unchanged: every
 * program the guest starts is started under ocerz.  A universal binary runs its
 * x86_64 slice, and a program with no x86_64 slice, arm64-only, is refused by
 * the child ocerz, which cannot read it, with status 65, exactly as a cache-mode
 * guest's is.  system and popen run the command with sh -c, where sh is
 * /bin/sh, as Apple's x86 libc does, so the shell is the x86_64 slice of the
 * system's /bin/sh running under ocerz in native mode, and a program the command
 * names is in turn started under ocerz.  system follows Apple's: SIGINT and
 * SIGQUIT are ignored in the guest's handler table while the command runs and
 * restored to the child's default if the guest had not ignored them, SIGCHLD is
 * blocked, and the status comes from wait4, 127 in the exit status when the
 * shell could not be started.  popen and pclose keep their own list of open
 * streams, each a host FILE over a pipe, or a socket pair for r+, and close
 * every other popen'd stream in the child, as Apple's do.
 *
 * ---- threads ----
 * A thread the guest creates is a host thread whose start routine is a
 * callback into the guest's, and its guest frames run on a stack ocerz makes
 * for it when it first enters guest code.  The host stack the attributes size
 * holds ocerz's own frames and the native frames of every bridged call
 * instead, and the size the guest asked for was chosen for its own frames:
 * Chromium makes one thread with a 16 KB stack, and the translator alone takes
 * 40 KB of host stack to translate a block, so that thread overflowed its
 * guard page the first time it ran a block nobody had translated.  A guest
 * asking for less than 512 KB, the size a secondary thread gets by default,
 * gets 512 KB of host stack; one that hands over stack memory of its own keeps
 * it.
 *
 * pthread_key_create and its siblings keep the guest's values in the guest
 * thread block, where an inlined gs-relative load finds them (the overrides
 * file explains why), and Darwin's reserved keys 10 to 255 live there too,
 * usable without being created, since the Swift runtime uses one that way;
 * pthread_key_init_np records a destructor for one, which runs with the
 * others when the thread ends.  dispatch_main ends the main thread in the real
 * libdispatch, which then drains the main queue from its thread pool.  Here the
 * main thread is the one the process belongs to, so the handler runs the
 * native CFRunLoopRun on it for good instead, which drains the main queue the
 * way an application's main thread does; the asynchronous main of a Swift
 * program ends there, and its exit comes from a job on that queue.  A call from
 * any other thread stops the process, as libdispatch's own check does.
 *
 * ---- 128-bit arithmetic ----
 * compiler-rt's __udivti3, __umodti3, __divti3, __modti3, __udivmodti4,
 * __clzti2 and the __fix conversions to 128-bit integers are what a compiler
 * calls for __int128 division and conversion, and no header declares them.
 * System V passes a 128-bit integer in two registers, low half first, and
 * returns one in RAX and RDX, so each is computed here from the guest's
 * registers.  Dividing by zero raises SIGFPE with FPE_INTDIV, as the real
 * routine's divide instruction does under Rosetta, and a guest with no handler
 * for it dies of it.  A conversion out of range saturates, as compiler-rt's
 * does: to the largest or smallest value by the sign, a NaN by its sign bit,
 * and to zero for an unsigned one below zero.
 *
 * ---- the floating-point environment ----
 * fenv.h's functions read and change the guest's x87 control and status words
 * and its MXCSR, which live in OcerzCPU, and x86 numbers its exception flags
 * and rounding modes differently from arm64, so none of them can be the host's
 * call: a bridged fesetround(FE_DOWNWARD) asked the host for an arm64 mode that
 * does not exist and failed, and the crossing put the host's mode back after it
 * anyway.  The handlers do what x86 libm does, as running it under Rosetta
 * shows: fegetround reads the x87 control word, fesetround sets both rounding
 * fields and hands an unknown mode back as its nonzero answer, feraiseexcept
 * sets inexact along with overflow or underflow the way the arithmetic it
 * performs does, fegetenv fills all sixteen bytes of the x86 fenv_t, and
 * feholdexcept masks every exception after saving.  The flags the guest's
 * arithmetic raised are where the host's arithmetic raised them, in FPSR,
 * since translated SSE and x87 instructions are arm64 floating-point ones, so
 * the handlers read and clear those along with the flags in OcerzCPU, which
 * ldmxcsr and the earlier handlers left there.  Arithmetic in a native call
 * raises them too, as x86 libm's own arithmetic would.  An exception the guest
 * unmasked does not trap.  _FE_DFL_ENV and _FE_DFL_DISABLE_SSE_DENORMS_ENV are
 * var records holding the x86 defaults (src/vdylib.c).
 */
#include "ocerz/sysbridge.h"
#include "ocerz/abi.h"
#include "ocerz/apidb.h"
#include "ocerz/bridge.h"
#include "ocerz/syscall.h"
#include "ocerz/vdylib.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/jit.h"
#include "ocerz/interp.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <paths.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <ulimit.h>
#include <unistd.h>
#include <wchar.h>
#include <arm_acle.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#ifndef F_RDADVISEV
#define F_RDADVISEV 116
#endif

extern char **environ;

int ocerz_host_open_nocancel(const char *path, int flags, ...) __asm__("_open$NOCANCEL");
int ocerz_host_openat_nocancel(int fd, const char *path, int flags, ...) __asm__("_openat$NOCANCEL");
int ocerz_host_fcntl_nocancel(int fd, int cmd, ...) __asm__("_fcntl$NOCANCEL");
extern int guarded_open_np(const char *path, const uint64_t *guard, unsigned guardflags, int flags, ...);
extern int guarded_open_dprotected_np(const char *path, const uint64_t *guard, unsigned guardflags, int flags,
                                      int dpclass, int dpflags, ...);

#define SB_LIB OCERZ_BRIDGE_LIBSYSTEM
#define SB_ARGV_MAX 256
#define SB_ENV_MAX 512

#define JB_RBX 0
#define JB_RBP 8
#define JB_RSP 16
#define JB_R12 24
#define JB_R13 32
#define JB_R14 40
#define JB_R15 48
#define JB_RIP 56
#define JB_MXCSR 72
#define JB_FPCW 76
#define JB_TSD_PTR_MUNGE 0x38

static uint64_t sb_arg(const OcerzCPU *cpu, int i)
{
    static const int regs[6] = { OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9 };
    if (i < 6)
        return cpu->gpr[regs[i]];
    return ocerz_ld(cpu->gpr[OCERZ_RSP] + 8 * (uint64_t)(i - 5), 8);
}

static void *sb_ptr(uint64_t g)
{
    return g ? ocerz_g2h(g) : NULL;
}

static int sb_ret(struct OcerzVM *vm, OcerzCPU *cpu, int64_t r)
{
    ocerz_bridge_return(cpu, (uint64_t)r);
    return ocerz_bridge_settle(vm, cpu);
}

static int sb_posix(struct OcerzVM *vm, OcerzCPU *cpu, int err)
{
    if (err) {
        errno = err;
        return sb_ret(vm, cpu, -1);
    }
    return sb_ret(vm, cpu, 0);
}

static __attribute__((noreturn)) void sb_refuse(const char *sym, const char *why)
{
    fprintf(stderr, "ocerz: bridge: %s %s %s\n", SB_LIB, sym, why);
    exit(OCERZ_BRIDGE_UNIMPL_EXIT);
}

static int sb_vector(uint64_t gv, char **out, int cap)
{
    int n = 0;
    uint64_t p;
    for (uint64_t at = gv; gv && n < cap - 1 && (p = ocerz_ld(at, 8)) != 0; at += 8)
        out[n++] = (char *)ocerz_g2h(p);
    out[n] = NULL;
    return n;
}

static int sb_open_mode(int flags, uint64_t raw)
{
    int creat = (flags & O_CREAT) != 0;
#if defined(O_TMPFILE)
    creat = creat || (flags & O_TMPFILE) == O_TMPFILE;
#endif
    return creat ? (int)raw : 0;
}

int ocerz_sys_open(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *path = sb_ptr(sb_arg(cpu, 0));
    int flags = (int)sb_arg(cpu, 1);
    int mode = sb_open_mode(flags, sb_arg(cpu, 2));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_open", "i(pii)", (const void *)open);
    int r = open(path, flags, mode);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_open_nocancel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *path = sb_ptr(sb_arg(cpu, 0));
    int flags = (int)sb_arg(cpu, 1);
    int mode = sb_open_mode(flags, sb_arg(cpu, 2));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_open$NOCANCEL", "i(pii)",
                       (const void *)ocerz_host_open_nocancel);
    int r = ocerz_host_open_nocancel(path, flags, mode);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_openat(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int fd = (int)sb_arg(cpu, 0);
    const char *path = sb_ptr(sb_arg(cpu, 1));
    int flags = (int)sb_arg(cpu, 2);
    int mode = sb_open_mode(flags, sb_arg(cpu, 3));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_openat", "i(ipii)", (const void *)openat);
    int r = openat(fd, path, flags, mode);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_openat_nocancel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int fd = (int)sb_arg(cpu, 0);
    const char *path = sb_ptr(sb_arg(cpu, 1));
    int flags = (int)sb_arg(cpu, 2);
    int mode = sb_open_mode(flags, sb_arg(cpu, 3));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_openat$NOCANCEL", "i(ipii)",
                       (const void *)ocerz_host_openat_nocancel);
    int r = ocerz_host_openat_nocancel(fd, path, flags, mode);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_open_dprotected_np(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *path = sb_ptr(sb_arg(cpu, 0));
    int flags = (int)sb_arg(cpu, 1);
    int cls = (int)sb_arg(cpu, 2);
    int dpflags = (int)sb_arg(cpu, 3);
    int mode = sb_open_mode(flags, sb_arg(cpu, 4));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_open_dprotected_np", "i(piiii)",
                       (const void *)open_dprotected_np);
    int r = open_dprotected_np(path, flags, cls, dpflags, mode);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_openat_dprotected_np(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int fd = (int)sb_arg(cpu, 0);
    const char *path = sb_ptr(sb_arg(cpu, 1));
    int flags = (int)sb_arg(cpu, 2);
    int cls = (int)sb_arg(cpu, 3);
    int dpflags = (int)sb_arg(cpu, 4);
    int mode = sb_open_mode(flags, sb_arg(cpu, 5));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_openat_dprotected_np", "i(ipiiii)",
                       (const void *)openat_dprotected_np);
    int r = openat_dprotected_np(fd, path, flags, cls, dpflags, mode);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_guarded_open_np(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *path = sb_ptr(sb_arg(cpu, 0));
    const uint64_t *guard = sb_ptr(sb_arg(cpu, 1));
    unsigned guardflags = (unsigned)sb_arg(cpu, 2);
    int flags = (int)sb_arg(cpu, 3);
    int mode = sb_open_mode(flags, sb_arg(cpu, 4));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_guarded_open_np", "i(ppuii)", (const void *)guarded_open_np);
    int r = guarded_open_np(path, guard, guardflags, flags, mode);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_guarded_open_dprotected_np(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *path = sb_ptr(sb_arg(cpu, 0));
    const uint64_t *guard = sb_ptr(sb_arg(cpu, 1));
    unsigned guardflags = (unsigned)sb_arg(cpu, 2);
    int flags = (int)sb_arg(cpu, 3);
    int cls = (int)sb_arg(cpu, 4);
    int dpflags = (int)sb_arg(cpu, 5);
    int mode = sb_open_mode(flags, sb_arg(cpu, 6));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_guarded_open_dprotected_np", "i(ppuiiii)",
                       (const void *)guarded_open_dprotected_np);
    int r = guarded_open_dprotected_np(path, guard, guardflags, flags, cls, dpflags, mode);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

typedef unsigned __int128 sb_u128;
typedef __int128 sb_i128;

static sb_u128 sb_arg128(const OcerzCPU *cpu, int first)
{
    return (sb_u128)sb_arg(cpu, first) | (sb_u128)sb_arg(cpu, first + 1) << 64;
}

static int sb_ret128(struct OcerzVM *vm, OcerzCPU *cpu, sb_u128 v)
{
    cpu->gpr[OCERZ_RDX] = (uint64_t)(v >> 64);
    return sb_ret(vm, cpu, (int64_t)(uint64_t)v);
}

static int sb_div128_zero(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t at = ocerz_ld(cpu->gpr[OCERZ_RSP], 8);
    cpu->gpr[OCERZ_RSP] += 8;
    cpu->rip = at;
    if (ocerz_signal_deliver(cpu, OCERZ_SIGFPE, at, OCERZ_FPE_INTDIV, 0))
        return ocerz_bridge_settle(vm, cpu);
    fprintf(stderr, "ocerz: guest divided a 128-bit integer by zero, near rip=%#llx\n", (unsigned long long)at);
    fflush(stderr);
    signal(SIGFPE, SIG_DFL);
    raise(SIGFPE);
    abort();
}

int ocerz_sys_udivti3(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_u128 a = sb_arg128(cpu, 0), b = sb_arg128(cpu, 2);
    return b ? sb_ret128(vm, cpu, a / b) : sb_div128_zero(vm, cpu);
}

int ocerz_sys_umodti3(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_u128 a = sb_arg128(cpu, 0), b = sb_arg128(cpu, 2);
    return b ? sb_ret128(vm, cpu, a % b) : sb_div128_zero(vm, cpu);
}

int ocerz_sys_udivmodti4(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_u128 a = sb_arg128(cpu, 0), b = sb_arg128(cpu, 2);
    uint64_t rem = sb_arg(cpu, 4);
    if (!b)
        return sb_div128_zero(vm, cpu);
    if (rem) {
        sb_u128 r = a % b;
        ocerz_st(rem, 8, (uint64_t)r);
        ocerz_st(rem + 8, 8, (uint64_t)(r >> 64));
    }
    return sb_ret128(vm, cpu, a / b);
}

static sb_i128 sb_i128_min(void)
{
    return (sb_i128)((sb_u128)1 << 127);
}

int ocerz_sys_divti3(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_i128 a = (sb_i128)sb_arg128(cpu, 0), b = (sb_i128)sb_arg128(cpu, 2);
    if (!b)
        return sb_div128_zero(vm, cpu);
    return sb_ret128(vm, cpu, (sb_u128)(b == -1 ? (sb_i128)(0 - (sb_u128)a) : a / b));
}

int ocerz_sys_modti3(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_i128 a = (sb_i128)sb_arg128(cpu, 0), b = (sb_i128)sb_arg128(cpu, 2);
    if (!b)
        return sb_div128_zero(vm, cpu);
    return sb_ret128(vm, cpu, (sb_u128)(b == -1 ? 0 : a % b));
}

int ocerz_sys_clzti2(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_u128 a = sb_arg128(cpu, 0);
    uint64_t hi = (uint64_t)(a >> 64), lo = (uint64_t)a;
    int n = hi ? __builtin_clzll(hi) : lo ? 64 + __builtin_clzll(lo) : 128;
    return sb_ret(vm, cpu, n);
}

static double sb_xmm0(const OcerzCPU *cpu, int single)
{
    if (single) {
        float f;
        uint32_t w = (uint32_t)cpu->xmm[0].lo;
        memcpy(&f, &w, sizeof f);
        return f;
    }
    double d;
    uint64_t w = cpu->xmm[0].lo;
    memcpy(&d, &w, sizeof d);
    return d;
}

static sb_u128 sb_fix_signed(double d)
{
    if (isnan(d))
        return signbit(d) ? (sb_u128)sb_i128_min() : (sb_u128)sb_i128_min() - 1;
    if (d >= 0x1p127)
        return (sb_u128)sb_i128_min() - 1;
    if (d < -0x1p127)
        return (sb_u128)sb_i128_min();
    return (sb_u128)(sb_i128)d;
}

static sb_u128 sb_fix_unsigned(double d)
{
    if (isnan(d))
        return signbit(d) ? 0 : ~(sb_u128)0;
    if (d < 0)
        return 0;
    if (d >= 0x1p128)
        return ~(sb_u128)0;
    return (sb_u128)d;
}

int ocerz_sys_fixdfti(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret128(vm, cpu, sb_fix_signed(sb_xmm0(cpu, 0)));
}

int ocerz_sys_fixsfti(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret128(vm, cpu, sb_fix_signed(sb_xmm0(cpu, 1)));
}

int ocerz_sys_fixunsdfti(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret128(vm, cpu, sb_fix_unsigned(sb_xmm0(cpu, 0)));
}

int ocerz_sys_fixunssfti(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret128(vm, cpu, sb_fix_unsigned(sb_xmm0(cpu, 1)));
}

#define SB_FE_ALL 0x3fu
#define SB_FE_INVALID 0x01u
#define SB_FE_OVERFLOW 0x08u
#define SB_FE_UNDERFLOW 0x10u
#define SB_FE_INEXACT 0x20u
#define SB_FE_ROUND 0xc00u
#define SB_FE_MASKS 0x1f80u

static const struct {
    uint32_t x86;
    uint64_t fpsr;
} g_sb_fe_flags[] = {
    { 0x01, 1u << 0 }, { 0x04, 1u << 1 }, { 0x08, 1u << 2 },
    { 0x10, 1u << 3 }, { 0x20, 1u << 4 }, { 0x02, 1u << 7 },
};

static uint32_t sb_fe_raised(const OcerzCPU *cpu)
{
    uint64_t fpsr = __arm_rsr64("fpsr");
    uint32_t f = (cpu->mxcsr | cpu->fsw) & SB_FE_ALL;
    for (size_t k = 0; k < sizeof g_sb_fe_flags / sizeof g_sb_fe_flags[0]; k++)
        if (fpsr & g_sb_fe_flags[k].fpsr)
            f |= g_sb_fe_flags[k].x86;
    return f;
}

static void sb_fe_clear(OcerzCPU *cpu, uint32_t e)
{
    uint64_t fpsr = __arm_rsr64("fpsr"), keep = fpsr;
    e &= SB_FE_ALL;
    for (size_t k = 0; k < sizeof g_sb_fe_flags / sizeof g_sb_fe_flags[0]; k++)
        if (e & g_sb_fe_flags[k].x86)
            keep &= ~g_sb_fe_flags[k].fpsr;
    if (keep != fpsr)
        __arm_wsr64("fpsr", keep);
    cpu->mxcsr &= ~e;
    cpu->fsw &= (uint16_t)~e;
}

static void sb_fe_raise(OcerzCPU *cpu, uint32_t e)
{
    e &= SB_FE_ALL;
    if (e & (SB_FE_OVERFLOW | SB_FE_UNDERFLOW))
        e |= SB_FE_INEXACT;
    cpu->mxcsr |= e;
}

static void sb_fe_store(OcerzCPU *cpu, uint64_t env)
{
    ocerz_st(env, 2, cpu->fcw);
    ocerz_st(env + 2, 2, (cpu->fsw & ~(7u << 11)) | (uint32_t)(cpu->ftop & 7) << 11);
    ocerz_st(env + 4, 4, cpu->mxcsr | sb_fe_raised(cpu));
    ocerz_st(env + 8, 8, 0);
}

static void sb_fe_load(OcerzCPU *cpu, uint64_t env)
{
    sb_fe_clear(cpu, SB_FE_ALL);
    cpu->fcw = (uint16_t)ocerz_ld(env, 2);
    cpu->fsw = (uint16_t)(ocerz_ld(env + 2, 2) & ~(7u << 11));
    cpu->mxcsr = (uint32_t)ocerz_ld(env + 4, 4);
    ocerz_apply_mxcsr_round(cpu->mxcsr);
}

int ocerz_sys_feclearexcept(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_fe_clear(cpu, (uint32_t)sb_arg(cpu, 0));
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_feraiseexcept(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_fe_raise(cpu, (uint32_t)sb_arg(cpu, 0));
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_fetestexcept(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret(vm, cpu, sb_fe_raised(cpu) & (uint32_t)sb_arg(cpu, 0));
}

int ocerz_sys_fegetexceptflag(struct OcerzVM *vm, OcerzCPU *cpu)
{
    ocerz_st(sb_arg(cpu, 0), 2, sb_fe_raised(cpu) & (uint32_t)sb_arg(cpu, 1));
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_fesetexceptflag(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t e = (uint32_t)sb_arg(cpu, 1) & SB_FE_ALL;
    uint32_t want = (uint32_t)ocerz_ld(sb_arg(cpu, 0), 2) & e;
    sb_fe_clear(cpu, e);
    cpu->mxcsr |= want;
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_fegetround(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret(vm, cpu, cpu->fcw & SB_FE_ROUND);
}

int ocerz_sys_fesetround(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int mode = (int)sb_arg(cpu, 0);
    if ((uint32_t)mode & ~SB_FE_ROUND)
        return sb_ret(vm, cpu, mode);
    cpu->fcw = (uint16_t)((cpu->fcw & ~SB_FE_ROUND) | (uint32_t)mode);
    cpu->mxcsr = (cpu->mxcsr & ~(3u << 13)) | (uint32_t)mode << 3;
    ocerz_apply_mxcsr_round(cpu->mxcsr);
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_fegetenv(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_fe_store(cpu, sb_arg(cpu, 0));
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_fesetenv(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_fe_load(cpu, sb_arg(cpu, 0));
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_feholdexcept(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_fe_store(cpu, sb_arg(cpu, 0));
    sb_fe_clear(cpu, SB_FE_ALL);
    cpu->fcw |= SB_FE_ALL;
    cpu->mxcsr |= SB_FE_MASKS;
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_feupdateenv(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t raised = sb_fe_raised(cpu);
    sb_fe_load(cpu, sb_arg(cpu, 0));
    cpu->mxcsr |= raised;
    return sb_ret(vm, cpu, 0);
}

static int sb_fcntl_takes_pointer(int cmd)
{
    switch (cmd) {
    case F_GETLK: case F_SETLK: case F_SETLKW: case F_SETLKWTIMEOUT: case F_GETLKPID:
    case F_OFD_GETLK: case F_OFD_SETLK: case F_OFD_SETLKW: case F_OFD_SETLKWTIMEOUT:
    case F_PREALLOCATE: case F_PUNCHHOLE: case F_SETSIZE: case F_RDADVISE: case F_RDADVISEV:
    case F_LOG2PHYS: case F_LOG2PHYS_EXT: case F_GETPATH: case F_GETPATH_NOFIRMLINK:
    case F_GETPATH_MTMINFO: case F_GETCODEDIR: case F_PATHPKG_CHECK: case F_ADDSIGS:
    case F_ADDFILESIGS: case F_ADDFILESIGS_FOR_DYLD_SIM: case F_ADDFILESIGS_RETURN:
    case F_ADDFILESIGS_INFO: case F_ADDFILESUPPL: case F_ADDSIGS_MAIN_BINARY: case F_FINDSIGS:
    case F_TRANSCODEKEY: case F_TRIM_ACTIVE_FILE: case F_SPECULATIVE_READ: case F_CHECK_LV:
    case F_GETSIGSINFO: case F_ATTRIBUTION_TAG:
        return 1;
    default:
        return 0;
    }
}

static void *sb_fcntl_arg(int cmd, uint64_t raw)
{
    return sb_fcntl_takes_pointer(cmd) ? sb_ptr(raw) : (void *)(uintptr_t)raw;
}

int ocerz_sys_fcntl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int fd = (int)sb_arg(cpu, 0);
    int cmd = (int)sb_arg(cpu, 1);
    void *arg = sb_fcntl_arg(cmd, sb_arg(cpu, 2));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_fcntl", "i(iip)", (const void *)fcntl);
    int r = fcntl(fd, cmd, arg);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_fcntl_nocancel(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int fd = (int)sb_arg(cpu, 0);
    int cmd = (int)sb_arg(cpu, 1);
    void *arg = sb_fcntl_arg(cmd, sb_arg(cpu, 2));
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_fcntl$NOCANCEL", "i(iip)",
                       (const void *)ocerz_host_fcntl_nocancel);
    int r = ocerz_host_fcntl_nocancel(fd, cmd, arg);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_ioctl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int fd = (int)sb_arg(cpu, 0);
    unsigned long request = (unsigned long)sb_arg(cpu, 1);
    uint64_t raw = sb_arg(cpu, 2);
    void *arg = (request & (IOC_IN | IOC_OUT)) ? sb_ptr(raw) : (void *)(uintptr_t)raw;
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_ioctl", "i(iLp)", (const void *)ioctl);
    int r = ioctl(fd, request, arg);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_sem_open(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *name = sb_ptr(sb_arg(cpu, 0));
    int oflag = (int)sb_arg(cpu, 1);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_sem_open", "p(piiu)", (const void *)sem_open);
    sem_t *r = (oflag & O_CREAT) ? sem_open(name, oflag, (int)sb_arg(cpu, 2), (unsigned)sb_arg(cpu, 3))
                                 : sem_open(name, oflag);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, (int64_t)(intptr_t)r);
}

int ocerz_sys_shm_open(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *name = sb_ptr(sb_arg(cpu, 0));
    int oflag = (int)sb_arg(cpu, 1);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_shm_open", "i(pii)", (const void *)shm_open);
    int r = (oflag & O_CREAT) ? shm_open(name, oflag, (int)sb_arg(cpu, 2)) : shm_open(name, oflag);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_semctl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int semid = (int)sb_arg(cpu, 0);
    int semnum = (int)sb_arg(cpu, 1);
    int cmd = (int)sb_arg(cpu, 2);
    uint64_t raw = sb_arg(cpu, 3);
    union semun u;
    memcpy(&u, &raw, sizeof u);
    if (cmd == IPC_STAT || cmd == IPC_SET)
        u.buf = sb_ptr(raw);
    else if (cmd == GETALL || cmd == SETALL)
        u.array = sb_ptr(raw);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_semctl", "i(iiip)", (const void *)semctl);
    int r = semctl(semid, semnum, cmd, u);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_ulimit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int cmd = (int)sb_arg(cpu, 0);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_ulimit", "l(il)", (const void *)ulimit);
    long r = cmd == UL_SETFSIZE ? ulimit(cmd, (long)sb_arg(cpu, 1)) : ulimit(cmd);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_mmap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t out = 0;
    int e = ocerz_guest_mmap(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1), (int)sb_arg(cpu, 2),
                             (int)sb_arg(cpu, 3), (int)sb_arg(cpu, 4), sb_arg(cpu, 5), &out);
    if (e) {
        errno = e;
        return sb_ret(vm, cpu, -1);
    }
    return sb_ret(vm, cpu, (int64_t)out);
}

int ocerz_sys_munmap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_posix(vm, cpu, ocerz_guest_munmap(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1)));
}

int ocerz_sys_mprotect(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_posix(vm, cpu, ocerz_guest_mprotect(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1),
                                                  (int)sb_arg(cpu, 2)));
}

int ocerz_sys_madvise(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_posix(vm, cpu, ocerz_guest_madvise(vm, cpu, sb_arg(cpu, 0), sb_arg(cpu, 1),
                                                 (int)sb_arg(cpu, 2)));
}

int ocerz_sys_vm_allocate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret(vm, cpu, ocerz_guest_vm_allocate(vm, cpu, (uint32_t)sb_arg(cpu, 0), sb_arg(cpu, 1),
                                                   sb_arg(cpu, 2), (int)sb_arg(cpu, 3)));
}

int ocerz_sys_vm_deallocate(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret(vm, cpu, ocerz_guest_vm_deallocate(vm, cpu, (uint32_t)sb_arg(cpu, 0),
                                                     sb_arg(cpu, 1), sb_arg(cpu, 2)));
}

int ocerz_sys_vm_protect(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret(vm, cpu, ocerz_guest_vm_protect(vm, cpu, (uint32_t)sb_arg(cpu, 0), sb_arg(cpu, 1),
                                                  sb_arg(cpu, 2), (int)sb_arg(cpu, 3),
                                                  (int)sb_arg(cpu, 4)));
}

static uint64_t sb_jmp_token(const OcerzCPU *cpu)
{
    uint64_t slot = cpu->gs_base + JB_TSD_PTR_MUNGE;
    return cpu->gs_base && ocerz_addr_readable(slot) ? ocerz_ld(slot, 8) : 0;
}

static void sb_jmp_save(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t env, int save_mask)
{
    uint64_t rsp = cpu->gpr[OCERZ_RSP];
    uint64_t tok = sb_jmp_token(cpu);
    ocerz_st(env + JB_RBX, 8, cpu->gpr[OCERZ_RBX]);
    ocerz_st(env + JB_RBP, 8, cpu->gpr[OCERZ_RBP] ^ tok);
    ocerz_st(env + JB_RSP, 8, (rsp + 8) ^ tok);
    ocerz_st(env + JB_R12, 8, cpu->gpr[OCERZ_R12]);
    ocerz_st(env + JB_R13, 8, cpu->gpr[OCERZ_R13]);
    ocerz_st(env + JB_R14, 8, cpu->gpr[OCERZ_R14]);
    ocerz_st(env + JB_R15, 8, cpu->gpr[OCERZ_R15]);
    ocerz_st(env + JB_RIP, 8, ocerz_ld(rsp, 8) ^ tok);
    ocerz_st(env + JB_MXCSR, 4, cpu->mxcsr);
    ocerz_st(env + JB_FPCW, 2, cpu->fcw);
    if (save_mask) {
        ocerz_guest_sigprocmask(vm, cpu, SIG_BLOCK, 0, env + OCERZ_JB_MASK);
        ocerz_st(env + OCERZ_JB_ONSSTACK, 4, ocerz_guest_altstack_flags(cpu));
    }
    ocerz_st(env + OCERZ_JB_OCERZ_MAGIC, 8, OCERZ_JB_MAGIC);
    ocerz_st(env + OCERZ_JB_OCERZ_LEVEL, 8, ocerz_bridge_level());
}

static void sb_jmp_check(uint64_t env, const char *sym)
{
    const struct OcerzBridgeFrame *cb = ocerz_bridge_callback_frame();
    const char *call = cb && cb->sym ? cb->sym : "native code";
    char why[512];
    if (ocerz_ld(env + OCERZ_JB_OCERZ_MAGIC, 8) != OCERZ_JB_MAGIC) {
        if (!cb)
            return;
        snprintf(why, sizeof why,
                 "was handed a jmp_buf no setjmp of ocerz's filled, inside a callback %s made, so it cannot"
                 " tell whether the jump would skip %s's native frames; refused", call, call);
        sb_refuse(sym, why);
    }
    uint64_t want = ocerz_ld(env + OCERZ_JB_OCERZ_LEVEL, 8);
    if (want == ocerz_bridge_level())
        return;
    for (const struct OcerzBridgeFrame *f = cb; f; f = f->around) {
        if (f->level == want) {
            snprintf(why, sizeof why,
                     "from inside a callback %s made, to a setjmp taken outside that call, would skip the"
                     " native frames of %s; refused", call, call);
            sb_refuse(sym, why);
        }
    }
    sb_refuse(sym, "to a setjmp whose call has returned, or that another thread took; refused");
}

static int sb_jmp(struct OcerzVM *vm, OcerzCPU *cpu, uint64_t env, uint32_t val, int restore_mask,
                  const char *sym)
{
    sb_jmp_check(env, sym);
    if (restore_mask) {
        ocerz_guest_sigprocmask(vm, cpu, SIG_SETMASK, env + OCERZ_JB_MASK, 0);
        ocerz_guest_set_onstack(cpu, (ocerz_ld(env + OCERZ_JB_ONSSTACK, 4) & SS_ONSTACK) != 0);
    }
    uint64_t tok = sb_jmp_token(cpu);
    cpu->gpr[OCERZ_RBX] = ocerz_ld(env + JB_RBX, 8);
    cpu->gpr[OCERZ_RBP] = ocerz_ld(env + JB_RBP, 8) ^ tok;
    cpu->gpr[OCERZ_R12] = ocerz_ld(env + JB_R12, 8);
    cpu->gpr[OCERZ_R13] = ocerz_ld(env + JB_R13, 8);
    cpu->gpr[OCERZ_R14] = ocerz_ld(env + JB_R14, 8);
    cpu->gpr[OCERZ_R15] = ocerz_ld(env + JB_R15, 8);
    cpu->rip = ocerz_ld(env + JB_RIP, 8) ^ tok;
    cpu->gpr[OCERZ_RSP] = ocerz_ld(env + JB_RSP, 8) ^ tok;
    cpu->fsw = 0;
    cpu->ftw = 0;
    cpu->ftop = 0;
    cpu->fcw = (uint16_t)ocerz_ld(env + JB_FPCW, 2);
    cpu->mxcsr = (uint32_t)ocerz_ld(env + JB_MXCSR, 4);
    ocerz_apply_mxcsr_round(cpu->mxcsr);
    cpu->rflags &= ~OCERZ_DF;
    cpu->gpr[OCERZ_RAX] = val ? val : 1;
    return ocerz_bridge_settle(vm, cpu);
}

int ocerz_sys_setjmp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_jmp_save(vm, cpu, sb_arg(cpu, 0), 1);
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys__setjmp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    sb_jmp_save(vm, cpu, sb_arg(cpu, 0), 0);
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_sigsetjmp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t env = sb_arg(cpu, 0);
    uint32_t savemask = (uint32_t)sb_arg(cpu, 1);
    ocerz_st(env + OCERZ_JB_SAVEMASK, 4, savemask);
    sb_jmp_save(vm, cpu, env, savemask != 0);
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_longjmp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_jmp(vm, cpu, sb_arg(cpu, 0), (uint32_t)sb_arg(cpu, 1), 1, "_longjmp");
}

int ocerz_sys__longjmp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_jmp(vm, cpu, sb_arg(cpu, 0), (uint32_t)sb_arg(cpu, 1), 0, "__longjmp");
}

int ocerz_sys_siglongjmp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t env = sb_arg(cpu, 0);
    return sb_jmp(vm, cpu, env, (uint32_t)sb_arg(cpu, 1),
                  ocerz_ld(env + OCERZ_JB_SAVEMASK, 4) != 0, "_siglongjmp");
}

static int sb_translated_frame_below(struct OcerzVM *vm)
{
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    uintptr_t bottom = top - pthread_get_stacksize_np(self);
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    for (int depth = 0; depth < 65536 && fp >= bottom && fp + 16 <= top && (fp & 7) == 0; depth++) {
        const uintptr_t *record = (const uintptr_t *)fp;
        if (ocerz_jit_pc_in_arena(vm, (const void *)record[1]))
            return 1;
        if (record[0] <= fp)
            break;
        fp = record[0];
    }
    return 0;
}

int ocerz_sys_fork(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const struct OcerzBridgeFrame *cb = ocerz_bridge_callback_frame();
    if (cb && sb_translated_frame_below(vm)) {
        char why[256];
        snprintf(why, sizeof why,
                 "was called inside a callback %s made from translated code, and the child would return"
                 " into a translation it does not inherit; refused", cb->sym ? cb->sym : "native code");
        sb_refuse("_fork", why);
    }
    int pid = 0;
    int e = ocerz_guest_fork(vm, cpu, &pid);
    if (e) {
        errno = e;
        return sb_ret(vm, cpu, -1);
    }
    return sb_ret(vm, cpu, pid);
}

static int sb_exec(struct OcerzVM *vm, OcerzCPU *cpu, const char *path, char *const *argv,
                   char *const *envp)
{
    return ocerz_guest_execve(vm, cpu, path, argv, envp);
}

typedef int (*SbTry)(void *ctx, const char *path, char *const *argv);

static int sb_path_search(const char *name, const char *search, char *const *argv, SbTry try,
                          void *ctx)
{
    if (!name)
        return EFAULT;
    int eacces = 0;
    char buf[PATH_MAX];
    const char *bp = name;
    char *cur = NULL, *copy = NULL;
    int single = strchr(name, '/') != NULL;
    if (!single) {
        if (*name == '\0')
            return ENOENT;
        copy = strdup(search ? search : _PATH_DEFPATH);
        if (!copy)
            return ENOMEM;
        cur = copy;
    }
    int err = ENOENT;
    for (;;) {
        if (!single) {
            const char *p = strsep(&cur, ":");
            if (!p)
                break;
            size_t lp = *p ? strlen(p) : 1, ln = strlen(name);
            if (!*p)
                p = ".";
            if (lp + ln + 2 > sizeof buf) {
                err = ENAMETOOLONG;
                continue;
            }
            memcpy(buf, p, lp);
            buf[lp] = '/';
            memcpy(buf + lp + 1, name, ln);
            buf[lp + ln + 1] = '\0';
            bp = buf;
        }
        err = try(ctx, bp, argv);
        switch (err) {
        case 0:
        case E2BIG:
        case ENOMEM:
        case ETXTBSY:
            free(copy);
            return err;
        case ELOOP:
        case ENAMETOOLONG:
        case ENOENT:
        case ENOTDIR:
            break;
        case ENOEXEC: {
            int cnt = 0;
            while (argv && argv[cnt])
                cnt++;
            char **memp = calloc((size_t)cnt + 2, sizeof *memp);
            if (!memp) {
                free(copy);
                return ENOEXEC;
            }
            memp[0] = (char *)"sh";
            memp[1] = (char *)bp;
            for (int k = 1; k < cnt; k++)
                memp[k + 1] = argv[k];
            err = try(ctx, _PATH_BSHELL, memp);
            free(memp);
            free(copy);
            return err;
        }
        default: {
            struct stat sb;
            if (stat(bp, &sb) != 0)
                break;
            if (err == EACCES) {
                eacces = 1;
                break;
            }
            free(copy);
            return err;
        }
        }
        if (single)
            break;
    }
    free(copy);
    return eacces ? EACCES : ENOENT;
}

typedef struct SbExecCtx {
    struct OcerzVM *vm;
    OcerzCPU *cpu;
    char *const *envp;
} SbExecCtx;

static int sb_try_exec(void *ctx, const char *path, char *const *argv)
{
    SbExecCtx *c = ctx;
    return sb_exec(c->vm, c->cpu, path, argv, c->envp);
}

static int sb_exec_search(struct OcerzVM *vm, OcerzCPU *cpu, const char *name, const char *search,
                          char *const *argv)
{
    SbExecCtx c = { vm, cpu, environ };
    return sb_path_search(name, search, argv, sb_try_exec, &c);
}

int ocerz_sys_execve(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char *argv[SB_ARGV_MAX], *envp[SB_ENV_MAX];
    uint64_t gargv = sb_arg(cpu, 1), genv = sb_arg(cpu, 2);
    sb_vector(gargv, argv, SB_ARGV_MAX);
    sb_vector(genv, envp, SB_ENV_MAX);
    return sb_posix(vm, cpu, sb_exec(vm, cpu, sb_ptr(sb_arg(cpu, 0)), gargv ? argv : NULL,
                                     genv ? envp : NULL));
}

int ocerz_sys_execv(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char *argv[SB_ARGV_MAX];
    uint64_t gargv = sb_arg(cpu, 1);
    sb_vector(gargv, argv, SB_ARGV_MAX);
    return sb_posix(vm, cpu, sb_exec(vm, cpu, sb_ptr(sb_arg(cpu, 0)), gargv ? argv : NULL, environ));
}

int ocerz_sys_execvp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char *argv[SB_ARGV_MAX];
    sb_vector(sb_arg(cpu, 1), argv, SB_ARGV_MAX);
    return sb_posix(vm, cpu, sb_exec_search(vm, cpu, sb_ptr(sb_arg(cpu, 0)), getenv("PATH"), argv));
}

int ocerz_sys_execvP(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char *argv[SB_ARGV_MAX];
    sb_vector(sb_arg(cpu, 2), argv, SB_ARGV_MAX);
    const char *search = sb_ptr(sb_arg(cpu, 1));
    return sb_posix(vm, cpu, sb_exec_search(vm, cpu, sb_ptr(sb_arg(cpu, 0)),
                                            search ? search : _PATH_DEFPATH, argv));
}

static int sb_list_argv(const OcerzCPU *cpu, int first, char **out, int cap, int *after)
{
    int n = 0, i = first;
    uint64_t p;
    while ((p = sb_arg(cpu, i++)) != 0) {
        if (n >= cap - 1)
            return E2BIG;
        out[n++] = (char *)ocerz_g2h(p);
    }
    out[n] = NULL;
    if (after)
        *after = i;
    return 0;
}

int ocerz_sys_execl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char *argv[SB_ARGV_MAX];
    int e = sb_list_argv(cpu, 1, argv, SB_ARGV_MAX, NULL);
    if (e)
        return sb_posix(vm, cpu, e);
    return sb_posix(vm, cpu, sb_exec(vm, cpu, sb_ptr(sb_arg(cpu, 0)), argv, environ));
}

int ocerz_sys_execle(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char *argv[SB_ARGV_MAX], *envp[SB_ENV_MAX];
    int after = 0;
    int e = sb_list_argv(cpu, 1, argv, SB_ARGV_MAX, &after);
    if (e)
        return sb_posix(vm, cpu, e);
    uint64_t genv = sb_arg(cpu, after);
    sb_vector(genv, envp, SB_ENV_MAX);
    return sb_posix(vm, cpu, sb_exec(vm, cpu, sb_ptr(sb_arg(cpu, 0)), argv, genv ? envp : NULL));
}

int ocerz_sys_execlp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    char *argv[SB_ARGV_MAX];
    int e = sb_list_argv(cpu, 1, argv, SB_ARGV_MAX, NULL);
    if (e)
        return sb_posix(vm, cpu, e);
    return sb_posix(vm, cpu, sb_exec_search(vm, cpu, sb_ptr(sb_arg(cpu, 0)), getenv("PATH"), argv));
}

typedef struct SbSpawnCtx {
    struct OcerzVM *vm;
    OcerzCPU *cpu;
    int pid;
    const posix_spawn_file_actions_t *fa;
    const posix_spawnattr_t *attr;
    char *const *envp;
} SbSpawnCtx;

static int sb_try_spawn(void *ctx, const char *path, char *const *argv)
{
    SbSpawnCtx *c = ctx;
    return ocerz_guest_posix_spawn(c->vm, c->cpu, &c->pid, path, c->fa, c->attr, argv, c->envp);
}

static int sb_spawn(struct OcerzVM *vm, OcerzCPU *cpu, int search)
{
    char *argv[SB_ARGV_MAX], *envp[SB_ENV_MAX];
    uint64_t gpid = sb_arg(cpu, 0), gargv = sb_arg(cpu, 4), genv = sb_arg(cpu, 5);
    sb_vector(gargv, argv, SB_ARGV_MAX);
    sb_vector(genv, envp, SB_ENV_MAX);
    SbSpawnCtx c = { vm, cpu, 0, sb_ptr(sb_arg(cpu, 2)), sb_ptr(sb_arg(cpu, 3)), genv ? envp : NULL };
    const char *path = sb_ptr(sb_arg(cpu, 1));
    int e = search ? sb_path_search(path, getenv("PATH"), gargv ? argv : NULL, sb_try_spawn, &c)
                   : sb_try_spawn(&c, path, gargv ? argv : NULL);
    if (e == 0 && gpid)
        ocerz_st(gpid, 4, (uint32_t)c.pid);
    return sb_ret(vm, cpu, e);
}

int ocerz_sys_posix_spawn(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_spawn(vm, cpu, 0);
}

int ocerz_sys_posix_spawnp(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_spawn(vm, cpu, 1);
}

static pthread_mutex_t g_sb_system_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_sb_system_count;
static uint8_t g_sb_intact[16], g_sb_quitact[16];

static void sb_ignore(struct OcerzVM *vm, OcerzCPU *cpu, int sig, uint64_t scratch, uint8_t *saved,
                      sigset_t *defaults, short *flags)
{
    uint64_t act = scratch, oact = scratch + 16;
    ocerz_st(act, 8, (uint64_t)(uintptr_t)SIG_IGN);
    ocerz_st(act + 8, 4, 0);
    ocerz_st(act + 12, 4, 0);
    ocerz_guest_sigaction_user(vm, cpu, sig, act, oact);
    memcpy(saved, ocerz_g2h(oact), 16);
    if (ocerz_ld(oact, 8) != (uint64_t)(uintptr_t)SIG_IGN) {
        sigaddset(defaults, sig);
        *flags |= POSIX_SPAWN_SETSIGDEF;
    }
}

static void sb_restore(struct OcerzVM *vm, OcerzCPU *cpu, int sig, uint64_t scratch, const uint8_t *saved)
{
    memcpy(ocerz_g2h(scratch), saved, 16);
    ocerz_guest_sigaction_user(vm, cpu, sig, scratch, 0);
}

int ocerz_sys_system(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t gcmd = sb_arg(cpu, 0);
    if (!gcmd)
        return sb_ret(vm, cpu, access(_PATH_BSHELL, F_OK) == -1 ? 0 : 1);
    uint64_t scratch = (cpu->gpr[OCERZ_RSP] - 128 - 64) & ~0xfull;
    uint64_t set = scratch + 32, oset = scratch + 48;
    sigset_t defaults;
    sigemptyset(&defaults);
    short flags = POSIX_SPAWN_SETSIGMASK;

    pthread_mutex_lock(&g_sb_system_lock);
    if (g_sb_system_count++ == 0) {
        sb_ignore(vm, cpu, SIGINT, scratch, g_sb_intact, &defaults, &flags);
        sb_ignore(vm, cpu, SIGQUIT, scratch, g_sb_quitact, &defaults, &flags);
    }
    pthread_mutex_unlock(&g_sb_system_lock);
    ocerz_st(set, 4, 1u << (SIGCHLD - 1));
    ocerz_guest_sigprocmask(vm, cpu, SIG_BLOCK, set, oset);
    sigset_t old = (sigset_t)ocerz_ld(oset, 4);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setsigmask(&attr, &old);
    if (flags & POSIX_SPAWN_SETSIGDEF)
        posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, flags);
    char *argv[] = { (char *)"sh", (char *)"-c", (char *)ocerz_g2h(gcmd), NULL };
    int pid = 0;
    int err = ocerz_guest_posix_spawn(vm, cpu, &pid, _PATH_BSHELL, NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);

    int pstat;
    if (err == 0) {
        pid_t w;
        do {
            w = wait4(pid, &pstat, 0, NULL);
        } while (w == -1 && errno == EINTR);
        if (w == -1)
            pstat = -1;
    } else if (err == ENOMEM || err == EAGAIN) {
        pstat = -1;
    } else {
        pstat = W_EXITCODE(127, 0);
    }
    int saved_errno = errno;

    pthread_mutex_lock(&g_sb_system_lock);
    if (--g_sb_system_count == 0) {
        sb_restore(vm, cpu, SIGINT, scratch, g_sb_intact);
        sb_restore(vm, cpu, SIGQUIT, scratch, g_sb_quitact);
    }
    pthread_mutex_unlock(&g_sb_system_lock);
    ocerz_guest_sigprocmask(vm, cpu, SIG_SETMASK, oset, 0);
    errno = saved_errno;
    return sb_ret(vm, cpu, pstat);
}

typedef struct SbPopen {
    struct SbPopen *next;
    FILE *fp;
    pid_t pid;
} SbPopen;

static SbPopen *g_sb_popen;
static pthread_mutex_t g_sb_popen_lock = PTHREAD_MUTEX_INITIALIZER;

static FILE *sb_popen(struct OcerzVM *vm, OcerzCPU *cpu, const char *command, const char *type)
{
    int twoway = 0;
    if (!type) {
        errno = EINVAL;
        return NULL;
    }
    if (strchr(type, '+')) {
        twoway = 1;
        type = "r+";
    } else if ((*type != 'r' && *type != 'w') || type[1]) {
        errno = EINVAL;
        return NULL;
    }
    SbPopen *cur = malloc(sizeof *cur);
    if (!cur)
        return NULL;
    int pdes[2];
    if ((twoway ? socketpair(AF_UNIX, SOCK_STREAM, 0, pdes) : pipe(pdes)) < 0) {
        free(cur);
        return NULL;
    }
    FILE *iop;
    int other;
    if (*type == 'r') {
        iop = fdopen(pdes[0], type);
        other = pdes[1];
    } else {
        iop = fdopen(pdes[1], type);
        other = pdes[0];
    }
    if (!iop) {
        int e = errno;
        close(pdes[0]);
        close(pdes[1]);
        free(cur);
        errno = e;
        return NULL;
    }
    posix_spawn_file_actions_t fa;
    int err = posix_spawn_file_actions_init(&fa);
    if (err) {
        fclose(iop);
        close(other);
        free(cur);
        errno = err;
        return NULL;
    }
    if (*type == 'r') {
        if (pdes[1] != STDOUT_FILENO) {
            posix_spawn_file_actions_adddup2(&fa, pdes[1], STDOUT_FILENO);
            posix_spawn_file_actions_addclose(&fa, pdes[1]);
            if (twoway)
                posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDIN_FILENO);
        } else if (twoway && pdes[1] != STDIN_FILENO) {
            posix_spawn_file_actions_adddup2(&fa, pdes[1], STDIN_FILENO);
        }
        posix_spawn_file_actions_addclose(&fa, pdes[0]);
    } else {
        if (pdes[0] != STDIN_FILENO) {
            posix_spawn_file_actions_adddup2(&fa, pdes[0], STDIN_FILENO);
            posix_spawn_file_actions_addclose(&fa, pdes[0]);
        }
        posix_spawn_file_actions_addclose(&fa, pdes[1]);
    }
    pthread_mutex_lock(&g_sb_popen_lock);
    for (SbPopen *p = g_sb_popen; p; p = p->next)
        posix_spawn_file_actions_addclose(&fa, fileno(p->fp));
    pthread_mutex_unlock(&g_sb_popen_lock);

    char *argv[] = { (char *)"sh", (char *)"-c", (char *)command, NULL };
    int pid = 0;
    err = ocerz_guest_posix_spawn(vm, cpu, &pid, _PATH_BSHELL, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err == ENOMEM || err == EAGAIN) {
        fclose(iop);
        close(other);
        free(cur);
        errno = err;
        return NULL;
    } else if (err != 0) {
        pid = -1;
    }
    if (*type == 'r')
        close(pdes[1]);
    else
        close(pdes[0]);
    cur->fp = iop;
    cur->pid = pid;
    pthread_mutex_lock(&g_sb_popen_lock);
    cur->next = g_sb_popen;
    g_sb_popen = cur;
    pthread_mutex_unlock(&g_sb_popen_lock);
    fwide(iop, -1);
    return iop;
}

int ocerz_sys_popen(struct OcerzVM *vm, OcerzCPU *cpu)
{
    FILE *iop = sb_popen(vm, cpu, sb_ptr(sb_arg(cpu, 0)), sb_ptr(sb_arg(cpu, 1)));
    return sb_ret(vm, cpu, iop ? (int64_t)ocerz_h2g(iop) : 0);
}

int ocerz_sys_pclose(struct OcerzVM *vm, OcerzCPU *cpu)
{
    FILE *iop = sb_ptr(sb_arg(cpu, 0));
    SbPopen *cur, **link;
    pthread_mutex_lock(&g_sb_popen_lock);
    for (link = &g_sb_popen; (cur = *link) != NULL; link = &cur->next)
        if (cur->fp == iop)
            break;
    if (cur)
        *link = cur->next;
    pthread_mutex_unlock(&g_sb_popen_lock);
    if (!cur)
        return sb_ret(vm, cpu, -1);
    fclose(iop);
    if (cur->pid < 0) {
        free(cur);
        return sb_ret(vm, cpu, W_EXITCODE(127, 0));
    }
    int pstat = 0;
    pid_t pid;
    do {
        pid = wait4(cur->pid, &pstat, 0, NULL);
    } while (pid == -1 && errno == EINTR);
    free(cur);
    return sb_ret(vm, cpu, pid == -1 ? -1 : pstat);
}

#define SB_KEY_FIRST 256u
#define SB_KEY_END 768u
#define SB_KEY_ROUNDS 4
#define SB_RESERVED_FIRST 10u

typedef struct SbKey {
    _Atomic int used;
    _Atomic uint64_t destructor;
} SbKey;

static SbKey g_sb_keys[SB_KEY_END - SB_KEY_FIRST];
static SbKey g_sb_reserved[SB_KEY_FIRST];
static unsigned g_sb_key_next;
static pthread_mutex_t g_sb_key_lock = PTHREAD_MUTEX_INITIALIZER;

static SbKey *sb_key(uint64_t key)
{
    if (key >= SB_RESERVED_FIRST && key < SB_KEY_FIRST)
        return &g_sb_reserved[key];
    if (key < SB_KEY_FIRST || key >= SB_KEY_END)
        return NULL;
    SbKey *k = &g_sb_keys[key - SB_KEY_FIRST];
    return k->used ? k : NULL;
}

int ocerz_sys_pthread_key_create(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t out = sb_arg(cpu, 0), destructor = sb_arg(cpu, 1);
    unsigned n = SB_KEY_END - SB_KEY_FIRST, got = n;
    pthread_mutex_lock(&g_sb_key_lock);
    for (unsigned i = 0; i < n && got == n; i++) {
        unsigned at = (g_sb_key_next + i) % n;
        if (!g_sb_keys[at].used)
            got = at;
    }
    if (got != n) {
        g_sb_keys[got].destructor = destructor;
        g_sb_keys[got].used = 1;
        g_sb_key_next = got + 1;
    }
    pthread_mutex_unlock(&g_sb_key_lock);
    if (got == n)
        return sb_ret(vm, cpu, EAGAIN);
    if (cpu->gs_base)
        ocerz_st(cpu->gs_base + 8 * (uint64_t)(SB_KEY_FIRST + got), 8, 0);
    ocerz_st(out, 8, SB_KEY_FIRST + got);
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_pthread_key_init_np(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t key = (uint64_t)(uint32_t)sb_arg(cpu, 0), destructor = sb_arg(cpu, 1);
    if (key < SB_RESERVED_FIRST || key >= SB_KEY_FIRST)
        return sb_ret(vm, cpu, EINVAL);
    g_sb_reserved[key].destructor = destructor;
    g_sb_reserved[key].used = 1;
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_pthread_key_delete(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t which = sb_arg(cpu, 0);
    SbKey *k = which >= SB_KEY_FIRST ? sb_key(which) : NULL;
    if (!k)
        return sb_ret(vm, cpu, EINVAL);
    k->used = 0;
    k->destructor = 0;
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_pthread_setspecific(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t key = sb_arg(cpu, 0);
    if (!sb_key(key) || !cpu->gs_base)
        return sb_ret(vm, cpu, EINVAL);
    ocerz_st(cpu->gs_base + 8 * key, 8, sb_arg(cpu, 1));
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_pthread_getspecific(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t key = sb_arg(cpu, 0);
    uint64_t v = sb_key(key) && cpu->gs_base ? ocerz_ld(cpu->gs_base + 8 * key, 8) : 0;
    return sb_ret(vm, cpu, (int64_t)v);
}

static void sb_key_destructors(struct OcerzVM *vm, OcerzCPU *cpu)
{
    static int off = -1;
    if (off < 0)
        off = getenv("OCERZ_NO_TSD_DTORS") ? 1 : 0;
    if (off || !vm || !cpu || !cpu->gs_base)
        return;
    for (int round = 0; round < SB_KEY_ROUNDS; round++) {
        int ran = 0;
        for (unsigned key = SB_RESERVED_FIRST; key < SB_KEY_END && !vm->exited; key++) {
            SbKey *k = key < SB_KEY_FIRST ? &g_sb_reserved[key] : &g_sb_keys[key - SB_KEY_FIRST];
            if (key < SB_KEY_FIRST && !k->used)
                continue;
            uint64_t destructor = k->used ? k->destructor : 0;
            uint64_t slot = cpu->gs_base + 8 * (uint64_t)key;
            uint64_t value = ocerz_ld(slot, 8);
            if (!value)
                continue;
            ocerz_st(slot, 8, 0);
            if (!destructor)
                continue;
            uint64_t args[1] = { value };
            ocerz_vm_call(vm, destructor, args, 1, (cpu->gpr[OCERZ_RSP] - 256) & ~0xfull);
            ran = 1;
        }
        if (!ran)
            break;
    }
}

typedef struct SbThreadStart {
    void *(*entry)(void *);
    void *arg;
} SbThreadStart;

static void *sb_thread_main(void *p)
{
    SbThreadStart start = *(SbThreadStart *)p;
    free(p);
    void *result = start.entry(start.arg);
    OcerzCPU *cpu = ocerz_vm_current_cpu();
    if (!cpu)
        cpu = ocerz_thread_attach(ocerz_vm_process());
    if (cpu)
        sb_key_destructors(cpu->vm, cpu);
    return result;
}

int ocerz_sys_pthread_create(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t out = sb_arg(cpu, 0), attr = sb_arg(cpu, 1), routine = sb_arg(cpu, 2), arg = sb_arg(cpu, 3);
    SbThreadStart *start = malloc(sizeof *start);
    void *entry = routine ? ocerz_abi_callback_intern(routine, "p(p)") : NULL;
    if (!start || !entry) {
        free(start);
        return sb_ret(vm, cpu, routine ? EAGAIN : EINVAL);
    }
    start->entry = (void *(*)(void *))entry;
    start->arg = sb_ptr(arg);
    /*
     * The thread runs ocerz's translator and the guest on one stack, and
     * translation alone can outgrow a small guest-sized one (a Unity worker
     * overflowed its guard page mid-translation).  Unless the guest brought
     * its own stack memory, the host thread gets at least SB_MIN_STACK, which
     * costs address space only.
     */
    enum { SB_MIN_STACK = 4 << 20 };
    pthread_attr_t a;
    const pthread_attr_t *given = (const pthread_attr_t *)sb_ptr(attr);
    void *own_stack = NULL;
    size_t size = 0;
    if (given)
        memcpy(&a, given, sizeof a);
    else
        pthread_attr_init(&a);
    pthread_attr_getstackaddr(&a, &own_stack);
    pthread_attr_getstacksize(&a, &size);
    if (!own_stack && size < SB_MIN_STACK)
        pthread_attr_setstacksize(&a, SB_MIN_STACK);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_pthread_create", "i(ppc{p(p)}p)", (const void *)pthread_create);
    pthread_t made = NULL;
    int rc = pthread_create(&made, &a, sb_thread_main, start);
    ocerz_bridge_lower(&outer);
    if (!given)
        pthread_attr_destroy(&a);
    if (rc)
        free(start);
    else if (out)
        ocerz_st(out, 8, (uint64_t)(uintptr_t)made);
    return sb_ret(vm, cpu, rc);
}

int ocerz_sys_pthread_exit(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *value = sb_ptr(sb_arg(cpu, 0));
    /* The guest's pthread_cleanup_push is a macro that links its handler onto
       pthread_self()->__cleanup_stack, and pthread_self is the host's, so the
       host's pthread_exit would call those x86 routines as arm64 code (Mono's
       Boehm GC_thread_exit_proc when a Unity game quits).  They run as guest
       code first, innermost first, as Darwin runs them before the key
       destructors. */
    struct __darwin_pthread_handler_rec **stack = &pthread_self()->__cleanup_stack;
    while (*stack && !vm->exited) {
        uint64_t rec = (uint64_t)(uintptr_t)*stack;
        uint64_t routine = ocerz_ld(rec, 8), arg = ocerz_ld(rec + 8, 8);
        if (!ocerz_abi_is_guest_code(routine))
            break;
        *stack = (struct __darwin_pthread_handler_rec *)(uintptr_t)ocerz_ld(rec + 16, 8);
        uint64_t args[1] = { arg };
        ocerz_vm_call(vm, routine, args, 1, (cpu->gpr[OCERZ_RSP] - 256) & ~0xfull);
    }
    sb_key_destructors(vm, cpu);
    pthread_exit(value);
}

int ocerz_sys_glob(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int flags = (int)sb_arg(cpu, 1);
    uint64_t errfunc = sb_arg(cpu, 2), slot = 0;
    if (flags & GLOB_ALTDIRFUNC)
        sb_refuse("_glob", "was given GLOB_ALTDIRFUNC, whose directory functions in glob_t are x86 code");
    if (errfunc && (ocerz_abi_callback_convert(errfunc, "i(pi)", &slot) != OCERZ_OK || !slot))
        sb_refuse("_glob", "could not bind its error function to a callback");
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_glob", "i(pic{i(pi)}p)", (const void *)glob);
    int r = glob(sb_ptr(sb_arg(cpu, 0)), flags, slot ? (int (*)(const char *, int))ocerz_g2h(slot) : NULL,
                 sb_ptr(sb_arg(cpu, 3)));
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_globfree(struct OcerzVM *vm, OcerzCPU *cpu)
{
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_globfree", "v(p)", (const void *)globfree);
    globfree(sb_ptr(sb_arg(cpu, 0)));
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, 0);
}

int ocerz_sys_dispatch_main(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (!pthread_main_np()) {
        fprintf(stderr, "ocerz: bridge: dispatch_main() must be called on the main thread\n");
        ocerz_vm_request_exit(vm, 134);
        return OCERZ_STEP_EXIT;
    }
    void (*run)(void) = (void (*)(void))ocerz_bridge_host_symbol(OCERZ_BRIDGE_COREFOUNDATION, "CFRunLoopRun");
    if (!run) {
        fprintf(stderr, "ocerz: bridge: dispatch_main needs the native CFRunLoopRun, which could not be found\n");
        exit(OCERZ_BRIDGE_UNIMPL_EXIT);
    }
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_dispatch_main", "v()", (const void *)run);
    for (;;)
        run();
}

static int sb_vm_region(struct OcerzVM *vm, OcerzCPU *cpu, const char *export)
{
    mach_port_t task = (mach_port_t)sb_arg(cpu, 0);
    uint64_t addrp = sb_arg(cpu, 1), sizep = sb_arg(cpu, 2), info = sb_arg(cpu, 4);
    vm_region_flavor_t flavor = (vm_region_flavor_t)sb_arg(cpu, 3);
    uint64_t query = addrp ? ocerz_ld(addrp, 8) : 0;
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, export, "i(uppippp)", (const void *)mach_vm_region);
    kern_return_t kr = mach_vm_region(task, sb_ptr(addrp), sb_ptr(sizep), flavor, sb_ptr(info),
                                      sb_ptr(sb_arg(cpu, 5)), sb_ptr(sb_arg(cpu, 6)));
    ocerz_bridge_lower(&outer);
    if (kr == KERN_SUCCESS && task == mach_task_self() && addrp && sizep && info &&
        (flavor == VM_REGION_BASIC_INFO_64 || flavor == VM_REGION_BASIC_INFO)) {
        uint64_t ga = query, gsz = 0;
        unsigned prot = 0, maxprot = 0;
        ocerz_guest_vm_region(&ga, &gsz, &prot, &maxprot);
        if ((prot || maxprot) && ((ga <= query && query - ga < gsz) || ga <= ocerz_ld(addrp, 8))) {
            ocerz_st(addrp, 8, ga);
            ocerz_st(sizep, 8, gsz);
            ocerz_st(info, 4, prot);
            ocerz_st(info + 4, 4, maxprot);
        }
    }
    return sb_ret(vm, cpu, kr);
}

int ocerz_sys_mach_vm_region(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_vm_region(vm, cpu, "_mach_vm_region");
}

int ocerz_sys_vm_region_64(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_vm_region(vm, cpu, "_vm_region_64");
}

/* A fixed mach_vm_map below the low limit in a Wine process names the guest's
   shadow, not the host's address: Wine probes free space with FIXED and no
   OVERWRITE before it maps there, which the host answered for its own memory,
   and msync maps the wineserver's shared memory object.  The range is checked
   and claimed in ocerz's tables, and an object is mapped over the claimed
   shadow pages. */
#define SB_USER_VA_END 0x0000800000000000ull

/* With a low shadow, guest code reads every address below 12 GB through the
   shadow, so a host mapping the kernel placed there would be one page to the
   caller and another to the code that uses it: MacNdCheese's wineserver keeps
   msync's wait words in such pages and its wakes were lost.  An anywhere map
   the guest asks for searches from 12 GB up. */
static mach_vm_address_t sb_anywhere_floor(mach_port_t target, int flags, mach_vm_address_t addr)
{
    if (ocerz_low_base && target == mach_task_self() && (flags & VM_FLAGS_ANYWHERE) && addr < OCERZ_LOW_LIMIT)
        return OCERZ_LOW_LIMIT;
    return addr;
}

static kern_return_t sb_vm_map_low(struct OcerzVM *vm, uint64_t addr, mach_vm_size_t size, int flags,
                                   mach_port_t object, uint64_t offset, boolean_t copy, vm_prot_t cur,
                                   vm_prot_t max, vm_inherit_t inherit)
{
    if (!size || (addr & (OCERZ_HOST_PAGE_SIZE - 1)))
        return KERN_INVALID_ARGUMENT;
    if (size > OCERZ_LOW_LIMIT - addr)
        return KERN_NO_SPACE;
    if (!(flags & VM_FLAGS_OVERWRITE) && ocerz_mem_range_in_use(addr, size))
        return KERN_NO_SPACE;
    if (ocerz_mem_pinned(addr, size))
        return KERN_NO_SPACE;
    ocerz_jit_invalidate_range(vm, addr, size);
    int prot = (cur & VM_PROT_READ ? PROT_READ : 0) | (cur & VM_PROT_WRITE ? PROT_WRITE : 0) |
               (cur & VM_PROT_EXECUTE ? PROT_EXEC : 0);
    if (ocerz_map_fixed(addr, size, object == MACH_PORT_NULL ? prot : PROT_READ | PROT_WRITE) != OCERZ_OK)
        return KERN_NO_SPACE;
    if (object == MACH_PORT_NULL)
        return KERN_SUCCESS;
    mach_vm_address_t host = (mach_vm_address_t)(uintptr_t)ocerz_g2h(addr);
    kern_return_t kr = mach_vm_map(mach_task_self(), &host, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, object,
                                   offset, copy, cur & ~VM_PROT_EXECUTE, max, inherit);
    if (kr != KERN_SUCCESS)
        ocerz_unmap(addr, size);
    return kr;
}

/* An anonymous fixed map above the low window is guest memory, as a fixed
   mmap is: cache mode claims it in ocerz's map (syscall.c) and so does this.
   Sent to the host instead, it was a mapping ocerz did not know, and Wine's
   anon_mmap_tryfixed - this map, then mmap(MAP_FIXED) over it - found its own
   reservation in the way, so Chromium's 16 GB PartitionAlloc pools failed. */
static kern_return_t sb_vm_map_claim(struct OcerzVM *vm, uint64_t addr, mach_vm_size_t size, int flags)
{
    const int rw = PROT_READ | PROT_WRITE;
    int ok;
    if (flags & VM_FLAGS_OVERWRITE)
        ok = ocerz_map_fixed(addr, size, rw) == OCERZ_OK ||
             (ocerz_mem_register_range(addr, addr + size) == OCERZ_OK && ocerz_map_fixed(addr, size, rw) == OCERZ_OK);
    else
        ok = ocerz_map_claim_fixed(addr, size, rw) == OCERZ_OK || ocerz_map_claim_region(addr, size, rw) == OCERZ_OK ||
             (ocerz_mem_register_range(addr, addr + size) == OCERZ_OK &&
              ocerz_map_claim_region(addr, size, rw) == OCERZ_OK);
    if (!ok)
        return KERN_NO_SPACE;
    ocerz_jit_invalidate_range(vm, addr, size);
    return KERN_SUCCESS;
}

int ocerz_sys_mach_vm_map(struct OcerzVM *vm, OcerzCPU *cpu)
{
    mach_port_t target = (mach_port_t)sb_arg(cpu, 0);
    uint64_t addrp = sb_arg(cpu, 1);
    mach_vm_size_t size = sb_arg(cpu, 2);
    int flags = (int)sb_arg(cpu, 4);
    mach_vm_address_t addr = addrp ? ocerz_ld(addrp, 8) : 0;
    /* An anywhere hint above user space cannot be met, and only an uninitialized
       variable passes one: MacNdCheese's wineserver maps its msync pages from a
       stack slot x86 libsystem frames would have overwritten under Rosetta and
       that native mode leaves holding old strings, so the hint is dropped. */
    if ((flags & VM_FLAGS_ANYWHERE) && addr >= SB_USER_VA_END)
        addr = 0;
    addr = sb_anywhere_floor(target, flags, addr);
    if (target == mach_task_self() && ocerz_low_base && !(flags & VM_FLAGS_ANYWHERE) && addr < OCERZ_LOW_LIMIT)
        return sb_ret(vm, cpu, sb_vm_map_low(vm, addr, size, flags, (mach_port_t)sb_arg(cpu, 5), sb_arg(cpu, 6),
                                             (boolean_t)sb_arg(cpu, 7), (vm_prot_t)sb_arg(cpu, 8),
                                             (vm_prot_t)sb_arg(cpu, 9), (vm_inherit_t)sb_arg(cpu, 10)));
    if (target == mach_task_self() && !(flags & VM_FLAGS_ANYWHERE) && size &&
        (mach_port_t)sb_arg(cpu, 5) == MACH_PORT_NULL && addr >= OCERZ_LOW_LIMIT)
        return sb_ret(vm, cpu, sb_vm_map_claim(vm, addr, size, flags));
    if (target == mach_task_self() && !(flags & VM_FLAGS_ANYWHERE) && size)
        ocerz_jit_invalidate_range(vm, addr, size);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_mach_vm_map", "i(upLLiuLiiiu)", (const void *)mach_vm_map);
    kern_return_t kr = mach_vm_map(target, &addr, size, sb_arg(cpu, 3), flags, (mach_port_t)sb_arg(cpu, 5),
                                   sb_arg(cpu, 6), (boolean_t)sb_arg(cpu, 7),
                                   (vm_prot_t)sb_arg(cpu, 8) & ~VM_PROT_EXECUTE,
                                   (vm_prot_t)sb_arg(cpu, 9), (vm_inherit_t)sb_arg(cpu, 10));
    ocerz_bridge_lower(&outer);
    if (kr == KERN_SUCCESS && addrp)
        ocerz_st(addrp, 8, addr);
    return sb_ret(vm, cpu, kr);
}

int ocerz_sys_mach_vm_remap(struct OcerzVM *vm, OcerzCPU *cpu)
{
    mach_port_t target = (mach_port_t)sb_arg(cpu, 0);
    uint64_t addrp = sb_arg(cpu, 1), curp = sb_arg(cpu, 8), maxp = sb_arg(cpu, 9);
    mach_vm_size_t size = sb_arg(cpu, 2);
    int flags = (int)sb_arg(cpu, 4);
    mach_vm_address_t addr = addrp ? ocerz_ld(addrp, 8) : 0;
    vm_prot_t cur = 0, max = 0;
    addr = sb_anywhere_floor(target, flags, addr);
    if (target == mach_task_self() && !(flags & VM_FLAGS_ANYWHERE) && size)
        ocerz_jit_invalidate_range(vm, addr, size);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_mach_vm_remap", "i(upLLiuLippu)", (const void *)mach_vm_remap);
    kern_return_t kr = mach_vm_remap(target, &addr, size, sb_arg(cpu, 3), flags, (mach_port_t)sb_arg(cpu, 5),
                                     sb_arg(cpu, 6), (boolean_t)sb_arg(cpu, 7), &cur, &max,
                                     (vm_inherit_t)sb_arg(cpu, 10));
    ocerz_bridge_lower(&outer);
    if (kr == KERN_SUCCESS) {
        if (addrp)
            ocerz_st(addrp, 8, addr);
        if (curp)
            ocerz_st(curp, 4, (uint32_t)cur);
        if (maxp)
            ocerz_st(maxp, 4, (uint32_t)max);
    }
    return sb_ret(vm, cpu, kr);
}

int ocerz_sys_pthread_get_stackaddr_np(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *thread = sb_ptr(sb_arg(cpu, 0));
    uint64_t lo = 0, hi = 0;
    if (ocerz_vm_guest_stack(vm, thread, &lo, &hi))
        return sb_ret(vm, cpu, (int64_t)hi);
    return sb_ret(vm, cpu, (int64_t)ocerz_h2g(pthread_get_stackaddr_np((pthread_t)thread)));
}

int ocerz_sys_pthread_get_stacksize_np(struct OcerzVM *vm, OcerzCPU *cpu)
{
    void *thread = sb_ptr(sb_arg(cpu, 0));
    uint64_t lo = 0, hi = 0;
    if (ocerz_vm_guest_stack(vm, thread, &lo, &hi))
        return sb_ret(vm, cpu, (int64_t)(hi - lo));
    return sb_ret(vm, cpu, (int64_t)pthread_get_stacksize_np((pthread_t)thread));
}

int ocerz_sys_getpagesize(struct OcerzVM *vm, OcerzCPU *cpu)
{
    return sb_ret(vm, cpu, (int64_t)OCERZ_GUEST_PAGE_SIZE);
}

int ocerz_sys_sysconf(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int name = (int)sb_arg(cpu, 0);
    if (name == _SC_PAGESIZE)
        return sb_ret(vm, cpu, (int64_t)OCERZ_GUEST_PAGE_SIZE);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_sysconf", "l(i)", (const void *)sysconf);
    long r = sysconf(name);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_host_page_size(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t out = sb_arg(cpu, 1);
    if (out)
        ocerz_st(out, 8, OCERZ_GUEST_PAGE_SIZE);
    return sb_ret(vm, cpu, KERN_SUCCESS);
}

static void sb_sysctl_page_size(int is_page_size, uint64_t oldp, uint64_t oldlenp, int r)
{
    if (!is_page_size || r != 0 || !oldp || !oldlenp)
        return;
    uint64_t len = ocerz_ld(oldlenp, 8);
    if (len == 4 || len == 8)
        ocerz_st(oldp, (int)len, OCERZ_GUEST_PAGE_SIZE);
}

int ocerz_sys_sysctl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int *mib = sb_ptr(sb_arg(cpu, 0));
    unsigned n = (unsigned)sb_arg(cpu, 1);
    uint64_t oldp = sb_arg(cpu, 2), oldlenp = sb_arg(cpu, 3);
    int page = mib && n == 2 && mib[0] == CTL_HW && mib[1] == HW_PAGESIZE;
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_sysctl", "i(pupppL)", (const void *)sysctl);
    int r = sysctl(mib, n, sb_ptr(oldp), sb_ptr(oldlenp), sb_ptr(sb_arg(cpu, 4)), (size_t)sb_arg(cpu, 5));
    ocerz_bridge_lower(&outer);
    sb_sysctl_page_size(page, oldp, oldlenp, r);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_sysctlbyname(struct OcerzVM *vm, OcerzCPU *cpu)
{
    const char *name = sb_ptr(sb_arg(cpu, 0));
    uint64_t oldp = sb_arg(cpu, 1), oldlenp = sb_arg(cpu, 2);
    int page = name && (strcmp(name, "hw.pagesize") == 0 || strcmp(name, "hw.pagesize32") == 0 ||
                        strcmp(name, "vm.pagesize") == 0);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_sysctlbyname", "i(ppppL)", (const void *)sysctlbyname);
    int r = sysctlbyname(name, sb_ptr(oldp), sb_ptr(oldlenp), sb_ptr(sb_arg(cpu, 3)), (size_t)sb_arg(cpu, 4));
    ocerz_bridge_lower(&outer);
    sb_sysctl_page_size(page, oldp, oldlenp, r);
    return sb_ret(vm, cpu, r);
}

#define SB_SANDBOX_FILTER_MASK 0x3fffffff

extern int sandbox_check(pid_t pid, const char *operation, int type, ...);

int ocerz_sys_sandbox_check(struct OcerzVM *vm, OcerzCPU *cpu)
{
    pid_t pid = (pid_t)sb_arg(cpu, 0);
    const char *operation = sb_ptr(sb_arg(cpu, 1));
    int type = (int)sb_arg(cpu, 2);
    struct OcerzBridgeFrame outer;
    ocerz_bridge_raise(&outer, SB_LIB, "_sandbox_check", "i(ipi)", (const void *)sandbox_check);
    int r = (type & SB_SANDBOX_FILTER_MASK) ? sandbox_check(pid, operation, type, sb_ptr(sb_arg(cpu, 3)))
                                            : sandbox_check(pid, operation, type);
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

#define SB_SANDBOX_LIB "/usr/lib/libsandbox.1.dylib"

static void *sb_sandbox_sym(const char *lib, const char *name)
{
    void *h = lib ? dlopen(lib, RTLD_LAZY) : RTLD_DEFAULT;
    return h ? dlsym(h, name) : NULL;
}

int ocerz_sys_sandbox_init(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int (*fn)(const char *, uint64_t, char **) =
        (int (*)(const char *, uint64_t, char **))sb_sandbox_sym(NULL, "sandbox_init");
    struct OcerzBridgeFrame outer;
    int r = -1;
    ocerz_apidb_preload();
    ocerz_bridge_raise(&outer, SB_LIB, "_sandbox_init", "i(pLp)", (const void *)fn);
    if (fn)
        r = fn(sb_ptr(sb_arg(cpu, 0)), sb_arg(cpu, 1), sb_ptr(sb_arg(cpu, 2)));
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_sandbox_init_with_parameters(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int (*fn)(const char *, uint64_t, const char *const *, char **) =
        (int (*)(const char *, uint64_t, const char *const *, char **))sb_sandbox_sym(
            NULL, "sandbox_init_with_parameters");
    struct OcerzBridgeFrame outer;
    int r = -1;
    ocerz_apidb_preload();
    ocerz_bridge_raise(&outer, SB_LIB, "_sandbox_init_with_parameters", "i(pLpp)", (const void *)fn);
    if (fn)
        r = fn(sb_ptr(sb_arg(cpu, 0)), sb_arg(cpu, 1), sb_ptr(sb_arg(cpu, 2)), sb_ptr(sb_arg(cpu, 3)));
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_sandbox_ms(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int (*fn)(const char *, int, void *) =
        (int (*)(const char *, int, void *))sb_sandbox_sym(NULL, "__sandbox_ms");
    struct OcerzBridgeFrame outer;
    int r = -1;
    ocerz_apidb_preload();
    ocerz_bridge_raise(&outer, SB_LIB, "___sandbox_ms", "i(pip)", (const void *)fn);
    if (fn)
        r = fn(sb_ptr(sb_arg(cpu, 0)), (int)sb_arg(cpu, 1), sb_ptr(sb_arg(cpu, 2)));
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

int ocerz_sys_sandbox_apply(struct OcerzVM *vm, OcerzCPU *cpu)
{
    int (*fn)(void *) = (int (*)(void *))sb_sandbox_sym(SB_SANDBOX_LIB, "sandbox_apply");
    struct OcerzBridgeFrame outer;
    int r = -1;
    ocerz_apidb_preload();
    ocerz_bridge_raise(&outer, SB_SANDBOX_LIB, "_sandbox_apply", "i(p)", (const void *)fn);
    if (fn)
        r = fn(sb_ptr(sb_arg(cpu, 0)));
    ocerz_bridge_lower(&outer);
    return sb_ret(vm, cpu, r);
}

