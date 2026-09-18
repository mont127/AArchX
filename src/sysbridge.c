/*
 * libSystem calls a generic crossing cannot make, answered by ocerz: the
 * variadic file and IPC calls, memory mapping, non-local jumps and processes.
 *
 * ---- variadic calls whose optional argument is fixed ----
 * open, openat, their $NOCANCEL forms, open_dprotected_np and
 * openat_dprotected_np, fcntl and fcntl$NOCANCEL, ioctl, sem_open, shm_open,
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
 * the identity map is the crossing itself.  mach_vm_map and mach_vm_remap stay
 * stubs.
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
 */
#include "ocerz/sysbridge.h"
#include "ocerz/bridge.h"
#include "ocerz/syscall.h"
#include "ocerz/vdylib.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/jit.h"
#include "ocerz/interp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include <sys/wait.h>
#include <ulimit.h>
#include <unistd.h>
#include <wchar.h>
#include <mach/mach.h>

extern char **environ;

int ocerz_host_open_nocancel(const char *path, int flags, ...) __asm__("_open$NOCANCEL");
int ocerz_host_openat_nocancel(int fd, const char *path, int flags, ...) __asm__("_openat$NOCANCEL");
int ocerz_host_fcntl_nocancel(int fd, int cmd, ...) __asm__("_fcntl$NOCANCEL");

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
