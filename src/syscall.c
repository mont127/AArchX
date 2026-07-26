/*
 * src/syscall.c
 *
 * The guest-to-host syscall boundary: emulated x86_64 macOS syscalls are
 * translated and either intercepted or forwarded to the native arm64 XNU
 * kernel of the same machine. This is the security- and correctness-critical
 * frontier of Ocerz, so the absolute rule here is: NEVER hand the kernel a
 * pointer argument that still carries a guest virtual address. Every pointer
 * is converted through ocerz_g2h (guest_base offset) before forwarding, and
 * any syscall whose number is not in a table is a loud STEP_FATAL diagnostic
 * rather than a blind forward — an untranslated pointer would let the guest
 * read or scribble over arbitrary host memory.
 *
 * Dispatch keys off rax = (class << 24) | number. Class 2 is BSD/Unix and
 * runs through bsd_table[], a static array indexed by syscall number whose
 * entries declare: the human name, the argument count, a ptr_mask bit per
 * argument (bit i set => arg i is a pointer to translate when non-NULL),
 * whether a second value is returned in rdx (pipe), and an optional intercept
 * handler. Arguments arrive in rdi rsi rdx r10 r8 r9 (note r10, not rcx — the
 * x86_64 syscall convention replaces rcx, which SYSCALL clobbers with the
 * return rip); arguments seven and beyond are read from the guest stack at
 * rsp+8, rsp+16, ... (XNU skips the slot at rsp, the return-address slot of
 * the libSystem stub). Forwarding translates the masked pointers, calls
 * ocerz_host_syscall, then replicates the BSD error convention exactly: on
 * error set guest rflags.CF and put errno in rax; on success clear CF, put the
 * result in rax, and for dual-return syscalls put the second value in rdx.
 *
 * Class 1 is Mach traps: kern_return_t in rax with no carry protocol, trap
 * numbers negated by the raw layer. Most are forwarded after translating the
 * one pointer argument; the _kernelrpc_mach_vm_* memory traps are intercepted
 * to route through the Ocerz arena allocator just like mmap, because the guest
 * must only ever receive guest addresses inside the reserved arena.
 *
 * Class 3 is machine-dependent: trap 3 (thread_fast_set_cthread_self) stores
 * the new TLS base into cpu->gs_base, which is how x86_64 macOS reaches
 * thread-local storage at %gs:0.
 *
 * Intercept policy (documented deviations from a real kernel):
 *  - mmap/munmap/mprotect/madvise: arena and 16KB-page policy (see mem.h);
 *    guest MAP_FIXED never reaches the host except through ocerz_map_fixed.
 *  - shared_region_check_np: returns ENOSYS (78) so dyld runs cache-less; a
 *    future phase maps the x86_64 shared cache and answers for real.
 *  - bsdthread_register: returns the pthread feature-flag word the kernel
 *    would; sigaction/sigprocmask/sigaltstack record the handler addresses in
 *    static storage keyed by signal and fake success (no host signal state is
 *    touched — an arm64 signal frame is meaningless to the guest).
 *  - GUEST THREADS ARE REAL HOST THREADS. bsdthread_create(360) carves a guest
 *    thread onto a fresh host pthread that owns its own heap OcerzCPU and runs
 *    its own ocerz_vm_run_cpu loop over the SHARED guest arena. It enters the
 *    cache's _thread_start with the kernel's register convention (rdi=pthread
 *    self, rsi=a fresh mach port, rdx=func, rcx=arg, r8=stack, r9=flags|bit28)
 *    — bit 28 tells libpthread "the kernel set the TSD base", which Ocerz did
 *    by pointing the new CPU's gs_base at pthread+0xe0. bsdthread_terminate(361)
 *    flips the CPU's `terminated` flag (ending its loop) and signals the join
 *    semaphore. Thread join/condvars/mutexes synchronise through the real host
 *    kernel: __ulock_wait/wait2/wake (515/516/544) and the Mach semaphore traps
 *    are host-forwarded, so two real host threads block and wake each other
 *    natively over the shared lock words. __pthread_kill(328) delivers a signal
 *    to the calling thread by running its recorded handler via a nested
 *    ocerz_vm_call (saving and restoring the interrupted CPU). The JIT block
 *    cache and the bump allocator take a mutex (jit.c/mem.c); the current CPU
 *    is a per-thread TLS pointer so the crash handler reports the faulting
 *    thread. The GCD workqueue (workq_kernreturn REQTHREADS spawning a
 *    start_wqthread worker) is implemented with the faithful wqthread entry
 *    ABI: REQTHREADS derives the QoS class index from the requested
 *    pthread_priority and hands start_wqthread the kernel R8 word with the QoS
 *    marker (bit14) set and the class index in the low byte (plus NEWSPI and
 *    TSD_BASE_SET), so __pthread_wqthread builds a real QoS pthread_priority and
 *    _dispatch_worker_thread2 drains the correct root-queue bucket. A file-
 *    static running-worker count stops REQTHREADS spawning a fresh host thread
 *    per request when one already services the pool; THREAD_RETURN terminates
 *    the worker and its host-thread exit decrements the count (the real kernel
 *    parks and reuses the thread; fresh-per-request is equivalent for
 *    dispatch_async). The workqueue is ON by default — the two failures that
 *    once forced a gate are fixed: the immediate-form BT bug that made
 *    _Block_copy heap-copy libdispatch's global sentinel destructor blocks
 *    (interp_ext.c), and the thread identity below. Every spawned guest thread
 *    (bsdthread_create and wqthread alike) receives the REAL host thread's mach
 *    port as its kport, set by the worker entry on its own thread: libpthread
 *    stores that port in TSD, libdispatch publishes it as the unfair-lock owner
 *    in __ulock_wait lock words, and the host kernel — which arbitrates those
 *    waits for us — must be able to resolve the owner to a live thread, or it
 *    reports the "Owner in ulock is unknown" client crash. A minted receive
 *    right names no thread; mach_thread_self of the worker does.
 *  - Process creation, the Rosetta model: every guest process is a real host
 *    process, and an x86_64 child re-enters the emulator. posix_spawn(244)
 *    host-spawns this ocerz binary (_NSGetExecutablePath) on the guest's
 *    target path, forwarding guest argv[1..] and envp verbatim (the spawn-
 *    attrs/file-actions descriptor is not yet honored); the host child pid is
 *    written back and host-forwarded wait4 reaps it with real status bits.
 *    fork(2) host-forks the emulator itself: the guest arena clones
 *    copy-on-write at the exact instruction, the calling thread alone
 *    survives (POSIX), the guest's own atfork handlers re-fetch task/thread
 *    ports through the forwarded Mach traps, and the Darwin dual return is
 *    rax=pid,rdx=0 in the parent and rax=0,rdx=1 in the child. pthread_atfork
 *    takes the JIT and bump-allocator mutexes across the fork so the child
 *    never inherits a lock owned by a thread it does not have; the host libc
 *    fork runs Apple's own malloc atfork machinery for the same reason.
 *    execve(59) replaces the process image the kernel way: it host-execve's this
 *    ocerz binary onto the guest target (self + "-path" + guest_path + guest argv),
 *    so the re-entered emulator loads the new image in a fresh address space while
 *    the caller's pid, fds (with their real FD_CLOEXEC bits, since guest fds are
 *    host fds) and parent linkage are preserved exactly as exec requires. The
 *    guest path (a[0]) drives loading and the guest argv (a[1], whose argv[0] may
 *    differ from the path) is forwarded verbatim; a leading "#!" line is parsed
 *    and the interpreter re-exec'd with the script appended, mirroring XNU's
 *    script-exec. On any failure execve returns the host errno to the guest.
 *  - mach_vm_*: arena allocator. ANYWHERE requests bump-allocate; FIXED
 *    requests (flags bit0 clear) honor the kernel contract — memory at
 *    exactly the caller's address via ocerz_map_claim_fixed, or an honest
 *    KERN_NO_SPACE when the range may be occupied. libmalloc grows large
 *    blocks in place with FIXED allocations at the block's end and trusts a
 *    success without re-reading the address, so a silently-relocated "success"
 *    hands the guest a pointer to unmapped memory (NSMutableData's 10MB grow
 *    used to SIGBUS exactly this way); the NO_SPACE failure instead routes
 *    libmalloc to its normal allocate-new-and-copy fallback.
 * thread_fast_set_cthread_self returns 0x60 (the historical %gs selector);
 * the exact value the guest expects is uncertain but unused, documented here.
 *
 * The _nocancel BSD aliases (read/write/open/close/pread/pwrite) are mapped
 * to the same handling as their cancellable forms — cancellation points have
 * no meaning to a single-threaded interpreter.
 */
#include "ocerz/syscall.h"
#include "ocerz/vm.h"
#include "ocerz/mem.h"
#include "ocerz/jit.h"
#include "ocerz/sys_raw.h"
#include "ocerz/interp.h"
#include "ocerz/dyld.h"

#include <stddef.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <spawn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <signal.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <time.h>

#define OCERZ_BSD_MAX 600

#define OCERZ_ENOMEM_V 12
#define OCERZ_ENOTSUP_V 45
#define OCERZ_ENOSYS_V 78

#define OCERZ_NSIG 64

#define OCERZ_MACH_KERN_SUCCESS 0
#define OCERZ_MACH_KERN_FAILURE 5
#define OCERZ_MACH_KERN_NO_SPACE 3
#define OCERZ_MACH_KERN_NOT_SUPPORTED 46

#define OCERZ_F_PREALLOCATE 42
#define OCERZ_F_GETPATH 50

#define OCERZ_IOV_MAX 64

struct ocerz_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

typedef int (*ocerz_bsd_fn)(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8]);

typedef struct ocerz_bsd_entry {
    const char *name;
    uint8_t nargs;
    uint8_t ptr_mask;
    uint8_t dual_ret;
    ocerz_bsd_fn intercept;
} ocerz_bsd_entry;

/* Per-signal registered disposition (process-wide). The Darwin __sigaction the
 * guest passes is {handler@0, sa_tramp@8, sa_mask@16(u32), sa_flags@20(int)};
 * delivering a signal needs all four — the trampoline is the return address the
 * guest's _sigtramp expects, sa_flags carries SA_SIGINFO/SA_ONSTACK, sa_mask is
 * added to the blocked set for the handler's duration. */
typedef struct {
    uint64_t handler;
    uint64_t tramp;
    uint64_t mask;
    uint32_t flags;
} GuestSigact;
static GuestSigact guest_sigact[OCERZ_NSIG];

#define DARWIN_SA_ONSTACK 0x0001u
#define DARWIN_SA_RESETHAND 0x0004u
#define DARWIN_SA_NODEFER 0x0010u
#define DARWIN_SA_SIGINFO 0x0040u

static const char *errno_name(int e)
{
    switch (e) {
    case 1: return "EPERM";
    case 2: return "ENOENT";
    case 3: return "ESRCH";
    case 4: return "EINTR";
    case 5: return "EIO";
    case 9: return "EBADF";
    case 11: return "EDEADLK";
    case 12: return "ENOMEM";
    case 13: return "EACCES";
    case 14: return "EFAULT";
    case 17: return "EEXIST";
    case 20: return "ENOTDIR";
    case 21: return "EISDIR";
    case 22: return "EINVAL";
    case 24: return "EMFILE";
    case 35: return "EAGAIN";
    case 45: return "ENOTSUP";
    case 78: return "ENOSYS";
    default: return "E?";
    }
}

static void ret_ok(OcerzCPU *cpu, uint64_t v)
{
    cpu->rflags &= ~(uint64_t)OCERZ_CF;
    cpu->gpr[OCERZ_RAX] = v;
}

static void ret_ok2(OcerzCPU *cpu, uint64_t v, uint64_t v2)
{
    cpu->rflags &= ~(uint64_t)OCERZ_CF;
    cpu->gpr[OCERZ_RAX] = v;
    cpu->gpr[OCERZ_RDX] = v2;
}

static void ret_err(OcerzCPU *cpu, uint64_t errno_v)
{
    cpu->rflags |= OCERZ_CF;
    cpu->gpr[OCERZ_RAX] = errno_v;
}

static void mach_ret(OcerzCPU *cpu, uint64_t kr)
{
    cpu->gpr[OCERZ_RAX] = kr;
}

static int sys_exit(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    if (getenv("OCERZ_EXITLOG")) {
        fprintf(stderr, "ocerz: EXITLOG code=%d rip=%#llx ret-chain:",
                (int)a[0], (unsigned long long)cpu->rip);
        uint64_t fp = cpu->gpr[OCERZ_RBP];
        for (int d = 0; d < 8 && fp >= ocerz_arena_lo && fp < ocerz_arena_hi; d++) {
            fprintf(stderr, " %#llx", (unsigned long long)ocerz_ld(fp + 8, 8));
            fp = ocerz_ld(fp, 8);
        }
        fprintf(stderr, "\n");
    }
    ocerz_vm_request_exit(vm, (int)(uint32_t)a[0] & 0xff);
    return OCERZ_STEP_EXIT;
}

static int sys_unsupported(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    (void)cpu;
    (void)a;
    return OCERZ_STEP_FATAL;
}

static int sys_abort_payload(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    OCERZ_LOG("guest abort_with_payload: reason_namespace=%llu reason_code=%llu\n",
              (unsigned long long)a[0], (unsigned long long)a[1]);
    for (int k = 0; k < 5; k++) {
        uint64_t v = ocerz_ld(0x300002000ull + (uint64_t)k * 8, 8);
        fprintf(stderr, "  selref[%d] = %#llx %s\n", k, (unsigned long long)v,
                v >= 0x7ff800000000ull ? "CACHE" : "ARENA(raw)");
    }
    (void)cpu;
    ocerz_vm_request_exit(vm, 134);
    return OCERZ_STEP_EXIT;
}

static void memtrace(const char *op, uint64_t a, uint64_t l, int prot, int flags)
{
    static int on = -1;
    if (on < 0)
        on = getenv("OCERZ_MEMTRACE") != NULL ? 1 : 0;
    if (on && a >= 0x7ff00000ull && a < 0x80000000ull)
        fprintf(stderr, "ocerz: MEM %s addr=%#llx len=%#llx prot=%#x flags=%#x comm=%d\n",
                op, (unsigned long long)a, (unsigned long long)l, prot, flags,
                ocerz_addr_committed(a));
}

static int sys_mmap(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    uint64_t addr = a[0];
    uint64_t len = a[1];
    int prot = (int)a[2];
    int flags = (int)a[3];
    int fd = (int)(int32_t)a[4];
    uint64_t pos = a[5];
    int anon = (flags & MAP_ANON) != 0 || fd < 0;
    int fixed = (flags & MAP_FIXED) != 0;
    uint64_t gaddr;

    if (flags & MAP_SHARED)
        ocerz_jit_require_ordered(vm);

    memtrace(anon ? "mmap-anon" : "mmap-file", addr, len, prot, flags);
    if (anon) {
        if (fixed) {
            int rc;
            if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0) {
                rc = ocerz_unmap(addr, len);
                if (rc != OCERZ_OK)
                    rc = ocerz_mem_register_range(addr, addr + len);
            } else {
                rc = ocerz_map_fixed(addr, len, prot);
                if (rc != OCERZ_OK &&
                    ocerz_mem_register_range(addr, addr + len) == OCERZ_OK)
                    rc = ocerz_map_fixed(addr, len, prot);
            }
            if (rc != OCERZ_OK) {
                ret_err(cpu, OCERZ_ENOMEM_V);
                return OCERZ_STEP_OK;
            }
            if ((flags & MAP_SHARED) &&
                ocerz_map_shared_anon(addr, len, prot) != OCERZ_OK) {
                ocerz_unmap(addr, len);
                ret_err(cpu, OCERZ_ENOMEM_V);
                return OCERZ_STEP_OK;
            }
            ret_ok(cpu, addr);
            return OCERZ_STEP_OK;
        }
        /* A non-zero addr without MAP_FIXED is a HINT. The kernel grants it when the range
         * is free, and callers use exactly that to probe the address space -- Wine locates
         * its Windows address space this way. ocerz_map_hint grants it only when it can
         * reserve brand-new host space there, so a hint can never displace live memory;
         * anything else falls through to the allocator, which is what happened before. */
        gaddr = 0;
        if (addr != 0 && !getenv("OCERZ_NO_MMAP_HINT") &&
            ocerz_map_hint(addr, len, prot) == OCERZ_OK)
            gaddr = addr & ~0x3fffull;   /* page-aligned base the hint mapped at */
        if (gaddr == 0)
            gaddr = ocerz_map_anywhere(len, prot);
        if (gaddr == 0) {
            ret_err(cpu, OCERZ_ENOMEM_V);
            return OCERZ_STEP_OK;
        }
        if ((flags & MAP_SHARED) &&
            ocerz_map_shared_anon(gaddr, len, prot) != OCERZ_OK) {
            ocerz_unmap(gaddr, len);
            ret_err(cpu, OCERZ_ENOMEM_V);
            return OCERZ_STEP_OK;
        }
        ret_ok(cpu, gaddr);
        return OCERZ_STEP_OK;
    }

    if (fixed) {
        int rc = ocerz_map_fixed(addr, len, PROT_READ | PROT_WRITE);
        if (rc != OCERZ_OK &&
            ocerz_mem_register_range(addr, addr + len) == OCERZ_OK)
            rc = ocerz_map_fixed(addr, len, PROT_READ | PROT_WRITE);
        if (rc != OCERZ_OK) {
            ret_err(cpu, OCERZ_ENOMEM_V);
            return OCERZ_STEP_OK;
        }
        gaddr = addr;
    } else {
        gaddr = ocerz_map_anywhere(len, PROT_READ | PROT_WRITE);
        if (gaddr == 0) {
            ret_err(cpu, OCERZ_ENOMEM_V);
            return OCERZ_STEP_OK;
        }
    }

    if (getenv("OCERZ_MEMTRACE")) {
        char path[256];
        path[0] = 0;
        fcntl(fd, F_GETPATH, path);
        fprintf(stderr, "ocerz: FILEMAP gaddr=%#llx len=%#llx prot=%#x SHARED=%d fd=%d off=%#llx path=%s\n",
                (unsigned long long)gaddr, (unsigned long long)len, prot,
                (flags & MAP_SHARED) ? 1 : 0, fd, (unsigned long long)pos, path);
    }
    /* A MAP_SHARED file mapping (the wineserver tmpmap blocks: KUSER_SHARED_DATA,
     * the session and per-thread blocks) must be GENUINELY shared so writes by
     * the server process are visible to clients. Overlay the fd MAP_SHARED onto
     * the reserved region; only fall back to a private pread snapshot if that
     * fails (or for MAP_PRIVATE file mappings, which want a copy). */
    if (flags & MAP_SHARED) {
        int src = ocerz_map_shared_file(gaddr, len, prot, fd, pos);
        if (getenv("OCERZ_MEMTRACE"))
            fprintf(stderr, "ocerz: SHAREDMAP gaddr=%#llx len=%#llx -> %s\n",
                    (unsigned long long)gaddr, (unsigned long long)len,
                    src == OCERZ_OK ? "shared-ok" : "FAILED(fallback-pread)");
        if (src == OCERZ_OK) {
            ret_ok(cpu, gaddr);
            return OCERZ_STEP_OK;
        }
    }
    {
        uint64_t pa[8] = { (uint64_t)fd, (uint64_t)(uintptr_t)ocerz_g2h(gaddr), len, pos, 0, 0, 0, 0 };
        int err = 0;
        uint64_t ret2 = 0;
        ocerz_host_syscall(153, pa, &ret2, &err);
        (void)err;
    }
    ocerz_protect(gaddr, len, prot);
    ret_ok(cpu, gaddr);
    return OCERZ_STEP_OK;
}

static int sys_munmap(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    ocerz_unmap(a[0], a[1]);
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int sys_mprotect(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    memtrace("mprotect", a[0], a[1], (int)a[2], 0);
    ocerz_protect(a[0], a[1], (int)a[2]);
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int sys_madvise(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    (void)a;
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int sys_shared_region_check_np(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    (void)a;
    ret_err(cpu, OCERZ_ENOSYS_V);
    return OCERZ_STEP_OK;
}

static int sys_bsdthread_register(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    (void)a;
    /* Return the pthread feature-flag word libpthread caches from __bsdthread_register
     * and that _pthread_workqueue_supported() reports. 0x4000005f historically advertised
     * QOS|NEWSPI|...|KEVENT(0x40) but NOT WORKLOOP(0x80). Guest libdispatch's
     * __dispatch_root_queues_init_once tests bit7 (PTHREAD_FEATURE_WORKLOOP): only if set
     * does it register _dispatch_workloop_worker_thread into libpthread's dispatch-callback
     * table workloop slot (0x7ff8436bd638). With bit7 clear that slot stays NULL, so a
     * kernel-delivered bit22 WORKLOOP worker `call rax` in _pthread_wqthread jumps to 0 ->
     * guest_rip=0 crash. Advertise WORKLOOP so the guest registers the real workloop drain.
     * Gate on OCERZ_HOSTWQ: `make check` runs without HOSTWQ, so it keeps seeing the
     * byte-identical historical 0x4000005f (no differential/guest regression). */
    uint64_t feat = 0x4000005f;
    if (getenv("OCERZ_HOSTWQ"))
        feat |= 0x80ull;   /* PTHREAD_FEATURE_WORKLOOP -> 0x400000df */
    ret_ok(cpu, feat);
    return OCERZ_STEP_OK;
}

/* A no-op "succeeded" stub. Only for calls whose entire effect ocerz genuinely does not
 * need. __semwait_signal was on this list and must never go back: it is macOS's core
 * timed-wait primitive -- usleep/nanosleep/pthread_cond_timedwait all land on it -- so
 * stubbing it made every sleep in the guest return instantly. */
static int sys_workq_stub(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    (void)a;
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

/* CPU-IDENTITY OVERRIDES.
 *
 * The guest is x86_64; the host is arm64. A blind forward hands the guest the HOST's
 * identity -- hw.machine "arm64", hw.cputype CPU_TYPE_ARM64 -- which is simply false for
 * the process asking, and guest software branches on it. Wine reads these to pick which
 * loader to exec and, seeing arm64, went looking for lib/wine/i386-unix/wine (a 32-bit
 * UNIX loader that cannot exist on this macOS), so `wine <program>` died instantly.
 *
 * The values below are not invented: they are what a translated x86_64 process actually
 * observes under Rosetta 2 on this machine, measured and matched field for field. Note
 * Rosetta overrides ONLY the identity fields -- hw.model, machdep.cpu.brand_string and the
 * cpu counts still report the real Apple hardware -- so anything not listed here keeps
 * forwarding, which is both simpler and more faithful than inventing a fake Intel machine.
 *
 * Resolved by NAME to a MIB once, rather than hardcoding numbers: hw.machine is the classic
 * 6.1, but hw.cputype/cpusubtype/cpufamily sit at dynamically assigned oids (6.108-6.111
 * here) that are not guaranteed across OS releases. */
struct ocerz_sysctl_ovr {
    const char *name;
    int is_str;
    const char *sval;
    uint32_t ival;
    int mib[8];
    size_t miblen;
    int resolved;
};

static struct ocerz_sysctl_ovr g_sysctl_ovr[] = {
    { "hw.machine",             1, "x86_64", 0,           {0}, 0, 0 },
    { "hw.cputype",             0, NULL,     7,           {0}, 0, 0 }, /* CPU_TYPE_X86 */
    { "hw.cpusubtype",          0, NULL,     4,           {0}, 0, 0 },
    { "hw.cpufamily",           0, NULL,     0x573B5EECu, {0}, 0, 0 }, /* Intel Westmere, as Rosetta reports */
    { "sysctl.proc_translated", 0, NULL,     1,           {0}, 0, 0 }, /* ocerz IS the translator */
};

static void ocerz_sysctl_ovr_resolve(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    for (unsigned i = 0; i < sizeof g_sysctl_ovr / sizeof g_sysctl_ovr[0]; i++) {
        g_sysctl_ovr[i].miblen = 8;
        if (sysctlnametomib(g_sysctl_ovr[i].name, g_sysctl_ovr[i].mib,
                            &g_sysctl_ovr[i].miblen) == 0)
            g_sysctl_ovr[i].resolved = 1;
    }
}

/* Write one override into the guest's (oldp, oldlenp), following sysctl's contract:
 * a NULL oldp is a size query, and a short buffer is ENOMEM with the needed size
 * reported. Returns a STEP code, since the caller returns it directly. */
static int ocerz_sysctl_ovr_emit(OcerzCPU *cpu, const struct ocerz_sysctl_ovr *o,
                                 uint64_t oldp, uint64_t oldlenp)
{
    uint64_t need = o->is_str ? (uint64_t)strlen(o->sval) + 1 : 4;
    if (oldp) {
        uint64_t cap = oldlenp ? ocerz_ld(oldlenp, 8) : need;
        if (cap < need) {
            if (oldlenp)
                ocerz_st(oldlenp, 8, need);
            ret_err(cpu, OCERZ_ENOMEM_V);
            return OCERZ_STEP_OK;
        }
        if (o->is_str)
            for (uint64_t k = 0; k < need; k++)
                ocerz_st(oldp + k, 1, (uint64_t)(uint8_t)o->sval[k]);
        else
            ocerz_st(oldp, 4, o->ival);
    }
    if (oldlenp)
        ocerz_st(oldlenp, 8, need);
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

/* sysctl(name, namelen, oldp, oldlenp, newp, newlen) -- the NUMERIC-MIB form, which is the
 * one that matters: libsystem's sysctlbyname() resolves the name to an oid and then calls
 * THIS, so a name-keyed hook on syscall 274 alone never fires (verified: OCERZ_SYSCTLLOG
 * showed zero 274 calls while the guest read hw.machine). Reads are matched against the
 * resolved override MIBs; writes and everything else forward untouched. */
static int sys_sysctl(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    uint64_t nlen = a[1];
    static int sclog2 = -1;
    if (sclog2 < 0) sclog2 = getenv("OCERZ_SYSCTLLOG") ? 1 : 0;
    if (a[4] == 0 && nlen >= 2 && nlen <= 8) {   /* a read, plausible mib length */
        int mib[8];
        for (uint64_t i = 0; i < nlen; i++)
            mib[i] = (int)(int32_t)ocerz_ld(a[0] + i * 4, 4);
        ocerz_sysctl_ovr_resolve();
        if (sclog2)
            fprintf(stderr, "ocerz: sysctl mib=%d.%d nlen=%llu oldp=%#llx oldlenp=%#llx\n",
                    mib[0], nlen>1?mib[1]:-1, (unsigned long long)nlen,
                    (unsigned long long)a[2], (unsigned long long)a[3]);
        for (unsigned i = 0; i < sizeof g_sysctl_ovr / sizeof g_sysctl_ovr[0]; i++) {
            const struct ocerz_sysctl_ovr *o = &g_sysctl_ovr[i];
            if (!o->resolved || o->miblen != nlen)
                continue;
            if (memcmp(mib, o->mib, (size_t)nlen * sizeof(int)) == 0)
                return ocerz_sysctl_ovr_emit(cpu, o, a[2], a[3]);
        }
    }
    uint64_t fa[8];
    memcpy(fa, a, sizeof fa);
    for (int i = 0; i < 8; i++)
        if ((0x1du & (1u << i)) && fa[i] != 0)
            fa[i] = (uint64_t)(uintptr_t)ocerz_g2h(fa[i]);
    int err = 0;
    uint64_t ret2 = 0;
    uint64_t r = ocerz_host_syscall(202, fa, &ret2, &err);
    if (err)
        ret_err(cpu, r);
    else
        ret_ok(cpu, r);
    return OCERZ_STEP_OK;
}

/* sysctlbyname(name, namelen, oldp, oldlenp, newp, newlen): a[0]=name(guest), a[1]=namelen,
 * a[2]=oldp(guest), a[3]=oldlenp(guest), a[4]=newp, a[5]=newlen. The x86_64 guest IS a translated
 * process (ocerz is its translator), so faithfully report sysctl.proc_translated=1 like Rosetta --
 * NOT the host arm64 process's 0 that a blind forward returns -- so translation-aware libsystem /
 * LaunchServices code takes the same branch it does under real Rosetta. Everything else forwards. */
static int sys_sysctlbyname(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    char name[160];
    uint64_t nl = a[1];
    if (nl >= sizeof name) nl = sizeof name - 1;
    for (uint64_t i = 0; i < nl; i++)
        name[i] = (char)ocerz_ld(a[0] + i, 1);
    name[nl] = 0;
    static int sclog = -1;
    if (sclog < 0) sclog = getenv("OCERZ_SYSCTLLOG") ? 1 : 0;
    if (sclog)
        fprintf(stderr, "ocerz: sysctlbyname '%s' oldp=%#llx\n", name, (unsigned long long)a[2]);
    if (strcmp(name, "sysctl.proc_translated") == 0) {
        if (a[3]) {
            uint64_t cap = ocerz_ld(a[3], 8);
            if (a[2] && cap >= 4)
                ocerz_st(a[2], 4, 1);
            ocerz_st(a[3], 8, 4);
        }
        ret_ok(cpu, 0);
        return OCERZ_STEP_OK;
    }
    uint64_t fa[8];
    memcpy(fa, a, sizeof fa);
    for (int i = 0; i < 8; i++)
        if ((0x1du & (1u << i)) && fa[i] != 0)
            fa[i] = (uint64_t)(uintptr_t)ocerz_g2h(fa[i]);
    int err = 0;
    uint64_t ret2 = 0;
    uint64_t r = ocerz_host_syscall(274, fa, &ret2, &err);
    if (err)
        ret_err(cpu, r);
    else
        ret_ok(cpu, r);
    return OCERZ_STEP_OK;
}

/* struct kevent_qos_s -- the ABI of every kevent list ocerz bridges. The kernel's
 * definition is PRIVATE (the public <sys/event.h> only forward-declares it), so this
 * is declared explicitly here and pinned by _Static_assert. It is 72 (0x48) bytes,
 * NOT 64: ocerz used to hard-code 0x40, which mis-addressed every list entry from
 * index 1 onward by 8 bytes per index and under-copied every single event by 8.
 *
 * The size is NOT taken on faith from a header, nor from a hand-rolled sizeof (that
 * proof would be circular -- it would only measure our own declaration). It is read
 * out of the shipping consumer, libdispatch's _dispatch_event_loop_merge, which
 * states the stride twice with two independent constants:
 *     leaq (,%r13,8),%rax ; leaq (%rax,%rax,8),%rdx   -> alloca(n * 8 * 9) = n * 72
 *     ...
 *     callq _dispatch_kevent_drain ; addq $0x48,%rbx  -> iteration stride = 0x48
 * The field offsets below were already correct in ocerz; only the stride was wrong,
 * which is exactly why this survived so long -- every single-event access was fine.
 * The asserts are what keep a future edit honest. */
struct ocerz_kevent_qos_s {
    uint64_t ident;
    int16_t  filter;
    uint16_t flags;
    uint32_t qos;
    uint64_t udata;
    uint32_t fflags;
    uint32_t xflags;
    int64_t  data;
    uint64_t ext[4];
};

_Static_assert(sizeof(struct ocerz_kevent_qos_s) == 72, "kevent_qos_s must be 72 bytes");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, ident)  == 0x00, "kevent ident offset");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, filter) == 0x08, "kevent filter offset");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, flags)  == 0x0a, "kevent flags offset");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, qos)    == 0x0c, "kevent qos offset");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, udata)  == 0x10, "kevent udata offset");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, fflags) == 0x18, "kevent fflags offset");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, xflags) == 0x1c, "kevent xflags offset");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, data)   == 0x20, "kevent data offset");
_Static_assert(offsetof(struct ocerz_kevent_qos_s, ext)    == 0x28, "kevent ext offset");

/* THE STRIDE IS THE TRUE 0x48. An earlier revision pinned it to 0x40; that was wrong, and
 * the note that justified it is superseded -- read this before touching it either way.
 *
 * kevent_qos_s is 0x48 (72) bytes: the struct above is accurate and static-asserted, and the
 * shipping libdispatch iterates its event list with `addq $0x48,%rbx`. At 0x40 the bridge
 * drops ext[3] and mis-addresses entry i by 8*i, in BOTH directions -- it read the kernel's
 * array (which is always 0x48) and wrote the guest's at 64-byte spacing.
 *
 * That was measured directly rather than argued: printing each delivered event's filter read
 * at both strides, a 2-event delivery gives
 *     i=0: 0x48 -> -8 (EVFILT_MACHPORT)   0x40 -> -8      (identical, offset 0)
 *     i=1: 0x48 -> -8 (EVFILT_MACHPORT)   0x40 -> 9987    (garbage)
 * so at 0x40 every entry past the first is shifted 8 bytes. The visible consequences, both
 * intermittent because nev>1 is rare: the MACHBRIDGE loop below reads that garbage filter,
 * does not recognise EVFILT_MACHPORT, and so leaves ext[0] pointing at the HOST message
 * buffer (SIGSEGV on an outside-arena address); and libdispatch reads a shifted neighbour
 * field as its aux header, getting a non-NULL pointer to zeroes and taking its
 * "BUG IN LIBDISPATCH: Invalid msg aux data size" abort.
 *
 * The superseded note recorded 0x48 as a hard regression -- "RESPONSIVE 0/8, window 0/8,
 * never launches" -- and concluded some other site was co-dependent on 64-byte entries.
 * Re-measured here with a window probe validated against known-good apps before being
 * trusted: the stride change BY ITSELF is 3/3 launches with windows. Whatever that
 * co-dependency was, it is not in the current tree.
 *
 * The trap that note was pointing at is real, though, just misattributed. A first attempt at
 * this fix changed the stride AND started relocating ext2/ext3 in the MACHBRIDGE loop below,
 * and that pair IS a 0/6 regression -- at 0x40 the aux fields were being read at the wrong
 * offsets and the relocation mostly no-oped, so correcting the stride is what armed it. The
 * two changes look like one change and are not. See the MACHBRIDGE comment.
 * OCERZ_STRIDE40 restores the old value so the A/B can be re-run from one binary. */
static uint64_t ocerz_kev_stride(void)
{
    static int s40 = -1;
    if (s40 < 0)
        s40 = getenv("OCERZ_STRIDE40") != NULL ? 1 : 0;
    return s40 ? (uint64_t)0x40 : (uint64_t)0x48;
}
#define OCERZ_KEVENT_QOS_S (ocerz_kev_stride())

/* WQ_KEVENT_LIST_LEN -- the number of kevent_qos_s entries in the buffer the kernel
 * hands a workqueue thread's callback, and therefore the real capacity of the re-arm
 * changelist that callback may return. This bounds BOTH directions of the bridge: how
 * many delivered events are copied in, and how many guest re-arms are copied back. */
#define OCERZ_WQ_KEVENT_LIST_LEN 16

/* OCERZ_KEVENT_QOS_REARM -- the stride for the RE-ARM relay in sys_workq_kernreturn,
 * and it MUST be the true 0x48, not the delivery-side 0x40 above. Here is the isolated
 * co-dependency the delivery-side comment asked to find:
 *
 * The re-arm relay reads the GUEST's outgoing changelist. That changelist is written by
 * the shipping libpthread/libdispatch (_dispatch_event_loop_merge), which always uses the
 * real 0x48-byte kevent_qos_s, and it is copied into the host kernel's 0x48-native events
 * buffer. BOTH ends of that copy are genuine kernel structs, so 0x40 there is unambiguously
 * wrong: it reads every entry past index 0 eight bytes short. The re-arm list is measured
 * n=2..3, and the EVFILT_WORKLOOP (-17) re-arm is routinely NOT entry 0 -- so ~half the time
 * (whenever it landed at index >=1) ocerz handed the kernel a garbage workloop re-arm, the
 * workloop was mis-armed and never drained, and the app parked at 0% CPU with no window:
 * the "Application not responding" lost-wakeup. Its dependence on the re-arm entry's ordering
 * is exactly why the hang was ~50% and not deterministic.
 *
 * Why this can be 0x48 while the delivery stride above stays 0x40: the delivery path is
 * nev==1-dominated (a workloop wakeup delivers a single EVFILT_WORKLOOP event), so its stride
 * never multiplies a nonzero index -- flipping the delivery macro to 0x48 was a regression
 * because it also perturbed the nev==1 evbuf layout the guest reads, not because 0x40 was ever
 * right for the re-arm. These are two different buffers with two different owners; they take
 * two different strides. */
#define OCERZ_KEVENT_QOS_REARM ((uint64_t)0x48)

static int sys_kevent_id(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8]);
static void ocerz_hostwq_register(OcerzVM *vm);

/* OCERZ_DQDUMP (diagnostic only, gated): a dispatch workloop id IS the guest
 * dispatch_workloop_t pointer, so the object's dq_state -- whose LOW 32 bits are
 * libdispatch's drain-owner lock (the servicing thread's mach port) -- is readable
 * straight out of guest memory. Dumping the object header at each arm / worker
 * entry / worker exit shows whether a workloop that is never drained is sitting
 * with a STALE non-zero owner (a worker that died mid-drain) or a zero owner. */
static void wl_dqdump(const char *tag, uint64_t id, unsigned cpun, unsigned kp)
{
    if (!id)
        return;
    fprintf(stderr, "ocerz: DQ %-6s id=%#llx cpu#%u kport=%#x |", tag,
            (unsigned long long)id, cpun, kp);
    for (uint64_t off = 0; off <= 0x48; off += 8)
        fprintf(stderr, " +%02llx=%016llx", (unsigned long long)off,
                (unsigned long long)ocerz_ld(id + off, 8));
    fprintf(stderr, "\n");
}

/* Per-host-worker-thread re-arm handoff for HOSTWQ inline workers. When the host
 * kernel calls one of the workqueue callbacks it hands &events/&nevents (in-out):
 * the callback fills them with the OUTGOING re-arm changelist before returning so
 * the host _pthread_wqthread re-arms the level-triggered sources. The guest worker
 * produces that changelist and issues workq_kernreturn(op 0x40/0x100, keventlist,
 * nkevents); sys_workq_kernreturn copies it back into *g_hostwq_tl_events and sets
 * *g_hostwq_tl_nevents. evcap caps the copy at the kernel buffer's capacity. */
static __thread void **g_hostwq_tl_events;
static __thread int   *g_hostwq_tl_nevents;
static __thread int    g_hostwq_tl_evcap;

#define OCERZ_THREAD_START 0x7ff802e6f820ull

struct ocerz_worker {
    OcerzVM *vm;
    OcerzCPU cpu;
};

static volatile int g_wq_running;

/* Active-workloop registry. A dispatch workloop is serviced by AT MOST one
 * Ocerz worker at a time: wl_try_acquire claims a workloop id on a commit and
 * the worker releases it when it exits. This is what lets a worker re-arm its
 * own workloop (kevent_id with the same id) WITHOUT forking a second worker,
 * while still letting a DIFFERENT workloop's wakeup summon its own worker --
 * the single global running-count gate dropped those cross-workloop wakeups and
 * left queues released-while-enqueued. */
static uint64_t g_active_wl[128];
static pthread_mutex_t g_wl_lock = PTHREAD_MUTEX_INITIALIZER;

static int wl_try_acquire(uint64_t id)
{
    if (id == 0)
        return 0;
    pthread_mutex_lock(&g_wl_lock);
    int slot = -1;
    for (int i = 0; i < 128; i++) {
        if (g_active_wl[i] == id) {
            pthread_mutex_unlock(&g_wl_lock);
            return 0;
        }
        if (g_active_wl[i] == 0 && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_wl_lock);
        return 0;
    }
    g_active_wl[slot] = id;
    pthread_mutex_unlock(&g_wl_lock);
    return 1;
}

static void wl_release(uint64_t id)
{
    if (id == 0)
        return;
    pthread_mutex_lock(&g_wl_lock);
    for (int i = 0; i < 128; i++)
        if (g_active_wl[i] == id) {
            g_active_wl[i] = 0;
            break;
        }
    pthread_mutex_unlock(&g_wl_lock);
}

static int ocerz_cpu_count(void)
{
    static int n;
    int cur = __atomic_load_n(&n, __ATOMIC_RELAXED);
    if (cur)
        return cur;
    size_t sz = sizeof(cur);
    int v = 0;
    if (sysctlbyname("hw.activecpu", &v, &sz, NULL, 0) != 0 || v < 1)
        v = 1;
    __atomic_store_n(&n, v, __ATOMIC_RELAXED);
    return v;
}

static int ocerz_next_cpu_number(void)
{
    static volatile int seq;
    int idx = __atomic_fetch_add(&seq, 1, __ATOMIC_SEQ_CST);
    int ncpu = ocerz_cpu_count();
    return 1 + (idx % (ncpu > 1 ? ncpu - 1 : 1));
}

static void *ocerz_worker_entry(void *p)
{
    struct ocerz_worker *w = (struct ocerz_worker *)p;
    /* Park until the main thread finishes loader-init (preloader release), so a
     * worker never commits its 32-bit stack / returns to PE before that -- the
     * thread-init sequencing real macOS+Wine has. See ocerz_init_gate_*. */
    ocerz_init_gate_wait();
    mach_port_t kp = mach_thread_self();
    w->cpu.gpr[OCERZ_RSI] = kp;
    ocerz_st(w->cpu.gpr[OCERZ_RDI] + 0xf8, 4, (uint64_t)(uint32_t)kp);
    ocerz_vm_run_cpu(w->vm, &w->cpu);
    mach_port_deallocate(mach_task_self(), kp);
    wl_release(w->cpu.wq_workloop_id);
    __atomic_fetch_sub(&g_wq_running, 1, __ATOMIC_SEQ_CST);
    free(w);
    return NULL;
}

static int ocerz_spawn_worker(OcerzVM *vm, const OcerzCPU *tmpl)
{
    ocerz_jit_require_ordered(vm);
    struct ocerz_worker *w = (struct ocerz_worker *)calloc(1, sizeof *w);
    if (!w)
        return -1;
    w->vm = vm;
    w->cpu = *tmpl;
    /* A worker starts with an independent guest stack. Never inherit a host
     * return prediction, especially one captured before JIT retirement. */
    w->cpu.ras_top = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16ull * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int rc = pthread_create(&th, &attr, ocerz_worker_entry, w);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(w);
        return -1;
    }
    return 0;
}

static void ocerz_fork_prepare(void)
{
    ocerz_jit_prefork();
    ocerz_mem_prefork();
}

static void ocerz_fork_parent(void)
{
    ocerz_mem_postfork();
    ocerz_jit_postfork();
}

static void ocerz_fork_child(void)
{
    ocerz_mem_postfork();
    ocerz_jit_postfork();
    __atomic_store_n(&g_wq_running, 0, __ATOMIC_SEQ_CST);
    /* A forked child inherits the parent's already-set-up address space, so its
     * workers must not park on the init gate. A child that goes on to execve a new
     * Wine process re-runs ocerz_mem_init -> ocerz_init_gate_arm and re-arms after
     * this, so re-exec'd Wine children still gate correctly. */
    ocerz_init_gate_release();
}

static int sys_fork(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)a;
    ocerz_jit_require_ordered(vm);
    static int registered;
    if (!registered) {
        registered = 1;
        pthread_atfork(ocerz_fork_prepare, ocerz_fork_parent, ocerz_fork_child);
    }
    pid_t pid = fork();
    if (pid < 0) {
        ret_err(cpu, (uint64_t)errno);
        return OCERZ_STEP_OK;
    }
    if (pid == 0) {
        ret_ok2(cpu, 0, 1);
        return OCERZ_STEP_OK;
    }
    ret_ok2(cpu, (uint64_t)pid, 0);
    return OCERZ_STEP_OK;
}

static const char *ocerz_self_path(void)
{
    static char buf[1024];
    if (!buf[0]) {
        uint32_t sz = sizeof buf;
        if (_NSGetExecutablePath(buf, &sz) != 0)
            buf[0] = 0;
    }
    return buf[0] ? buf : NULL;
}

static int env_inject_lowbase(char **henv, int m, int cap)
{
    static char lowbase_kv[40];
    static char topbase_kv[40];
    if (!ocerz_low_base)
        return m;
    int have_low = 0, have_top = 0;
    for (int i = 0; i < m; i++) {
        if (strncmp(henv[i], "OCERZ_LOWBASE=", 14) == 0)
            have_low = 1;
        if (strncmp(henv[i], "OCERZ_TOPBASE=", 14) == 0)
            have_top = 1;
    }
    if (!have_low && m < cap) {
        snprintf(lowbase_kv, sizeof lowbase_kv, "OCERZ_LOWBASE=%#llx",
                 (unsigned long long)ocerz_low_base);
        henv[m++] = lowbase_kv;
    }
    if (!have_top && m < cap) {
        snprintf(topbase_kv, sizeof topbase_kv, "OCERZ_TOPBASE=%#llx",
                 (unsigned long long)ocerz_top_base);
        henv[m++] = topbase_kv;
    }
    return m;
}

static int sys_posix_spawn(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    const char *self = ocerz_self_path();
    if (!self || !a[1]) {
        ret_err(cpu, EINVAL);
        return OCERZ_STEP_OK;
    }
    /* Same unfaithfulness sys_execve had: the process actually spawned is ocerz ITSELF
     * (`self`), which always exists, so posix_spawn returned 0 for a target that cannot be
     * loaded and the guest was told the spawn succeeded -- the failure surfaced only as the
     * child fataling. The kernel reports it to the CALLER instead. Check first and return
     * the errno posix_spawn would (it returns the error as its VALUE, not via errno). */
    const char *gpath = (const char *)ocerz_g2h(a[1]);
    if (access(gpath, X_OK) != 0) {
        ret_err(cpu, (uint64_t)errno);
        return OCERZ_STEP_OK;
    }
    char *hargv[260];
    int n = 0;
    hargv[n++] = (char *)(uintptr_t)self;
    hargv[n++] = (char *)gpath;
    if (a[3]) {
        uint64_t gv;
        for (uint64_t p = a[3] + 8; n < 258 && (gv = ocerz_ld(p, 8)) != 0; p += 8)
            hargv[n++] = (char *)ocerz_g2h(gv);
    }
    hargv[n] = NULL;
    char *henv[514];
    int m = 0;
    if (a[4]) {
        uint64_t gv;
        for (uint64_t p = a[4]; m < 512 && (gv = ocerz_ld(p, 8)) != 0; p += 8)
            henv[m++] = (char *)ocerz_g2h(gv);
        m = env_inject_lowbase(henv, m, 512);
    }
    henv[m] = NULL;
    pid_t hpid = 0;
    ocerz_jit_require_ordered(vm);
    int rc = posix_spawn(&hpid, self, NULL, NULL, hargv, a[4] ? henv : NULL);
    if (rc != 0) {
        ret_err(cpu, (uint64_t)rc);
        return OCERZ_STEP_OK;
    }
    if (a[0])
        ocerz_st(a[0], 4, (uint64_t)(uint32_t)hpid);
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int shebang_split(const char *path, char *line, size_t cap,
                         const char **interp, const char **arg)
{
    *interp = NULL;
    *arg = NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, line, cap - 1);
    close(fd);
    if (n < 2 || line[0] != '#' || line[1] != '!')
        return 0;
    line[n] = '\0';
    char *nl = strchr(line, '\n');
    if (nl)
        *nl = '\0';
    char *p = line + 2;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return 0;
    *interp = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    if (*p) {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p)
            *arg = p;
    }
    return 1;
}

static int sys_execve(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    const char *self = ocerz_self_path();
    if (!self || !a[0] || !a[1]) {
        ret_err(cpu, EINVAL);
        return OCERZ_STEP_OK;
    }
    const char *gpath = (const char *)ocerz_g2h(a[0]);

    /* A failed execve must be reported to the CALLER, which keeps running -- that is what
     * the kernel does. ocerz implements execve by host-exec'ing ITSELF with -path <target>,
     * so an unloadable target used to be discovered only in the replacement process, after
     * the caller was already gone; ocerz then died with "fatal: cannot read <target>".
     * Wine depends on the real behaviour: it probes lib/wine/i386-unix/wine -- a 32-bit
     * UNIX loader that cannot exist on macOS, which dropped 32-bit support in Catalina --
     * and EXPECTS ENOENT so it can fall back to the 64-bit loader. Turning that expected
     * failure into a fatal killed `wine <program>` outright. Check before replacing the
     * process, and return the same errno the kernel would. */
    if (access(gpath, X_OK) != 0) {
        ret_err(cpu, errno);
        return OCERZ_STEP_OK;
    }

    char *hargv[260];
    int n = 0;
    hargv[n++] = (char *)(uintptr_t)self;
    hargv[n++] = (char *)"-path";

    static char shline[1024];
    const char *interp = NULL, *iarg = NULL;
    if (shebang_split(gpath, shline, sizeof shline, &interp, &iarg)) {
        hargv[n++] = (char *)interp;
        hargv[n++] = (char *)interp;
        if (iarg)
            hargv[n++] = (char *)iarg;
        hargv[n++] = (char *)gpath;
        uint64_t gv;
        for (uint64_t p = a[1] + 8; n < 258 && (gv = ocerz_ld(p, 8)) != 0; p += 8)
            hargv[n++] = (char *)ocerz_g2h(gv);
    } else {
        hargv[n++] = (char *)gpath;
        uint64_t gv;
        for (uint64_t p = a[1]; n < 258 && (gv = ocerz_ld(p, 8)) != 0; p += 8)
            hargv[n++] = (char *)ocerz_g2h(gv);
    }
    hargv[n] = NULL;

    char *henv[514];
    int m = 0;
    if (a[2]) {
        uint64_t gv;
        for (uint64_t p = a[2]; m < 512 && (gv = ocerz_ld(p, 8)) != 0; p += 8)
            henv[m++] = (char *)ocerz_g2h(gv);
        m = env_inject_lowbase(henv, m, 512);
    }
    henv[m] = NULL;

    if (getenv("OCERZ_EXECLOG")) {
        fprintf(stderr, "ocerz: EXECLOG ->");
        for (int k = 0; k < n; k++)
            fprintf(stderr, " %s", hargv[k] ? hargv[k] : "(null)");
        fprintf(stderr, "\n");
    }
    execve(self, hargv, a[2] ? henv : NULL);
    ret_err(cpu, (uint64_t)errno);
    return OCERZ_STEP_OK;
}

#define OCERZ_START_WQTHREAD 0x7ff802e6f80cull
/* _pthread_ptr_munge_token: libpthread xors this into self[0] (the obfuscated self
 * pointer) in _pthread_struct_init/_pthread_wqthread_setup. VERIFIED on this host's
 * cache (OCERZ_WQDIAG probe): 0x7ff8436bd690 reads 0xa3f1c2b4d5e60718, whereas the
 * previously-used 0x7ff8436bd750 (_pthread_keys+0x80) reads 0 -- so ocerz was writing
 * self[0] = pth ^ 0 = pth. That was masked only because _pthread_wqthread_setup
 * re-wrote self[0] correctly on every entry; it becomes LOAD-BEARING now that a reused
 * worker sets WQ_FLAG_THREAD_REUSE and libpthread skips setup entirely. */
#define OCERZ_PTHREAD_COOKIE 0x7ff8436bd690ull

/* Cached env-flag test. getenv() is an environ-lock + linear scan; on the hottest
 * syscall paths -- mach_msg2 (one per WindowServer message) and the workloop-return
 * relay (measured at ~3M calls during a dispatch spin) -- an uncached getenv per call
 * is pure CPU plus cross-thread environ-lock contention (__findenv_locked showed up in
 * live profiles). Every OCERZ_* diagnostic flag is set once at launch and never
 * mutated, so the first lookup is cached. Each expansion site gets its own static. */
#define OCERZ_ENV_ON(name) (__extension__({ static int _env_c = -1; if (_env_c < 0) _env_c = getenv(name) ? 1 : 0; _env_c; }))

static int sys_workq_kernreturn(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    uint64_t op = a[0];
    if (op == 0x4) {
        cpu->terminated = 1;
        ret_ok(cpu, 0);
        return OCERZ_STEP_OK;
    }
    if (op == 0x40 || op == 0x100) {
        /* Re-arm stride: the true 0x48 (see OCERZ_KEVENT_QOS_REARM). OCERZ_REARM40 is a
         * TEST-ONLY escape hatch that forces the old broken 0x40 so the fix can be A/B'd
         * against its regression from a single binary; unset in every real launch. */
        static int rearm40 = -1;
        if (rearm40 < 0)
            rearm40 = getenv("OCERZ_REARM40") != NULL;
        /* The forced value is the LITERAL 0x40, not OCERZ_KEVENT_QOS_S: that used to be a
         * 0x40 #define, but it is now the true 0x48, which silently turned this A/B hatch
         * into a no-op -- both arms were measuring the same thing. */
        const uint64_t rstride = rearm40 ? (uint64_t)0x40 : OCERZ_KEVENT_QOS_REARM;
        /* KEVENT (0x40) / WORKLOOP (0x100) worker completion: the guest
         * _pthread_wqthread issues this after its drain, passing its OUTGOING
         * re-arm changelist (a[1]=keventlist, a[2]=nkevents). For a HOSTWQ inline
         * worker, copy that changelist back into the host kernel's events buffer so
         * the callback returns it and the host _pthread_wqthread re-arms the
         * level-triggered EV_DISPATCH sources -- WITHOUT this the callbacks forced
         * *nevents=0, so a readable source re-fired forever and starved the main
         * thread in __ulock_wait. Do NOT forward to the host kernel: the real host
         * _pthread_wqthread does that once our callback returns. */
        void **evp = g_hostwq_tl_events;
        int   *nvp = g_hostwq_tl_nevents;
        if (evp && nvp) {                       /* set only on an inline HOSTWQ worker */
            int n = (int)(int64_t)a[2];
            /* nkevents == -1 is libpthread's "terminate this thread" convention, not a
             * changelist; re-arm nothing. */
            if (n < 0) n = 0;
            /* The outgoing re-arm changelist is bounded by the KERNEL BUFFER's capacity,
             * NOT by how many events were delivered. The delivered count is not a
             * capacity and never was -- capping on it was a number ocerz invented, and
             * it silently discarded real re-arms.
             *
             * The contract, read out of the shipping libdispatch (_dispatch_event_loop_merge):
             *     movw $0xe, 0x32(%r15)     ; r15 = gs:0xe8 = the deferred-items struct
             * ddi_maxevents is set to 14 UNCONDITIONALLY, before the drain loop and with no
             * reference to the delivered count. So libdispatch is entitled to return up to
             * 14 re-arm entries off a 1-event delivery -- MEASURED here as n=2..3, and the
             * dropped entry was routinely the EVFILT_WORKLOOP (-17) re-arm itself, which is
             * exactly how a workloop got armed 141 times and never drained: the guest DID
             * drain and asked to re-arm, and ocerz threw the request away.
             *
             * 16 is the kernel's WQ_KEVENT_LIST_LEN, the real capacity of the buffer the
             * kernel hands the callback (ocerz already independently assumes it via its own
             * `if (nev > 16) nev = 16` in the bridge). Cap at 16, not at 14: 14 is an
             * implementation constant of THIS libdispatch build, whereas 16 is the buffer
             * contract. The guest evbuf is 0x8000 bytes and 14 * 0x48 = 0xA20, so neither
             * side is short of space -- there was never a reason to truncate.
             *
             * The prior "returning the full changelist exploded worker churn ~14000x
             * (WQ-ENTER 97 -> 1.4M)" measurement was real, but its cause was the stride bug
             * above, not a semantic mismatch: with OCERZ_KEVENT_QOS_S = 0x40 every entry
             * from index 1 on was read 8 bytes short of where the guest wrote it, so the
             * kernel was handed garbage kevents, errored them, and re-fired immediately. The
             * cap was a brake bolted onto a broken constant. It is only safe to lift AFTER
             * the stride is correct -- that dependency is real. */
            if (OCERZ_ENV_ON("OCERZ_KRLOG")) {
                fprintf(stderr, "ocerz: KRET cpu#%u op=%#llx n=%d evcap=%d%s |",
                        cpu->cpu_number, (unsigned long long)op, n, g_hostwq_tl_evcap,
                        n > g_hostwq_tl_evcap ? "  **TRUNCATED**" : "");
                for (int i = 0; i < n && i < 8; i++) {
                    uint64_t k = a[1] + (uint64_t)i * rstride;
                    uint64_t wl = ocerz_ld(k + 0x00, 8);
                    fprintf(stderr, " [%d]{id=%#llx filt=%d fl=%#llx fflags=%#llx dqs+38=%#llx}%s", i,
                            (unsigned long long)wl,
                            (int)(int16_t)ocerz_ld(k + 0x08, 2),
                            (unsigned long long)ocerz_ld(k + 0x0a, 2),
                            (unsigned long long)ocerz_ld(k + 0x18, 4),
                            (unsigned long long)(wl ? ocerz_ld(wl + 0x38, 8) : 0),  /* workloop dq_state: low 32b = drain-lock owner tid */
                            i >= g_hostwq_tl_evcap ? "<<DROPPED" : "");
                }
                fprintf(stderr, "\n");
            }
            /* Safety clamp only. Per the contract above (libdispatch caps itself at
             * ddi_maxevents=14 < the kernel buffer's 16) this can NEVER fire, so it is loud
             * rather than silent: if it ever does, the model here is wrong -- most likely a
             * future libdispatch raised maxevents past WQ_KEVENT_LIST_LEN -- and truncating
             * would resume silently dropping real re-arms. Do not downgrade this to a quiet
             * min(). */
            if (n > g_hostwq_tl_evcap) {
                fprintf(stderr,
                        "ocerz: BUG: workq_kernreturn re-arm list n=%d exceeds kernel buffer "
                        "capacity %d (op=%#llx) -- libdispatch's ddi_maxevents contract is "
                        "broken on this OS build; re-arms are being DROPPED\n",
                        n, g_hostwq_tl_evcap, (unsigned long long)op);
                n = g_hostwq_tl_evcap;
            }
            void *hbuf = *evp;
            if (hbuf && a[1]) {
                for (int i = 0; i < n; i++)
                    memcpy((char *)hbuf + (size_t)i * rstride,
                           ocerz_g2h(a[1] + (uint64_t)i * rstride),
                           rstride);
            } else {
                n = 0;
            }
            *nvp = n;                           /* host re-arms n sources */
            g_hostwq_tl_events = NULL;          /* consume once */
            g_hostwq_tl_nevents = NULL;
        }
        cpu->terminated = 1;                    /* park the inline worker cleanly */
        ret_ok(cpu, 0);
        return OCERZ_STEP_OK;
    }
    if (op == 0x20) {
        /* Under HOSTWQ_ASYNC, hand root-queue worker requests to the REAL kernel
         * (which then drives ocerz_hostwq_queue_cb) instead of spawning a
         * synthetic worker, so all worker management -- workloop (kevent_id),
         * event (kevent), and async (here) -- goes through one kernel-owned pool
         * with correct QoS/lifecycle, the same as a native process. */
        if (OCERZ_ENV_ON("OCERZ_HOSTWQ") && OCERZ_ENV_ON("OCERZ_HOSTWQ_ASYNC")) {
            ocerz_hostwq_register(vm);
            uint64_t fa[8];
            memcpy(fa, a, sizeof fa);
            uint64_t r2 = 0;
            int err = 0;
            ocerz_host_syscall(368, fa, &r2, &err);
            ret_ok(cpu, 0);
            return OCERZ_STEP_OK;
        }
        int reqcount = (int)a[2];
        if (reqcount < 1)
            reqcount = 1;
        uint64_t prio = a[3];
        uint64_t cookie = ocerz_ld(OCERZ_PTHREAD_COOKIE, 8);
        int running = __atomic_load_n(&g_wq_running, __ATOMIC_SEQ_CST);
        int want = reqcount - running;
        if (want > 8)
            want = 8;
        for (int i = 0; i < want; i++) {
            uint64_t region = ocerz_map_anywhere(0x200000, PROT_READ | PROT_WRITE);
            if (region == 0)
                break;
            __atomic_fetch_add(&g_wq_running, 1, __ATOMIC_SEQ_CST);
            uint64_t pth = region + 0x1f0000;
            ocerz_st(pth, 8, pth ^ cookie);
            ocerz_st(pth + 0xe0, 8, pth);
            OcerzCPU t = *cpu;
            t.terminated = 0;
            t.cpu_number = ocerz_next_cpu_number();
            t.rip = OCERZ_START_WQTHREAD;
            uint32_t qosbits = (uint32_t)((prio >> 8) & 0x3fff);
            int qos_idx = qosbits ? (__builtin_ctz(qosbits) + 1) : 4;
            if (qos_idx < 1)
                qos_idx = 1;
            if (qos_idx > 6)
                qos_idx = 6;
            t.gpr[OCERZ_RSP] = pth - 0x100;
            t.gpr[OCERZ_RDI] = pth;
            t.gpr[OCERZ_RSI] = 0;
            t.gpr[OCERZ_RDX] = pth;
            t.gpr[OCERZ_RCX] = 0;
            t.gpr[OCERZ_R8] = 0x40000ull | 0x200000ull | 0x4000ull | (uint64_t)qos_idx;
            t.gpr[OCERZ_R9] = 0;
            t.gs_base = pth + 0xe0;
            t.sig_altstack_sp = 0;
            t.sig_altstack_size = 0;
            t.sig_mask = 0;
            t.sig_on_stack = 0;
            t.sig_last_fault = 0;
            t.sig_repeat = 0;
            if (ocerz_spawn_worker(vm, &t) != 0) {
                __atomic_fetch_sub(&g_wq_running, 1, __ATOMIC_SEQ_CST);
                break;
            }
        }
        ret_ok(cpu, 0);
        return OCERZ_STEP_OK;
    }
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

/* WQ_FLAG_THREAD_* word handed to _pthread_wqthread (guest R8). Re-decoded from the
 * cache's _pthread_wqthread test-branches (scratchpad/wqdecode.c on this host):
 * bit14(0x4000)=QoS-class-in-low-byte; bit22(0x400000)=WORKLOOP worker (tested FIRST)
 * -> _dispatch_workloop_worker_thread(&workloop_id,&events,&nevents), workq_kernreturn
 * op 0x100, and it reads &workloop_id at keventlist-8 (=evbuf-8); bit19(0x80000)=KEVENT
 * worker -> _dispatch_kevent_worker_thread(&events,&nevents), op 0x40; neither bit ->
 * the plain _dispatch_worker_thread2, op 0x4, draining only the root concurrent queue.
 * bit18(0x40000)=NEWSPI + bit21(0x200000)=OUTSIDEQOS stay in BASE.
 * FIX: the previous OCERZ_WQ_FLAG_WORKLOOP=0x80000 was actually the KEVENT bit, so the
 * workloop callback mis-routed to the kevent drain and dropped the kernel-supplied
 * workloop_id. A WORKLOOP thread carries BOTH WORKLOOP|KEVENT (workloop is a kevent
 * specialization); bit22 is tested first so it wins. */
#define OCERZ_WQ_FLAG_BASE   (0x40000ull | 0x200000ull | 0x4000ull)
#define OCERZ_WQ_FLAG_WORKLOOP 0x400000ull   /* bit22 */
#define OCERZ_WQ_FLAG_KEVENT   0x80000ull    /* bit19 */
/* bit17: THREAD_REUSE. A real kernel workqueue thread is registered with libpthread
 * exactly ONCE; every later re-dispatch of that parked/recycled thread carries REUSE so
 * _pthread_wqthread SKIPS _pthread_wqthread_setup. ocerz's inline worker is structurally
 * a recycled thread (one guest pth per HOST wq thread, reused across callbacks), but it
 * presented REUSE=0 every time, so setup re-ran TAILQ_INSERT_TAIL(&__pthread_head, self)
 * on an ALREADY-linked node -> self->next == self (self-cycle) and _pthread_count grew
 * without bound (MEASURED: count tracked the callback count 1:1, 2->167 in 20s, with
 * selfcycle observed). pthread_from_mach_thread_np walks that list under
 * _pthread_list_lock with no cycle guard, so a non-matching lookup spins forever holding
 * the lock -- starving every later pthread create/exit. */
#define OCERZ_WQ_FLAG_REUSE    0x20000ull    /* bit17 */

/* libpthread dispatch-callback table workloop slot: holds the registered
 * _dispatch_workloop_worker_thread pointer, or 0 if the guest opted into the
 * kevent-only workqueue model (bsdthread_register did not advertise WORKLOOP). Read
 * at routing time so a NULL slot falls back to the kevent drain instead of null-calling. */
#define OCERZ_PTHREAD_WORKLOOP_SLOT 0x7ff8436bd638ull

#define OCERZ_EVFILT_WORKLOOP (-17)

/* Spawn one workqueue worker entering _pthread_wqthread in WORKLOOP mode to
 * service the workloop the guest just woke. The kernel hands such a worker ONLY
 * the EVFILT_WORKLOOP thread-request event -- never the workloop's source
 * registrations -- so the worker drains the workloop's enqueued blocks without
 * trying to receive from a mach-port source that has no message (echoing the
 * EVFILT_MACHPORT registrations made _dispatch trip "Unexpected error from mach
 * recv"). We therefore copy only the EVFILT_WORKLOOP (-17) changelist entries
 * into the worker's event list (EV_ADD/EV_ENABLE cleared to fired form), take
 * the QoS from the first of them, and skip the spawn entirely when the commit
 * carries no thread request (a pure source registration). The events live above
 * the worker's pthread struct (pth+0x8000, in the region's top slack, clear of
 * the down-growing stack at pth). */
static int ocerz_spawn_workloop_worker(OcerzVM *vm, const OcerzCPU *cpu,
                                       uint64_t workloop_id, uint64_t changelist, int nchanges)
{
    if (nchanges > 16)
        nchanges = 16;
    int has_wl = 0;
    for (int i = 0; i < nchanges; i++)
        if ((int16_t)ocerz_ld(changelist + (uint64_t)i * OCERZ_KEVENT_QOS_S + 8, 2)
            == OCERZ_EVFILT_WORKLOOP) {
            has_wl = 1;
            break;
        }
    if (!has_wl)
        return -1;
    uint64_t cookie = ocerz_ld(OCERZ_PTHREAD_COOKIE, 8);
    uint64_t region = ocerz_map_anywhere(0x200000, PROT_READ | PROT_WRITE);
    if (region == 0)
        return -1;
    uint64_t pth = region + 0x1f0000;
    uint64_t evbuf = pth + 0x8000;
    ocerz_st(evbuf - 8, 8, workloop_id);   /* &workloop_id = keventlist-8 for the workloop drain */
    int nev = 0;
    uint64_t prio = 0;
    for (int i = 0; i < nchanges; i++) {
        uint64_t src = changelist + (uint64_t)i * OCERZ_KEVENT_QOS_S;
        if ((int16_t)ocerz_ld(src + 8, 2) != OCERZ_EVFILT_WORKLOOP)
            continue;
        uint64_t dst = evbuf + (uint64_t)nev * OCERZ_KEVENT_QOS_S;
        for (uint64_t off = 0; off < OCERZ_KEVENT_QOS_S; off += 8)
            ocerz_st(dst + off, 8, ocerz_ld(src + off, 8));
        uint16_t fl = (uint16_t)ocerz_ld(dst + 0x0a, 2);
        fl &= (uint16_t) ~(uint16_t)(0x0001u | 0x0004u);
        ocerz_st(dst + 0x0a, 2, fl);
        if (nev == 0)
            prio = (uint32_t)ocerz_ld(src + 0x0c, 4);
        nev++;
    }
    int nchanges_out = nev;
    __atomic_fetch_add(&g_wq_running, 1, __ATOMIC_SEQ_CST);
    ocerz_st(pth, 8, pth ^ cookie);
    ocerz_st(pth + 0xe0, 8, pth);
    OcerzCPU t = *cpu;
    t.terminated = 0;
    t.cpu_number = ocerz_next_cpu_number();
    t.rip = OCERZ_START_WQTHREAD;
    for (int r = 0; r < 16; r++)
        t.gpr[r] = 0;
    uint32_t qosbits = (uint32_t)((prio >> 8) & 0x3fff);
    int qos_idx = qosbits ? (__builtin_ctz(qosbits) + 1) : 4;
    if (qos_idx < 1)
        qos_idx = 1;
    if (qos_idx > 6)
        qos_idx = 6;
    t.gpr[OCERZ_RSP] = pth - 0x100;
    t.gpr[OCERZ_RDI] = pth;
    t.gpr[OCERZ_RSI] = 0;
    t.gpr[OCERZ_RDX] = pth;
    t.gpr[OCERZ_RCX] = evbuf;
    t.gpr[OCERZ_R8] = OCERZ_WQ_FLAG_BASE | OCERZ_WQ_FLAG_WORKLOOP | OCERZ_WQ_FLAG_KEVENT | (uint64_t)qos_idx;
    t.gpr[OCERZ_R9] = (uint64_t)nchanges_out;
    t.gs_base = pth + 0xe0;
    t.wq_workloop_id = workloop_id;
    t.sig_altstack_sp = 0;
    t.sig_altstack_size = 0;
    t.sig_mask = 0;
    t.sig_on_stack = 0;
    t.sig_last_fault = 0;
    t.sig_repeat = 0;
    if (ocerz_spawn_worker(vm, &t) != 0) {
        __atomic_fetch_sub(&g_wq_running, 1, __ATOMIC_SEQ_CST);
        return -1;
    }
    return 0;
}

/* FAITHFUL workqueue path (OCERZ_HOSTWQ): instead of ocerz emulating the
 * kernel's thread-request and hand-building events, register OUR OWN host
 * (arm64) workqueue worker callbacks with the REAL kernel and forward the
 * guest's kevent ops to it. The kernel then decides when to bring up a worker,
 * delivers a FAITHFUL kevent it owns, and manages the workloop's serial drain
 * ownership -- everything the synthetic worker got subtly wrong. Each kernel-
 * spawned host worker thread enters one of these callbacks, which bridges into
 * the guest emulator at the guest's start_wqthread with the real event. Phase 1
 * here only logs, to confirm the kernel actually calls back. */
typedef unsigned long ocerz_pthread_priority_t;
extern int _pthread_workqueue_init_with_workloop(
    void (*queue_func)(ocerz_pthread_priority_t),
    void (*kevent_func)(void **events, int *nevents),
    void (*workloop_func)(uint64_t *workloop_id, void **events, int *nevents),
    int offset, int flags);

static OcerzVM *g_hostwq_vm;

/* Bridge a kernel-spawned host worker thread into the guest: build a guest
 * worker context, copy the kernel-delivered kevent array into guest memory, and
 * run the guest emulator inline from the guest's start_wqthread. The guest's
 * _pthread_wqthread -> _dispatch_*_worker_thread then drains FAITHFULLY off the
 * kernel's own event. When the guest worker finishes (its start_wqthread ud2,
 * handled as a clean terminate in interp.c), ocerz_vm_run_cpu returns and we
 * return to libpthread, which parks/recycles this host thread -- so serial-
 * ownership and park-and-reuse are the real kernel's job, not ours. */
static __thread uint64_t g_hostwq_tl_region;
/* Set when the previous worker recycled onto THIS host thread died via STEP_FATAL
 * (wrc==125) mid-drain: it left libdispatch's per-drain TSD dangling in the reused
 * region (gs:0xe8 _dispatch_deferred_items into the now-dead guest stack, gs:0xd8
 * adopted wlh). A real macOS wqthread cannot be in this state -- a genuine mid-drain
 * abort is process-fatal, so a recycled WQ_FLAG_REUSE thread ALWAYS re-enters with
 * those == 0. The next worker on this host thread consumes the flag and restores that
 * empty sentinel (see ocerz_hostwq_bridge / ocerz_hostwq_queue_cb). */
static __thread int g_hostwq_tl_fatal;
/* A/B toggle for the pthread stack-bounds write (pth+0xb0/pth+0xb8). Default ON
 * (bounds written, fixes the malloc_vreport backtrace-overrun seed); OCERZ_NO_STACKBOUNDS=1
 * skips them to test whether writing those fields regresses worker responsiveness. */
static int ocerz_no_stackbounds(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_NO_STACKBOUNDS") ? 1 : 0;
    return v;
}

/* Bridge ONE kernel-delivered Mach message into guest memory (UPDATE #23, Bug A).
 * An EVFILT_MACHPORT receive event the host kernel hands a host workqueue worker carries
 * ext0 = a message buffer the kernel allocated IN OCERZ'S HOST address space; guest
 * libdispatch reads ext0 as a GUEST address -> unmapped -> SIGSEGV (-> the gs:0x320 fatal
 * loop, since Wine's segv_handler can't run on a no-TEB worker). The macOS kernel would
 * have placed the message in the process's own AS; ocerz must do the same. Copy the message
 * into a fresh guest mapping and return its guest address. ocerz + host share one Mach port
 * namespace, so port NAMES inside need no translation; but a COMPLEX message's OOL / OOL-ports
 * descriptors point at OOL data the kernel placed in the HOST AS -- relocate each into guest
 * memory and patch the descriptor so the guest can read it. The guest later
 * vm_deallocate()s the copy (verified: gbuf addresses recycle), exactly as on real macOS. */
static uint64_t ocerz_bridge_mach_msg(uint64_t hbuf, uint64_t sz)
{
    if (!hbuf || !sz || sz > 0x100000)
        return 0;
    /* A message delivered this way can be followed by AUXILIARY data in the same buffer, and
     * the guest addresses it as msg + msg_size -- ext1 is the size of the MESSAGE, not of what
     * the kernel wrote. Copying ext1 bytes therefore stops exactly at the first byte of the
     * aux, and the guest reads the fresh zero page behind the copy instead: libdispatch sees
     * msgdh_size == 0 and takes its "BUG IN LIBDISPATCH: Invalid msg aux data size" abort,
     * killing the worker. Observed directly -- aux at msg+0x23c against a copy of 0x23c bytes.
     * The aux is self-describing (first word = total aux length, header included), so read
     * that length, bound it, and carry the aux across at the same relative offset. */
    uint64_t total = sz;
    if (!getenv("OCERZ_NO_AUXTAIL")) {          /* A/B hatch for the paragraph above */
        /* Validate this really is a mach_msg_aux_header_t before believing its length. The
         * probe reads the first word ABOVE the message, and for the top-most message in the
         * kernel's kevent data buffer that word is the adjacent kevent list -- where a mach
         * port name (0x2ff, say) sails through a bare "8 <= n <= 0x4000" test and drags an
         * arbitrary slab of host bytes into the guest copy. A real header is 8-byte aligned
         * and has a zero reserved word, which a port name pair is not. */
        uint32_t aux[2] = { 0, 0 };
        memcpy(aux, (const void *)(uintptr_t)(hbuf + sz), sizeof aux);
        if (aux[0] >= 8 && aux[0] <= 0x4000 && (aux[0] & 7u) == 0 && aux[1] == 0)
            total = sz + aux[0];
    }
    uint64_t gbuf = ocerz_map_anywhere((total + 0x3fffull) & ~0x3fffull, PROT_READ | PROT_WRITE);
    if (!gbuf)
        return 0;
    unsigned char *g = (unsigned char *)ocerz_g2h(gbuf);
    memcpy(g, (const void *)(uintptr_t)hbuf, (size_t)total);
    static int g_mm_log = -1;
    if (g_mm_log < 0) g_mm_log = getenv("OCERZ_MACHMSG") ? 1 : 0;
    int mm_ool = 0, mm_oolfail = 0, mm_unhandled = 0;
    uint32_t bits;
    memcpy(&bits, g, 4);
    if (bits & 0x80000000u) { /* MACH_MSGH_BITS_COMPLEX */
        uint32_t dcnt;
        memcpy(&dcnt, g + 0x18, 4);
        uint64_t off = 0x1c;
        for (uint32_t d = 0; d < dcnt && off + 12 <= sz; d++) {
            uint8_t type = g[off + 11];
            if (type == 0) { off += 12; continue; }      /* PORT: name only (shared namespace) */
            if (type == 4) { off += 16; continue; }      /* GUARDED_PORT: name+guard, no OOL data */
            if (type == 1 || type == 2 || type == 3) {   /* OOL / OOL_PORTS / OOL_VOLATILE */
                if (off + 16 > sz)
                    break;
                uint64_t ool_addr;
                uint32_t ool_n;
                memcpy(&ool_addr, g + off, 8);
                memcpy(&ool_n, g + off + 12, 4);
                /* type 2 = OOL_PORTS: the +12 field is a COUNT of port names (4 bytes each). */
                uint64_t bytes = (type == 2) ? (uint64_t)ool_n * 4u : ool_n;
                if (g_mm_log)
                    fprintf(stderr, "ocerz: BRIDGE-OOL type=%u ool_addr=%#llx bytes=%#llx%s\n",
                            type, (unsigned long long)ool_addr, (unsigned long long)bytes,
                            bytes > 0x1000000 ? "  **OVER-16MB-SKIPPED**" : "");
                if (ool_addr && bytes && bytes <= 0x1000000) {
                    uint64_t ogb = ocerz_map_anywhere((bytes + 0x3fffull) & ~0x3fffull,
                                                      PROT_READ | PROT_WRITE);
                    if (ogb) {
                        memcpy(ocerz_g2h(ogb), (const void *)(uintptr_t)ool_addr, (size_t)bytes);
                        memcpy(g + off, &ogb, 8); /* patch descriptor addr -> guest copy */
                        mm_ool++;
                    } else {
                        /* reloc alloc failed: deliver an EMPTY OOL (addr=0,size=0) rather than
                         * leave a host pointer the guest would fault on. */
                        uint64_t z = 0;
                        uint32_t z4 = 0;
                        memcpy(g + off, &z, 8);
                        memcpy(g + off + 12, &z4, 4);
                        mm_oolfail++;
                    }
                }
                off += 16;
                continue;
            }
            if (g_mm_log)
                fprintf(stderr, "ocerz: BRIDGE unknown desc type=%u off=%#llx dcnt=%u (walk aborts; rest un-relocated)\n",
                        type, (unsigned long long)off, dcnt);
            mm_unhandled = 1;
            break; /* type > 4: unknown stride, cannot safely continue */
        }
    }
    if (g_mm_log) {
        uint32_t msgh_size = 0, msgh_id = 0, dcnt2 = 0;
        memcpy(&msgh_size, g + 4, 4);
        memcpy(&msgh_id, g + 0x14, 4);
        if (bits & 0x80000000u) memcpy(&dcnt2, g + 0x18, 4);
        fprintf(stderr, "ocerz: BRIDGE-MSG recv_sz=%#llx msgh_size=%#x bits=%#x id=%u dcnt=%u ool=%d oolfail=%d unhandled=%d%s\n",
                (unsigned long long)sz, msgh_size, bits, msgh_id, dcnt2,
                mm_ool, mm_oolfail, mm_unhandled,
                (msgh_size > sz ? " **MSGH_SIZE>RECV**" : ""));
    }
    /* FOLLOW-UP (deferred per adversarial review): the kernel's host message buffer (hbuf) and
     * any host OOL backings are NOT vm_deallocate'd here -> a slow host-AS leak (~msg-size per
     * received Mach message). Freeing them (memory only, never mach_msg_destroy -- port names
     * persist in the shared namespace and the guest copy references them) is the correct
     * ownership, but is held back until it can be validated at the deep dispatch phase without
     * destabilizing the verified-working bridge. */
    return gbuf;
}

/* Log a forwarded mach_msg SEND the kernel rejected with a MACH_SEND_* error (gated on
 * OCERZ_MACHMSG). gmsg is the GUEST message buffer (pre-translation header), so descriptor
 * addresses are the guest-side values the kernel could not copyin. */
/* Mach errors worth reporting. The old test was (r & 0xfffff000) == 0x10000000, which
 * matches only the SEND class -- every RCV-class failure (0x10004xxx) was invisible,
 * including MACH_RCV_INVALID_DATA and MACH_RCV_BODY_ERROR. A receive that fails is exactly
 * how CoreFoundation's run loop dies, so this blindness is why such a failure could survive
 * repeated investigation. MACH_RCV_TIMED_OUT and MACH_RCV_TOO_LARGE are normal control flow
 * and stay quiet. */
static int ocerz_mach_err_interesting(uint64_t r)
{
    if ((r & 0xfffff000ull) == 0x10000000ull)
        return 1;                                   /* send class */
    if ((r & 0xfffff000ull) == 0x10004000ull)       /* receive class */
        return r != 0x10004003ull && r != 0x10004004ull;
    return 0;
}

/* Guest nanoseconds -> host mach ticks. The guest clock is ns (see case 89); the host is
 * Apple silicon ticks. Cached because it is on the timer path. A zero/!=OK timebase leaves
 * the value alone rather than mangling it. */
static uint64_t ocerz_guest_ns_to_host_ticks(uint64_t ns)
{
    static uint32_t numer, denom;
    if (!denom) {
        mach_timebase_info_data_t tb;
        if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.numer == 0 || tb.denom == 0) {
            numer = denom = 1;
        } else {
            numer = tb.numer;
            denom = tb.denom;
        }
    }
    if (numer == denom || ns == 0)
        return ns;
    return (uint64_t)(((__uint128_t)ns * denom) / numer);
}

static void ocerz_log_mach_send_err(int trap, uint64_t kr, const uint64_t *a, uint64_t gmsg)
{
    static int slog = -1;
    if (slog < 0) slog = getenv("OCERZ_MACHMSG") ? 1 : 0;
    if (!slog) return;
    /* For mach_msg2 (trap 47) the AUTHORITATIVE header is register-packed: a[2]=bits|(size<<32),
     * a[3]=remote|(local<<32), a[5]=desc_count|(rcv_name<<32). For mach_msg (trap 31) it lives in
     * the buffer at gmsg+0. Report both the register view and the in-buffer view + a body hexdump. */
    uint32_t rbits = (uint32_t)(a[2] & 0xffffffffu);
    uint32_t rsize = (uint32_t)(a[2] >> 32);
    uint32_t rdcnt = (uint32_t)(a[5] & 0xffffffffu);
    int complex = (trap == 47) ? ((rbits & 0x80000000u) != 0)
                               : (((uint32_t)ocerz_ld(gmsg, 4) & 0x80000000u) != 0);
    uint32_t dc = complex ? ((trap == 47) ? rdcnt : (uint32_t)ocerz_ld(gmsg + 0x18, 4)) : 0;
    fprintf(stderr,
            "ocerz: MACH-SEND-ERR(t%d)[%d] kr=%#llx opt=%#llx REG{bits=%#x size=%#x ports=%#llx dcnt=%u} BUF{bits=%#x size=%#x} complex=%d",
            trap, (int)getpid(), (unsigned long long)kr, (unsigned long long)a[1],
            rbits, rsize, (unsigned long long)a[3], rdcnt,
            (uint32_t)ocerz_ld(gmsg, 4), (uint32_t)ocerz_ld(gmsg + 4, 4), complex);
    if (dc > 8) dc = 8;
    for (uint32_t di = 0; di < dc; di++) {
        uint64_t doff = gmsg + 0x1c + (uint64_t)di * 16;
        uint64_t da = ocerz_ld(doff, 8);
        uint8_t dt = (uint8_t)ocerz_ld(doff + 11, 1);
        fprintf(stderr, " d%u{addr=%#llx type=%u committed=%d low=%d}", di,
                (unsigned long long)da, dt, ocerz_addr_committed(da), da < OCERZ_LOW_LIMIT);
    }
    fprintf(stderr, " buf:");
    for (int k = 0; k < 0x30; k += 8)
        fprintf(stderr, " %016llx", (unsigned long long)ocerz_ld(gmsg + (uint64_t)k, 8));
    fprintf(stderr, "\n");
}

struct ocerz_ool_save { uint64_t off; uint64_t orig; };

/* SEND-side mirror of ocerz_bridge_mach_msg (which relocates inbound OOL host->guest): for an
 * OUTGOING flat COMPLEX Mach message, rewrite each OOL/OOL_PORTS/OOL_VOLATILE descriptor's 8-byte
 * .address field from the guest value to its host mapping (ocerz_g2h), so the kernel's copyin reads
 * the correct host backing -- exactly what an in-process macOS send presents to the kernel. ocerz
 * shares the host port namespace, so PORT (type 0) / GUARDED_PORT (type 4) names need no change.
 * Identity-arena addresses (host==guest) are left untouched (no save -> restore is a no-op). gmsg is
 * the GUEST buffer (flat layout: msgh_bits@0, descriptor_count@0x18, descriptors@0x1c; bound by
 * send_size). Returns the count of translated descriptors recorded in saved[] for later restore. */
static int g_sendxlate_off = -1;

static int ocerz_send_xlate_descriptors(uint64_t gmsg, uint32_t send_size,
                                        struct ocerz_ool_save *saved, int max_saved)
{
    if (g_sendxlate_off < 0)
        g_sendxlate_off = getenv("OCERZ_NO_SENDXLATE") ? 1 : 0;
    if (g_sendxlate_off || gmsg == 0)
        return 0;
    uint32_t bits = (uint32_t)ocerz_ld(gmsg, 4);
    if (!(bits & 0x80000000u))               /* not COMPLEX (also naturally skips vector buffers) */
        return 0;
    uint32_t dcnt = (uint32_t)ocerz_ld(gmsg + 0x18, 4);
    if (dcnt == 0 || dcnt > 4096)
        return 0;
    uint64_t off = 0x1c;
    int n = 0;
    for (uint32_t d = 0; d < dcnt; d++) {
        if (send_size && off + 12 > send_size)
            break;
        uint8_t type = (uint8_t)ocerz_ld(gmsg + off + 11, 1);
        if (type == 0) { off += 12; continue; }   /* PORT: name only (shared namespace) */
        if (type == 1 || type == 2 || type == 3) { /* OOL / OOL_PORTS / OOL_VOLATILE: addr@+0 */
            if (send_size && off + 16 > send_size)
                break;
            uint64_t ga = ocerz_ld(gmsg + off, 8);
            if (ga != 0) {
                uint64_t ha = (uint64_t)(uintptr_t)ocerz_g2h(ga);
                if (ha != ga && n < max_saved) {  /* non-identity: translate + remember for restore */
                    saved[n].off = off;
                    saved[n].orig = ga;
                    n++;
                    ocerz_st(gmsg + off, 8, ha);
                }
            }
            off += 16;
            continue;
        }
        if (type == 4) { off += 16; continue; }   /* GUARDED_PORT: name+guard, no OOL addr */
        break;                                    /* unknown stride: stop */
    }
    return n;
}

/* Restore the guest-side OOL addresses after the trap so the guest buffer is byte-identical to what
 * it built (the guest still owns / will deallocate those regions with their GUEST addresses). Only
 * restore a field still holding OUR host value: a mach_msg2 send+receive overwrites the buffer with
 * the reply, and that reply must be left intact. */
static void ocerz_send_restore_descriptors(uint64_t gmsg, const struct ocerz_ool_save *saved, int n)
{
    for (int i = 0; i < n; i++) {
        uint64_t ha = (uint64_t)(uintptr_t)ocerz_g2h(saved[i].orig);
        if (ocerz_ld(gmsg + saved[i].off, 8) == ha)
            ocerz_st(gmsg + saved[i].off, 8, saved[i].orig);
    }
}

/* SEND-side translation for a MACH64_MSG_VECTOR mach_msg2 (opt bit 32): a[0] points to `count`
 * mach_msg_vector_t entries {msgv_data@0(8), msgv_rcv_addr@8(8), msgv_send_size@0x10(4),
 * msgv_rcv_size@0x14(4)} = 24 bytes. The kernel copyins each segment from msgv_data and writes the
 * reply to msgv_rcv_addr -- both are process-AS pointers, so translate any non-identity one
 * guest->host (the low-shadow segment pointer is what fails the copyin with MACH_SEND_INVALID_DATA).
 * Also translate OOL descriptors inside the first (control) segment, which carries the header+body.
 * Offsets in saved[] are relative to gvec so ocerz_send_restore_descriptors restores them. */
static int ocerz_send_xlate_vector(uint64_t gvec, uint32_t count, int sending, int receiving,
                                   struct ocerz_ool_save *saved, int max_saved)
{
    if (g_sendxlate_off < 0)
        g_sendxlate_off = getenv("OCERZ_NO_SENDXLATE") ? 1 : 0;
    if (g_sendxlate_off || gvec == 0 || count == 0 || count > 64)
        return 0;
    static int vlog = -1;
    if (vlog < 0) vlog = getenv("OCERZ_MACHMSG") ? 1 : 0;
    uint64_t seg0 = ocerz_ld(gvec, 8);          /* control segment (guest) -- read before translating */
    uint32_t seg0_size = (uint32_t)ocerz_ld(gvec + 0x10, 4);
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t e = gvec + (uint64_t)i * 24;
        if (vlog && i < 4)
            fprintf(stderr, "ocerz: VEC[%u] data=%#llx rcv_addr=%#llx ssize=%#x rsize=%#x\n", i,
                    (unsigned long long)ocerz_ld(e, 8), (unsigned long long)ocerz_ld(e + 8, 8),
                    (uint32_t)ocerz_ld(e + 0x10, 4), (uint32_t)ocerz_ld(e + 0x14, 4));
        /* msgv_data is read by the send half, msgv_rcv_addr written by the receive half.
         * Translate each only for the direction that actually uses it, so a field left
         * uninitialised by the caller for the unused direction is never rewritten. */
        for (int f = 0; f < 16; f += 8) {
            if (f == 0 ? !sending : !receiving)
                continue;
            uint64_t ga = ocerz_ld(e + f, 8);
            if (ga == 0)
                continue;
            uint64_t ha = (uint64_t)(uintptr_t)ocerz_g2h(ga);
            if (ha != ga && n < max_saved) {
                saved[n].off = (e + (uint64_t)f) - gvec;
                saved[n].orig = ga;
                n++;
                ocerz_st(e + f, 8, ha);
            }
        }
    }
    /* OOL descriptors in the control segment (header+body live there for a vector send). Walk on the
     * segment's ORIGINAL guest pointer; record restores relative to gvec by adding (seg0 - gvec).
     * Guard on the segment being committed so a non-pointer/garbage msgv_data never faults the walk. */
    if (sending && seg0 != 0 && ocerz_addr_committed(seg0) == 1) {
        struct ocerz_ool_save segsv[32];
        int segn = ocerz_send_xlate_descriptors(seg0, seg0_size, segsv, 32);
        for (int j = 0; j < segn && n < max_saved; j++) {
            saved[n].off = (seg0 + segsv[j].off) - gvec;
            saved[n].orig = segsv[j].orig;
            n++;
        }
    }
    return n;
}

static void ocerz_hostwq_bridge(uint64_t extra_r8, uint64_t workloop_id, const void *hev, int nev)
{
    OcerzVM *vm = g_hostwq_vm;
    if (!vm || nev <= 0 || !hev)
        return;
    if (nev > OCERZ_WQ_KEVENT_LIST_LEN)
        nev = OCERZ_WQ_KEVENT_LIST_LEN;
    uint64_t cookie = ocerz_ld(OCERZ_PTHREAD_COOKIE, 8);
    /* One guest worker region per HOST workqueue thread, reused across the many
     * callbacks the kernel routes through this recycled thread -- allocating a
     * fresh 2MB region per callback would bump-exhaust the arena within a few
     * thousand wakeups and then silently fail to bring up workers. */
    uint64_t region = g_hostwq_tl_region;
    if (region == 0) {
        region = ocerz_map_anywhere(0x200000, PROT_READ | PROT_WRITE);
        if (region == 0)
            return;
        g_hostwq_tl_region = region;
    }
    uint64_t pth = region + 0x1f0000;
    uint64_t evbuf = pth + 0x8000;
    ocerz_st(evbuf - 8, 8, workloop_id);   /* &workloop_id = keventlist-8; read by the guest
                                            * _dispatch_workloop_worker_thread (0 for kevent/async) */
    /* Copy each delivered kevent. The REAL kevent_qos_s is 0x48; the delivery stride
     * OCERZ_KEVENT_QOS_S (0x40) drops the last 8 bytes = ext[3] = EV_EXTIDX_WL_VALUE,
     * the EVFILT_WORKLOOP state-advertisement value the guest worker compares against
     * the workloop's dq_state to decide "drained vs stale". Dropping it makes that
     * compare read a STALE ext[3] -> the kernel keeps echoing EV_ERROR data=ESTALE(0x46)
     * and the workloop re-arms forever (measured: one wlid re-dispatched 2M+ times,
     * ~7s of startup burned on this spin). A single workloop wakeup is delivered nev==1,
     * so copy the FULL 0x48 for the single-event case -- this restores ext[3] WITHOUT
     * changing the 0x40 stride that the multi-event evbuf layout and the re-arm append
     * co-depend on (flipping the stride globally is the known 0/8-launch regression). For
     * nev==1 there is no next entry, so the extra 8 bytes land in unused evbuf space. */
    if (nev > 1 && OCERZ_ENV_ON("OCERZ_NEVLOG")) {
        fprintf(stderr, "ocerz: HOSTWQ-NEV nev=%d filters=", nev);
        for (int i = 0; i < nev; i++)
            fprintf(stderr, "%s%d", i ? "," : "",
                    (int)(int16_t)ocerz_ld(evbuf + (uint64_t)i * OCERZ_KEVENT_QOS_S + 8, 2));
        fprintf(stderr, "\n");
    }
    /* Whole entries, both sides at the true stride. ext[3] in particular must survive: for
     * EVFILT_WORKLOOP it is EV_EXTIDX_WL_VALUE, the state the guest worker compares against
     * the workloop's dq_state to decide "drained vs stale" -- truncating it made the kernel
     * echo EV_ERROR data=ESTALE forever and the workloop re-arm spin (one wlid re-dispatched
     * 2M+ times, ~7s of startup). That used to need a nev==1 special case to paper over the
     * short stride; at 0x48 it is simply the normal copy. */
    for (int i = 0; i < nev; i++)
        memcpy(ocerz_g2h(evbuf + (uint64_t)i * OCERZ_KEVENT_QOS_S),
               (const char *)hev + (size_t)i * OCERZ_KEVENT_QOS_S,
               (size_t)OCERZ_KEVENT_QOS_S);
    if (getenv("OCERZ_WSIG"))
        for (int i = 0; i < nev; i++) {
            const unsigned char *e = (const unsigned char *)hev + (size_t)i * OCERZ_KEVENT_QOS_S;
            uint64_t ident, udata, data, ext0, ext1; int16_t filt; uint16_t fl;
            memcpy(&ident, e + 0x00, 8); memcpy(&filt, e + 0x08, 2);
            memcpy(&fl, e + 0x0a, 2); memcpy(&udata, e + 0x10, 8);
            memcpy(&data, e + 0x20, 8); memcpy(&ext0, e + 0x28, 8); memcpy(&ext1, e + 0x30, 8);
            fprintf(stderr,
                    "ocerz: HOSTWQ-EV[%d] filt=%d flags=%#x ident=%#llx udata=%#llx data=%#llx ext0=%#llx ext1=%#llx\n",
                    i, filt, fl, (unsigned long long)ident, (unsigned long long)udata,
                    (unsigned long long)data, (unsigned long long)ext0, (unsigned long long)ext1);
        }
    /* MACH-MESSAGE BRIDGE (Bug A, UPDATE #23, default-on -- set OCERZ_NO_MACHBRIDGE to A/B):
     * an EVFILT_MACHPORT event carries ext0 = a host-allocated Mach message buffer the guest
     * cannot read; copy it (and any OOL descriptor data) into guest memory and rewrite ext0,
     * exactly as the macOS kernel would have delivered it in-process. Without this, the worker
     * faults on the unmapped buffer and (being a no-Wine-TEB thread) loops in segv_handler ->
     * the gs:0x320 fatal crash. */
    if (!getenv("OCERZ_NO_MACHBRIDGE"))
        for (int i = 0; i < nev; i++) {
            uint64_t dst = evbuf + (uint64_t)i * OCERZ_KEVENT_QOS_S;
            if ((int16_t)ocerz_ld(dst + 0x08, 2) != -8) continue; /* EVFILT_MACHPORT */
            uint64_t hbuf = ocerz_ld(dst + 0x28, 8);              /* ext0 = host kernel buf */
            uint64_t sz = ocerz_ld(dst + 0x30, 8);               /* ext1 = buffer size */
            uint64_t gbuf = ocerz_bridge_mach_msg(hbuf, sz);
            if (gbuf)
                ocerz_st(dst + 0x28, 8, gbuf);                    /* ext0 -> guest copy */
            else if (hbuf)
                ocerz_st(dst + 0x28, 8, 0);                       /* bridge failed: never leave a host ptr */
            /* ext2/ext3 carry auxiliary data belonging to a DIRECT-RECEIVED message, so they
             * are host allocations for the same reason ext0 is -- but they only mean that on
             * an event that actually delivered a message. Relocating them on every
             * EVFILT_MACHPORT event regardless is a hard regression: 0/6 launches, every run
             * dying at guest_rip=0x7ff802e312a6 on guest_addr=0x8ff000008ff (one 32-bit port
             * name repeated twice, i.e. downstream code reading a field pair this bridge had
             * rewritten out from under it).
             *
             * Narrowing it to events that really did deliver a message does not help either
             * (0/5 launches), because the premise is wrong: logging the fields shows ext2 =
             * 0x2ff000002ff with ext3 = 0, i.e. a 32-bit mach port name stored TWICE in one
             * word -- the same shape as the 0x8ff000008ff the guest then faults on. These are
             * packed 32-bit values the guest consumes directly, not an address and a length,
             * so there is nothing here to relocate and zeroing them destroys live data. */
            if (gbuf && getenv("OCERZ_WSIG"))
                fprintf(stderr, "ocerz: MACHBRIDGE ev[%d] hbuf=%#llx sz=%#llx -> gbuf=%#llx%s\n",
                        i, (unsigned long long)hbuf, (unsigned long long)sz,
                        (unsigned long long)gbuf,
                        (*(volatile uint32_t *)(uintptr_t)hbuf & 0x80000000u) ? " COMPLEX" : "");
        }
    mach_port_t kp = mach_thread_self();
    /* Has libpthread already registered THIS pth? self->0xd8 (thread_id, from
     * __thread_selfid()) is written only by _pthread_wqthread_setup, so reading it back
     * from guest memory is a self-validating predicate: it is non-zero iff setup really
     * ran to completion on this reused struct. Deriving REUSE from the guest this way --
     * rather than from a host-side "have I entered before" flag -- stays correct even if
     * an earlier entry faulted before setup linked the node. */
    /* Restore the empty per-drain TSD sentinel a completed native drain leaves, IF a
     * prior worker on this recycled host thread died wrc==125 mid-drain (flag below). It
     * left gs:0xe8 (_dispatch_deferred_items) dangling into its now-dead guest stack and
     * gs:0xd8 (adopted wlh) stale; libdispatch's entry guard `mov rdi,gs:0xe8; and rdi,~2;
     * jne` then dereferences that dangling ddi, writes through it (0x7ff802cda45d..cd791c
     * append a 72-byte kevent) -> abort -> UD2 -> a fresh dangling ddi -> self-sustaining
     * cascade. (The abort was previously described here as smashing the next worker's stack
     * canary via __stack_chk_fail. That was wrong, and it sent one investigation looking for
     * a buffer overrun that does not exist: tracing the aborting call shows libmalloc's
     * reporter invoked with the abort flag and the format "*** error for object %p: pointer
     * being freed was not allocated", i.e. an invalid free of the stale ddi, not an overrun.
     * The reset below is now unconditional for that reason.) gs_base=pth+0xe0
     * so gs:0xe8=pth+0x1c8, gs:0xd8=pth+0x1b8. Gated on the fatal flag ONLY: a clean exit
     * already self-clears both (native invariant), so an every-entry reset would be a no-op
     * there and would also MASK a real clean-path leak if one ever arose. */
    /* ...and unconditionally, not only after a fatal. The gated-on-fatal version rested on
     * "a clean exit already self-clears both (native invariant)", but that invariant does not
     * hold for a synthesized worker, and the cost of it not holding is concrete: at teardown
     * libdispatch walks a list freeing every untagged node (0x7ff802cd8306: test dil,1 / je
     * skip / mov r14,[rdi+8] / call free), and a stale gs:0xe8 left by the previous drain
     * still points at THIS region's stack -- ocerz reuses one region per host thread, so the
     * address is live-looking rather than obviously dead. free() then gets rsp_base-0xc0 and
     * libmalloc aborts the worker with "*** error for object %p: pointer being freed was not
     * allocated" (traced: the reporter's first vararg is exactly the ddi from WQ-FATAL).
     * Clearing per-drain state at entry is what a fresh kernel worker thread would present. */
    if (g_hostwq_tl_fatal || !getenv("OCERZ_NO_DDIRESET")) {
        ocerz_st(pth + 0x1c8, 8, 0);   /* gs:0xe8 _dispatch_deferred_items */
        ocerz_st(pth + 0x1b8, 8, 0);   /* gs:0xd8 adopted wlh */
        g_hostwq_tl_fatal = 0;
    }
    int registered = ocerz_ld(pth + 0xd8, 8) != 0;
    if (!registered) {
        ocerz_st(pth, 8, pth ^ cookie);
        ocerz_st(pth + 0xe0, 8, pth);
    }
    ocerz_st(pth + 0xf8, 4, (uint64_t)(uint32_t)kp);
    /* Populate the guest pthread stack bounds. libpthread's backtrace walker
     * thread_stack_pcs -- reached when libmalloc's diagnostic reporter malloc_vreport
     * captures a backtrace (e.g. free() emitting a NON-fatal "free(%p): ..." warning
     * during a workloop drain) -- bounds and terminates its frame-pointer walk with
     * pthread_get_stackaddr_np (reads pth+0xb0, the high base) and stacksize = pth+0xb0
     * minus pth+0xb8 (the low bottom). ocerz's synthetic worker left both 0, so the
     * bounds were [0,0], the walk could not terminate at the real ~10 frames and ran to
     * its 50-entry cap through this REUSED region's stale frame links -- overrunning
     * malloc_vreport's on-stack 50-slot backtrace buffer by one, whose buffer[50] Apple
     * packs to land EXACTLY on its stack canary at [rbp-0x30] (0x1c0-0x30 = 50*8). The
     * full-8-byte code-pointer write smashed the canary -> __stack_chk_fail -> abort on
     * the worker (STEP_FATAL). The worker C stack is RSP=pth-0x100 growing DOWN into
     * [region, pth-0x100), so the base is pth-0x100 and the bottom is region. */
    if (!ocerz_no_stackbounds()) {
        ocerz_st(pth + 0xb0, 8, pth - 0x100);   /* stackaddr: stack base (high) */
        ocerz_st(pth + 0xb8, 8, region);        /* stackbottom: stack low */
    }
    OcerzCPU t;
    memset(&t, 0, sizeof t);
    t.vm = vm;
    t.mxcsr = 0x1f80;
    t.fcw = 0x037f;
    t.cpu_number = ocerz_next_cpu_number();
    t.rip = OCERZ_START_WQTHREAD;
    t.gpr[OCERZ_RSP] = pth - 0x100;
    t.gpr[OCERZ_RDI] = pth;
    t.gpr[OCERZ_RSI] = kp;
    t.gpr[OCERZ_RDX] = pth;
    t.gpr[OCERZ_RCX] = evbuf;
    t.gpr[OCERZ_R8] = OCERZ_WQ_FLAG_BASE | extra_r8 | 4u
                    | (registered ? OCERZ_WQ_FLAG_REUSE : 0);
    t.gpr[OCERZ_R9] = (uint64_t)nev;
    t.gs_base = pth + 0xe0;
    if (getenv("OCERZ_GSTRACE"))
        fprintf(stderr, "ocerz: HOSTWQ worker enter region=%#llx pth=%#llx gs=%#llx rsp=%#llx nev=%d\n",
                (unsigned long long)region, (unsigned long long)pth,
                (unsigned long long)t.gs_base, (unsigned long long)t.gpr[OCERZ_RSP], nev);
    ocerz_init_gate_wait();
    int flt0 = nev > 0 ? (int)(int16_t)ocerz_ld(evbuf + 0x08, 2) : 0;
    uint64_t ident0 = nev > 0 ? ocerz_ld(evbuf + 0x00, 8) : 0;
    if (getenv("OCERZ_ULOCKLOG"))
        fprintf(stderr, "ocerz: WQ-ENTER cpu#%u kport=%#x nev=%d flt0=%d ident0=%#llx\n",
                t.cpu_number, (unsigned)kp, nev, flt0, (unsigned long long)ident0);
    int dqdump = getenv("OCERZ_DQDUMP") != NULL;
    if (dqdump)
        wl_dqdump("ENTER", workloop_id, t.cpu_number, (unsigned)kp);
    int wrc = ocerz_vm_run_cpu(vm, &t);
    if (dqdump)
        wl_dqdump(wrc == 125 ? "FATAL" : "EXIT", workloop_id, t.cpu_number, (unsigned)kp);
    /* A HOSTWQ worker that dies via STEP_FATAL (rc 125) unwinds straight back into
     * libpthread, leaving the guest's libdispatch per-thread state behind IN A REUSED
     * region. Dump that state (cost: only on an already-fatal path) -- it is what the
     * NEXT worker on this host thread inherits: gs:0xd8 = the adopted wlh, gs:0xe8 =
     * _dispatch_deferred_items (a pointer into THIS worker's now-dead guest stack),
     * gs:0x18 = _dispatch_tid_self. */
    if (wrc == 125) {
        /* Died mid-drain: flag this host thread so the NEXT worker recycled onto it
         * scrubs the dangling libdispatch per-drain TSD (gs:0xe8/gs:0xd8) before
         * libdispatch reads it. Functional; the fprintf below is only diagnostic. */
        g_hostwq_tl_fatal = 1;
        uint64_t gs = t.gs_base;
        uint64_t ddi = ocerz_ld(gs + 0xe8, 8) & ~2ull;
        uint64_t sdq = ddi ? ocerz_ld(ddi + 8, 8) : 0;
        fprintf(stderr,
                "ocerz: WQ-FATAL cpu#%u kport=%#x wlid=%#llx rsp_base=%#llx | "
                "wlh(gs:0xd8)=%#llx ddi(gs:0xe8)=%#llx tid(gs:0x18)=%#llx "
                "stashed_dq=%#llx dq_state=%#llx\n",
                t.cpu_number, (unsigned)kp, (unsigned long long)workloop_id,
                (unsigned long long)(pth - 0x100),
                (unsigned long long)ocerz_ld(gs + 0xd8, 8),
                (unsigned long long)ddi, (unsigned long long)ocerz_ld(gs + 0x18, 8),
                (unsigned long long)sdq,
                (unsigned long long)(sdq ? ocerz_ld(sdq + 0x38, 8) : 0));
    }
    if (getenv("OCERZ_ULOCKLOG"))
        fprintf(stderr, "ocerz: WQ-EXIT  cpu#%u kport=%#x rc=%d\n", t.cpu_number, (unsigned)kp, wrc);
    if (getenv("OCERZ_WQHIST") && wrc == 125) {
        uint64_t rh[32]; unsigned rn = ocerz_vm_riphist(rh, 32);
        fprintf(stderr, "ocerz: WQ-HIST cpu#%u flt0=%d ident0=%#llx rip=%#llx hist:",
                t.cpu_number, flt0, (unsigned long long)ident0,
                (unsigned long long)t.rip);
        for (unsigned i = 0; i < rn; i++)
            fprintf(stderr, " %#llx", (unsigned long long)rh[i]);
        fprintf(stderr, "\n");
    }
    mach_port_deallocate(mach_task_self(), kp);
}

/* The kernel's ASYNC (non-event) workqueue callback: it brings a worker up to
 * drain a root/global dispatch queue (a plain dispatch_async, or a dispatch
 * source whose handler targets a global queue rather than the main queue). The
 * historical stub did nothing, so under HOSTWQ those root-queue handlers never
 * ran -- a readable EVFILT_READ source whose handler lives on a global queue
 * (Cocoa brings up several) stayed un-drained and re-fired forever. Bridge it
 * exactly like the kevent path but with NO events: set up one guest worker
 * region per host workqueue thread and enter the guest's start_wqthread as an
 * async (NEWSPI, QoS-tagged, non-workloop) worker; _pthread_wqthread ->
 * _dispatch_worker_thread2 then drains the matching QoS root-queue bucket. */
static void ocerz_hostwq_queue_cb(ocerz_pthread_priority_t pri)
{
    OcerzVM *vm = g_hostwq_vm;
    if (!vm)
        return;
    uint64_t cookie = ocerz_ld(OCERZ_PTHREAD_COOKIE, 8);
    uint64_t region = g_hostwq_tl_region;
    if (region == 0) {
        region = ocerz_map_anywhere(0x200000, PROT_READ | PROT_WRITE);
        if (region == 0)
            return;
        g_hostwq_tl_region = region;
    }
    uint64_t pth = region + 0x1f0000;
    /* Mirror of the bridge scrub, and unconditional for the same reason it is there: the two
     * entry points SHARE one region and one pth per host thread (g_hostwq_tl_region), so a
     * stale gs:0xe8 left by either of them is read back by the other. Scrubbing only on the
     * bridge side leaves exactly half the hole open. */
    if (g_hostwq_tl_fatal || !getenv("OCERZ_NO_DDIRESET")) {
        ocerz_st(pth + 0x1c8, 8, 0);   /* gs:0xe8 _dispatch_deferred_items */
        ocerz_st(pth + 0x1b8, 8, 0);   /* gs:0xd8 adopted wlh */
        g_hostwq_tl_fatal = 0;
    }
    mach_port_t kp = mach_thread_self();
    ocerz_st(pth, 8, pth ^ cookie);
    ocerz_st(pth + 0xe0, 8, pth);
    ocerz_st(pth + 0xf8, 4, (uint64_t)(uint32_t)kp);
    /* Stack bounds for thread_stack_pcs -- see the detailed note in ocerz_hostwq_bridge.
     * Without these libpthread's backtrace walker overruns malloc_vreport's buffer into
     * its canary. The async worker shares the same RSP=pth-0x100 stack layout. */
    if (!ocerz_no_stackbounds()) {
        ocerz_st(pth + 0xb0, 8, pth - 0x100);   /* stackaddr: stack base (high) */
        ocerz_st(pth + 0xb8, 8, region);        /* stackbottom: stack low */
    }
    uint32_t qosbits = (uint32_t)(((uint64_t)pri >> 8) & 0x3fff);
    int qos_idx = qosbits ? (__builtin_ctz(qosbits) + 1) : 4;
    if (qos_idx < 1)
        qos_idx = 1;
    if (qos_idx > 6)
        qos_idx = 6;
    OcerzCPU t;
    memset(&t, 0, sizeof t);
    t.vm = vm;
    t.mxcsr = 0x1f80;
    t.fcw = 0x037f;
    t.cpu_number = ocerz_next_cpu_number();
    t.rip = OCERZ_START_WQTHREAD;
    t.gpr[OCERZ_RSP] = pth - 0x100;
    t.gpr[OCERZ_RDI] = pth;
    t.gpr[OCERZ_RSI] = kp;
    t.gpr[OCERZ_RDX] = pth;
    t.gpr[OCERZ_RCX] = 0;
    t.gpr[OCERZ_R8] = OCERZ_WQ_FLAG_BASE | (uint64_t)qos_idx;
    t.gpr[OCERZ_R9] = 0;
    t.gs_base = pth + 0xe0;
    ocerz_init_gate_wait();
    if (ocerz_vm_run_cpu(vm, &t) == 125)
        g_hostwq_tl_fatal = 1;   /* async worker also died mid-drain; scrub on next entry */
    mach_port_deallocate(mach_task_self(), kp);
}

static void ocerz_hostwq_kevent_cb(void **events, int *nevents)
{
    /* KEVENT servicer: route to the guest _dispatch_kevent_worker_thread (bit19) so it
     * drains the delivered kqworkq events. Publish the re-arm handoff so the worker's
     * op-0x40 workq_kernreturn can copy its outgoing changelist back into *events. */
    g_hostwq_tl_events  = events;
    g_hostwq_tl_nevents = nevents;
    /* The re-arm copy-back is bounded by the kernel buffer's capacity
     * (WQ_KEVENT_LIST_LEN), NOT by the delivered count *nevents -- see the contract
     * derivation in sys_workq_kernreturn. libdispatch may legitimately return up to
     * ddi_maxevents=14 re-arms off a single delivered event. */
    g_hostwq_tl_evcap   = OCERZ_WQ_KEVENT_LIST_LEN;
    ocerz_hostwq_bridge(OCERZ_WQ_FLAG_KEVENT, 0,
                        events ? *events : NULL, nevents ? *nevents : 0);
    /* If the worker completed via op-0x40 the copyback already set *nevents and cleared
     * the TLS; if it faulted/returned without one, re-arm nothing. */
    if (g_hostwq_tl_nevents) {
        if (nevents) *nevents = 0;
        g_hostwq_tl_events = NULL;
        g_hostwq_tl_nevents = NULL;
    }
}

static void ocerz_hostwq_workloop_cb(uint64_t *workloop_id, void **events, int *nevents)
{
    /* WORKLOOP servicer. Route to the guest _dispatch_workloop_worker_thread (bit22,
     * tested first by _pthread_wqthread) WITH the kernel-supplied workloop_id delivered at
     * evbuf-8, so the specific dispatch workloop owning Cocoa's CoreAnimation/CFRunLoop
     * source is drained -- BUT only if the guest actually registered that drain (its
     * libpthread workloop slot is non-NULL, which requires bsdthread_register to have
     * advertised WORKLOOP/bit7). If the slot is still NULL, bit22 would `call` a 0 pointer
     * (guest_rip=0); fall back to the always-registered kevent drain (bit19). This runtime
     * branch also keeps us correct across OS builds regardless of which model the guest picked. */
    uint64_t wl_slot = ocerz_ld(OCERZ_PTHREAD_WORKLOOP_SLOT, 8);
    uint64_t r8 = wl_slot ? (OCERZ_WQ_FLAG_WORKLOOP | OCERZ_WQ_FLAG_KEVENT)
                          : OCERZ_WQ_FLAG_KEVENT;
    if (getenv("OCERZ_ULOCKLOG") || getenv("OCERZ_KEVID"))
        fprintf(stderr,
                "ocerz: WLSLOT workloop[%#llx]=%#llx kevent[0x7ff8436bd660]=%#llx worker2[0x7ff8436beed0]=%#llx -> r8=%#llx wlid=%#llx\n",
                (unsigned long long)OCERZ_PTHREAD_WORKLOOP_SLOT, (unsigned long long)wl_slot,
                (unsigned long long)ocerz_ld(0x7ff8436bd660ull, 8),
                (unsigned long long)ocerz_ld(0x7ff8436beed0ull, 8),
                (unsigned long long)r8,
                (unsigned long long)(workloop_id ? *workloop_id : 0));
    g_hostwq_tl_events  = events;
    g_hostwq_tl_nevents = nevents;
    /* The re-arm copy-back is bounded by the kernel buffer's capacity
     * (WQ_KEVENT_LIST_LEN), NOT by the delivered count *nevents -- see the contract
     * derivation in sys_workq_kernreturn. libdispatch may legitimately return up to
     * ddi_maxevents=14 re-arms off a single delivered event. */
    g_hostwq_tl_evcap   = OCERZ_WQ_KEVENT_LIST_LEN;
    /* OCERZ_SPINLOG: spin detector. The workloop re-arm spin re-dispatches workloop
     * workers at ~hundreds-of-thousands/s; normal activity is ~25/s. Log every 5000th
     * dispatch (rare when idle, frequent during a spin) with the workloop id, its live
     * dq_state, and the first delivered event -- to see WHICH source keeps firing without
     * its drain clearing DIRTY. Diagnostic-only, gated. */
    if (OCERZ_ENV_ON("OCERZ_SPINLOG")) {
        static _Atomic unsigned long long g_disp;
        unsigned long long d = ++g_disp;
        if ((d % 5000ull) == 0) {
            uint64_t wlid = workloop_id ? *workloop_id : 0;
            int ne = nevents ? *nevents : 0;
            const unsigned char *ev = events ? (const unsigned char *)*events : NULL;
            int16_t f0 = 0; uint64_t id0 = 0, dt0 = 0; uint32_t ff0 = 0;
            if (ev && ne > 0) {
                memcpy(&id0, ev + 0x00, 8); memcpy(&f0, ev + 0x08, 2);
                memcpy(&ff0, ev + 0x0c, 4); memcpy(&dt0, ev + 0x20, 8);
            }
            fprintf(stderr, "ocerz: SPINLOG disp=%llu wlid=%#llx dq=%#llx nev=%d ev0{filt=%d fflags=%#x id=%#llx data=%#llx}\n",
                    d, (unsigned long long)wlid,
                    (unsigned long long)(wlid ? ocerz_ld(wlid + 0x38, 8) : 0), ne,
                    f0, ff0, (unsigned long long)id0, (unsigned long long)dt0);
        }
    }
    ocerz_hostwq_bridge(r8, workloop_id ? *workloop_id : 0,
                        events ? *events : NULL, nevents ? *nevents : 0);
    if (g_hostwq_tl_nevents) {
        if (nevents) *nevents = 0;
        g_hostwq_tl_events = NULL;
        g_hostwq_tl_nevents = NULL;
    }
}

static void ocerz_hostwq_register(OcerzVM *vm)
{
    static int done;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    if (!done) {
        ocerz_jit_require_ordered(vm);
        done = 1;
        g_hostwq_vm = vm;
        int rc = _pthread_workqueue_init_with_workloop(
            ocerz_hostwq_queue_cb, ocerz_hostwq_kevent_cb, ocerz_hostwq_workloop_cb, 0, 0);
        if (getenv("OCERZ_HOSTWQ_LOG"))
            fprintf(stderr, "ocerz: HOSTWQ registered rc=%d\n", rc);
    }
    pthread_mutex_unlock(&m);
}

/* kevent_id(uint64_t id, changelist, nchanges, eventlist, nevents, flags): the
 * dispatch-workloop channel. The guest commits a workloop wakeup here expecting
 * the kernel to bring up a servicing thread; ocerz brings one up explicitly (a
 * WORKLOOP worker). Only one workqueue worker is summoned at a time (the running
 * count gate) so a worker re-arming its own workloop does not fork the pool, and
 * non-commit calls just acknowledge. Returns 0 (non-blocking) like the kernel's
 * commit path. Gated behind OCERZ_KEVENT_WORKER while the GUI path is brought up;
 * default is the historical no-op stub so make check is unchanged. */
static int sys_kevent_id(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    if (getenv("OCERZ_HOSTWQ")) {
        ocerz_hostwq_register(vm);
        /* kevent_id is an 8-arg syscall: id, changelist, nchanges, eventlist,
         * nevents, data_out, data_available, flags. Only 6 args reach registers;
         * data_available (a[6]) and flags (a[7], carrying KEVENT_FLAG_WORKLOOP)
         * sit on the guest stack. Forward all 8, translating the four pointer
         * args (changelist, eventlist, data_out, data_available). */
        uint64_t fa[8];
        memcpy(fa, a, sizeof fa);
        fa[6] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        fa[7] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 16, 8);
        if (getenv("OCERZ_WSIG") && a[1] && (int)a[2] > 0) {
            int nc = (int)a[2]; if (nc > 8) nc = 8;
            for (int i = 0; i < nc; i++) {
                uint64_t k = a[1] + (uint64_t)i * OCERZ_KEVENT_QOS_S;
                int16_t filt = (int16_t)ocerz_ld(k + 0x08, 2);
                if (filt != -8) continue; /* EVFILT_MACHPORT only */
                fprintf(stderr,
                        "ocerz: KEVREG[%d] filt=%d flags=%#llx fflags=%#llx ident=%#llx udata=%#llx ext0=%#llx ext1=%#llx (ext0 g=%d)\n",
                        i, filt, (unsigned long long)ocerz_ld(k + 0x0a, 2),
                        (unsigned long long)ocerz_ld(k + 0x18, 4),
                        (unsigned long long)ocerz_ld(k + 0x00, 8),
                        (unsigned long long)ocerz_ld(k + 0x10, 8),
                        (unsigned long long)ocerz_ld(k + 0x28, 8),
                        (unsigned long long)ocerz_ld(k + 0x30, 8),
                        ocerz_addr_committed(ocerz_ld(k + 0x28, 8)));
            }
        }
        if (fa[1]) fa[1] = (uint64_t)(uintptr_t)ocerz_g2h(fa[1]);
        if (fa[3]) fa[3] = (uint64_t)(uintptr_t)ocerz_g2h(fa[3]);
        if (fa[5]) fa[5] = (uint64_t)(uintptr_t)ocerz_g2h(fa[5]);
        if (fa[6]) fa[6] = (uint64_t)(uintptr_t)ocerz_g2h(fa[6]);
        uint64_t ret2 = 0;
        int err = 0;
        /* KEVID trace (gated): the workloop commit/drain traffic. id (a[0]) is the
         * dispatch workloop; nchg>0 with an EVFILT_WORKLOOP(-17) change re-arms a
         * thread request (this is what drives the endless re-fire when a drain never
         * clears the workloop). Logged before the forward so a[1] is still the guest
         * changelist. flags (fa[7]) carries KEVENT_FLAG_WORKLOOP/DYNAMIC_KQ. */
        int kid_log = getenv("OCERZ_ULOCKLOG") != NULL;
        int chg0f = 0; unsigned chg0fl = 0; uint64_t chg0id = 0;
        if (kid_log && a[1] && (int)a[2] > 0) {
            chg0f  = (int)(int16_t)ocerz_ld(a[1] + 0x08, 2);
            chg0fl = (unsigned)(uint16_t)ocerz_ld(a[1] + 0x0a, 2);
            chg0id = ocerz_ld(a[1] + 0x00, 8);
        }
        uint64_t r = ocerz_host_syscall(375, fa, &ret2, &err);
        if (kid_log)
            fprintf(stderr, "ocerz: KEVID[%d] cpu#%u id=%#llx nchg=%lld nev=%lld flags=%#llx chg0{id=%#llx filt=%d fl=%#x} -> r=%lld err=%d\n",
                    (int)getpid(), cpu->cpu_number, (unsigned long long)a[0],
                    (long long)a[2], (long long)a[4], (unsigned long long)fa[7],
                    (unsigned long long)chg0id, chg0f, chg0fl, (long long)r, err);
        if (getenv("OCERZ_DQDUMP") && chg0f == -17)
            wl_dqdump("ARM", a[0], cpu->cpu_number, 0);
        /* SYNCHRONOUS receive: a worker draining its workloop calls kevent_id with
         * nevents>0, and the kernel returns EVFILT_MACHPORT events whose ext0 is a
         * host-allocated message buffer the guest cannot read -- the same gs:0x320 fault
         * as the callback path. Bridge each MACHPORT event in the returned eventlist
         * (a[3], the guest buffer the kernel just filled). */
        if (!err && (int64_t)r > 0 && a[3]) {
            int got = (int)r;
            if (got > 64) got = 64;
            for (int i = 0; i < got; i++) {
                uint64_t ev = a[3] + (uint64_t)i * OCERZ_KEVENT_QOS_S;
                if ((int16_t)ocerz_ld(ev + 0x08, 2) != -8) continue; /* EVFILT_MACHPORT */
                uint64_t hb = ocerz_ld(ev + 0x28, 8);
                uint64_t s = ocerz_ld(ev + 0x30, 8);
                uint64_t gb = ocerz_bridge_mach_msg(hb, s);
                if (gb)
                    ocerz_st(ev + 0x28, 8, gb);
                else if (hb)
                    ocerz_st(ev + 0x28, 8, 0);
            }
        }
        if (err)
            ret_err(cpu, r);
        else
            ret_ok(cpu, r);
        return OCERZ_STEP_OK;
    }
    if (getenv("OCERZ_KEVENT_WORKER")) {
        uint64_t changelist = a[1];
        int nchanges = (int)a[2];
        if (nchanges > 0 && changelist != 0 &&
            __atomic_load_n(&g_wq_running, __ATOMIC_SEQ_CST) == 0 &&
            wl_try_acquire(a[0])) {
            if (ocerz_spawn_workloop_worker(vm, cpu, a[0], changelist, nchanges) != 0)
                wl_release(a[0]);
        }
    }
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

/* kevent_qos(kq, changelist, nchanges, eventlist, nevents, data_out, data_available, flags):
 * the MODERN libdispatch kqueue/workqueue channel (same struct/arg layout as kevent_id). The
 * legacy stub returned 0 events and registered nothing, so under HOSTWQ the kernel never learned
 * about libdispatch's event SOURCES nor its servicing-worker requests -- the Cocoa app's CFRunLoop
 * sources (the ~21 level-triggered EVFILT_READ fds the manager could see via kevent(363) but never
 * got a worker to drain) were never serviced, so COCOA_APP_RUNNING was never posted and
 * macdrv_start_cocoa_app timed out ("Failed to start Cocoa app main loop"). Forward it to the real
 * kernel exactly like kevent_id (syscall 374): translate the four pointer args and bridge any
 * EVFILT_MACHPORT events the kernel returns (ext0 = host-allocated message -> guest copy). With
 * HOSTWQ registered, KEVENT_FLAG_WORKQ calls now drive ocerz's own kernel-owned workqueue
 * callbacks, the same as a native process. Stub remains the default when HOSTWQ is off so make
 * check is unchanged. */
static int sys_kevent_qos(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    /* flags (a[7]) sits on the guest stack. KEVENT_FLAG_WORKQ (0x20) marks a workqueue-manager
     * call: the kernel would invoke the registered workqueue callbacks, which under ocerz bridges
     * into a guest worker -- forwarding those re-entrantly during early dylib init ran a worker
     * with a half-built context and corrupted host control flow. Leave WORKQ-flagged calls to the
     * existing workq_kernreturn/kevent_id path (stub here); forward only the NON-WORKQ direct
     * kqueue source operations -- those are a plain kqueue syscall (like kevent/363) that just
     * register/wait on this kqueue's sources, which is exactly what libdispatch's CFRunLoop manager
     * needs to service the Cocoa app's event sources. */
    uint64_t kq_flags = ocerz_ld(cpu->gpr[OCERZ_RSP] + 16, 8);
    if (getenv("OCERZ_MGRPROBE") && (kq_flags & 0x20ull)) {
        /* WORKQ manager-request channel probe: dump the changelist thread-request so we can
         * see the EVENT_MANAGER priority (kevent qos field / a[2]=nchanges, a[1]=changelist). */
        int nch = (int)a[2]; uint64_t cl = a[1];
        fprintf(stderr, "ocerz: KQWORKQ[%d] flags=%#llx nchg=%d nev=%lld",
                (int)getpid(), (unsigned long long)kq_flags, nch, (long long)a[4]);
        for (int i = 0; i < nch && i < 4 && cl; i++) {
            uint64_t k = cl + (uint64_t)i * OCERZ_KEVENT_QOS_S;
            fprintf(stderr, " [%d]{id=%#llx filt=%d fl=%#llx qos=%#llx fflags=%#llx}", i,
                    (unsigned long long)ocerz_ld(k + 0x00, 8), (int)(int16_t)ocerz_ld(k + 0x08, 2),
                    (unsigned long long)(uint16_t)ocerz_ld(k + 0x0a, 2),
                    (unsigned long long)(uint32_t)ocerz_ld(k + 0x0c, 4),
                    (unsigned long long)(uint32_t)ocerz_ld(k + 0x18, 4));
        }
        fprintf(stderr, "\n");
    }
    /* OCERZ_FWD_WORKQ: also forward the KEVENT_FLAG_WORKQ manager channel to the host kernel.
     * The guest dispatch MANAGER polls the process kqueue via kevent_qos(WORKQ, qos=EVENT_MANAGER,
     * nev>0); stubbing it returns 0 events so the manager can never drain _dispatch_mgr_q and the
     * ENQUEUED_ON_MGR workloops livelock (re-arming NOTE_WL_THREAD_REQUEST forever) and the main
     * thread's wakeup is never posted. Forwarding lets the manager see real kqueue events. */
    static int fwd_workq = -1;
    if (fwd_workq < 0) fwd_workq = getenv("OCERZ_NO_FWD_WORKQ") ? 0 : 1;  /* default ON: run the manager */
    if (getenv("OCERZ_HOSTWQ") && (!(kq_flags & 0x20ull) || fwd_workq)) {
        ocerz_hostwq_register(vm);
        uint64_t fa[8];
        memcpy(fa, a, sizeof fa);
        fa[6] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        fa[7] = kq_flags;
        if (fa[1]) fa[1] = (uint64_t)(uintptr_t)ocerz_g2h(fa[1]);
        if (fa[3]) fa[3] = (uint64_t)(uintptr_t)ocerz_g2h(fa[3]);
        if (fa[5]) fa[5] = (uint64_t)(uintptr_t)ocerz_g2h(fa[5]);
        if (fa[6]) fa[6] = (uint64_t)(uintptr_t)ocerz_g2h(fa[6]);
        uint64_t ret2 = 0;
        int err = 0;
        uint64_t r = ocerz_host_syscall(374, fa, &ret2, &err);
        if (!err && (int64_t)r > 0 && a[3]) {
            int got = (int)r;
            if (got > 64) got = 64;
            for (int i = 0; i < got; i++) {
                uint64_t ev = a[3] + (uint64_t)i * OCERZ_KEVENT_QOS_S;
                if ((int16_t)ocerz_ld(ev + 0x08, 2) != -8) continue; /* EVFILT_MACHPORT */
                uint64_t hb = ocerz_ld(ev + 0x28, 8);
                uint64_t s = ocerz_ld(ev + 0x30, 8);
                uint64_t gb = ocerz_bridge_mach_msg(hb, s);
                if (gb)
                    ocerz_st(ev + 0x28, 8, gb);
                else if (hb)
                    ocerz_st(ev + 0x28, 8, 0);
            }
        }
        if (err)
            ret_err(cpu, r);
        else
            ret_ok(cpu, r);
        return OCERZ_STEP_OK;
    }
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int sys_bsdthread_create(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    uint64_t func = a[0], funarg = a[1], stack = a[2], pth = a[3], flags = a[4];
    (void)func;
    (void)funarg;
    ocerz_jit_require_ordered(vm);
    struct ocerz_worker *w = (struct ocerz_worker *)calloc(1, sizeof *w);
    if (!w) {
        ret_err(cpu, OCERZ_ENOMEM_V);
        return OCERZ_STEP_OK;
    }
    w->vm = vm;
    w->cpu = *cpu;
    w->cpu.ras_top = 0;
    w->cpu.terminated = 0;
    w->cpu.cpu_number = ocerz_next_cpu_number();
    w->cpu.rip = OCERZ_THREAD_START;
    w->cpu.gpr[OCERZ_RSP] = stack;
    w->cpu.gpr[OCERZ_RDI] = pth;
    w->cpu.gpr[OCERZ_RSI] = 0;
    w->cpu.gpr[OCERZ_RDX] = func;
    w->cpu.gpr[OCERZ_RCX] = funarg;
    w->cpu.gpr[OCERZ_R8] = stack;
    w->cpu.gpr[OCERZ_R9] = flags | 0x10000000ull;
    w->cpu.gs_base = pth + 0xe0;
    /* A new thread starts with NO sigaltstack, an empty blocked mask, and is not
     * on any altstack -- these are per-thread and must NOT be inherited from the
     * creating thread (w->cpu = *cpu copies them), or the new thread would build
     * signal frames on the parent's altstack and gate delivery by the parent's
     * mask, corrupting the parent and mis-delivering its own faults. */
    w->cpu.sig_altstack_sp = 0;
    w->cpu.sig_altstack_size = 0;
    w->cpu.sig_mask = 0;
    w->cpu.sig_on_stack = 0;
    w->cpu.sig_last_fault = 0;
    w->cpu.sig_repeat = 0;
    if (getenv("OCERZ_SIGTRACE"))
        fprintf(stderr, "ocerz: bsdthread_create pth=%#llx comm(pth)=%d stack=%#llx icount=%#llx\n",
                (unsigned long long)pth, ocerz_addr_committed(pth),
                (unsigned long long)stack, (unsigned long long)vm->insn_count);
    ocerz_st(pth + 0xe0, 8, pth);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16ull * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int rc = pthread_create(&th, &attr, ocerz_worker_entry, w);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(w);
        ret_err(cpu, OCERZ_ENOMEM_V);
        return OCERZ_STEP_OK;
    }
    ret_ok(cpu, pth);
    return OCERZ_STEP_OK;
}

/* The kernel's half of the pthread-join handshake. A joiner sleeps in
 * __ulock_wait(UL_COMPARE_AND_WAIT, &pth->join_word, kport) -- the u32 at
 * pthread+0x34 holding the exiting thread's kernel port -- and XNU's
 * bsdthread_terminate clears that word and wakes the waiter once the thread is
 * truly dead (uthread_joiner_wake). Without this, pthread_join either hangs or,
 * worse, the exiting thread's pre-terminate courtesy wake lets the joiner's
 * compare-wait return while the word still holds the kport, and libpthread's
 * retry logic misreads the state (mtstress: main "joined" early and exited
 * before its final output). The dead-thread sentinel is UINT32_MAX, not 0: _pthread_join's
 * already-exited fast path is `cmp dword [pth+0x34], -1` (an exiting thread
 * with NO registered joiner stores -1 itself from userspace, which is why only
 * the joiner-registered-first case broke). Store -1 and wake as the kernel
 * does; the wake op mirrors the waiter's (UL_COMPARE_AND_WAIT | ULF_NO_ERRNO). */
#define OCERZ_PTH_JOIN_WORD 0x34

static int sys_bsdthread_terminate(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    uint64_t pth = cpu->gs_base - 0xe0;
    uint64_t jw = pth + OCERZ_PTH_JOIN_WORD;
    if (pth > ocerz_arena_lo && pth < ocerz_arena_hi &&
        (uint32_t)ocerz_ld(jw, 4) == (uint32_t)a[2]) {
        ocerz_st(jw, 4, 0xffffffffull);
        uint64_t wa[8] = { 0x1000002ull, (uint64_t)(uintptr_t)ocerz_g2h(jw), 0, 0, 0, 0, 0, 0 };
        int err = 0;
        ocerz_host_syscall(516, wa, NULL, &err);
    }
    if (a[3] != 0)
        semaphore_signal((semaphore_t)a[3]);
    cpu->terminated = 1;
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int sys_sigaction(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    int sig = (int)a[0];
    uint64_t act = a[1];
    uint64_t oact = a[2];
    if (oact != 0 && sig >= 0 && sig < OCERZ_NSIG) {
        ocerz_st(oact + 0, 8, guest_sigact[sig].handler);
        ocerz_st(oact + 8, 8, guest_sigact[sig].tramp);
        ocerz_st(oact + 16, 4, (uint32_t)guest_sigact[sig].mask);
        ocerz_st(oact + 20, 4, guest_sigact[sig].flags);
    }
    if (act != 0 && sig >= 0 && sig < OCERZ_NSIG) {
        guest_sigact[sig].handler = ocerz_ld(act, 8);
        guest_sigact[sig].tramp = ocerz_ld(act + 8, 8);
        guest_sigact[sig].mask = (uint32_t)ocerz_ld(act + 16, 4);
        guest_sigact[sig].flags = (uint32_t)ocerz_ld(act + 20, 4);
    }
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int sys_pthread_kill(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    uint64_t signo = a[1];
    /* Decisive wall-2 probe: log every __pthread_kill(target_port, signo). The
     * wineserver kicks a CLIENT thread (in another process) into re-selecting for a
     * system-APC / context capture via __pthread_kill(client_thread_port, SIGUSR1);
     * ocerz currently ignores a[0] and runs the handler same-thread, dropping the
     * cross-thread kick. If this fires from the wineserver with signo=30 (SIGUSR1)
     * during the hang, that dropped signal is the deadlock. */
    if (getenv("OCERZ_PTKILL")) {
        uint64_t selfport = ocerz_host_mach_trap(27, a); /* mach_thread_self */
        fprintf(stderr, "ocerz: PTKILL[%d] target=%#llx signo=%llu self=%#llx cross=%d handler=%#llx\n",
                (int)getpid(), (unsigned long long)a[0], (unsigned long long)signo,
                (unsigned long long)selfport, (a[0] && a[0] != selfport) ? 1 : 0,
                (unsigned long long)(signo < OCERZ_NSIG ? guest_sigact[signo].handler : 0));
    }
    (void)vm; (void)signo;
    /* Faithful __pthread_kill: forward to the host kernel so it delivers signo to the
     * THREAD named by a[0] -- including cross-process (the wineserver kicking a client
     * thread via a port from mach_port_extract_right, server/mach.c:377). The target's
     * blocking forwarded host syscall (read(wait_fd)/kevent) returns EINTR and its
     * async_sig_handler records the signo; the guest handler then runs at that thread's
     * syscall-return boundary (ocerz_handle_syscall). This replaces the old same-thread
     * synthesis that DROPPED every cross-thread kick -> the wineboot service deadlock. */
    int err = 0;
    uint64_t r = ocerz_host_syscall(328, a, NULL, &err);
    if (err)
        ret_err(cpu, r);
    else
        ret_ok(cpu, r);
    return OCERZ_STEP_OK;
}

static int sys_sigprocmask(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    int how = (int)a[0];
    uint64_t set = a[1];
    uint64_t oset = a[2];
    if (oset != 0)
        ocerz_st(oset, 4, (uint32_t)cpu->sig_mask);
    if (set != 0) {
        uint32_t v = (uint32_t)ocerz_ld(set, 4);
        if (how == 1)            /* SIG_BLOCK */
            cpu->sig_mask |= v;
        else if (how == 2)       /* SIG_UNBLOCK */
            cpu->sig_mask &= ~(uint64_t)v;
        else                     /* SIG_SETMASK */
            cpu->sig_mask = v;
    }
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int sys_sigaltstack(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    uint64_t ss = a[0];
    uint64_t oss = a[1];
    if (oss != 0) {
        ocerz_st(oss + 0, 8, cpu->sig_altstack_sp);
        ocerz_st(oss + 8, 8, cpu->sig_altstack_size);
        ocerz_st(oss + 16, 4, cpu->sig_on_stack ? 0x0001u : 0u);
    }
    if (ss != 0) {
        uint32_t flags = (uint32_t)ocerz_ld(ss + 16, 4);
        if (flags & 0x0004u) {   /* SS_DISABLE */
            cpu->sig_altstack_sp = 0;
            cpu->sig_altstack_size = 0;
        } else {
            cpu->sig_altstack_sp = ocerz_ld(ss + 0, 8);
            cpu->sig_altstack_size = ocerz_ld(ss + 8, 8);
            /* Commit the WHOLE sigaltstack now. ocerz commits anonymous memory lazily, but
             * the signal-frame build writes to the TOP of the altstack from INSIDE the host
             * fault handler, where it cannot take map_lock to commit-on-demand -- an
             * uncommitted altstack page there faults again ("nested fault inside crash
             * handler" -> _exit(139), observed at gaddr just below the altstack top, comm=0).
             * Committing it eagerly here (a syscall context, map_lock-safe) guarantees the
             * frame build never faults; the macOS kernel likewise requires a usable altstack. */
            if (cpu->sig_altstack_sp && cpu->sig_altstack_size)
                ocerz_protect(cpu->sig_altstack_sp, cpu->sig_altstack_size,
                              3 /* VM_PROT_READ|VM_PROT_WRITE */);
        }
    }
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

/* Build the Darwin x86_64 signal frame for `cpu` and redirect it to the guest's
 * registered _sigtramp, exactly as the macOS kernel would when delivering `sig`.
 * Returns 1 if delivered (caller unwinds the host fault and resumes the guest at
 * the trampoline), 0 if no handler is registered (caller keeps its default
 * crash path). The frame is [siginfo][ucontext][mcontext(AVX,1032B)] laid out
 * top-down on the sigaltstack (when SA_ONSTACK) or the current guest stack; the
 * trampoline is entered with the kernel ABI rax=handler, edx=signum,
 * rcx=&siginfo, r8=&ucontext, r9=sigreturn-token, and on the handler's return it
 * calls __sigreturn (BSD 184) which restores OcerzCPU from mcontext.__ss. Offsets
 * are the probed Darwin layout (mcontext __es@0/__ss@16; __ss order
 * rax,rbx,rcx,rdx,rdi,rsi,rbp,rsp,r8..r15,rip@144,rflags@152). The fs/gs base are
 * stashed in the ucontext (uc+56/uc+64) and restored on sigreturn, matching the
 * macOS-14 kernel that saves/restores the segment base across a signal. */
#define OCERZ_MCTX_SIZE 1032u
#define OCERZ_UCTX_SIZE 768u
#define OCERZ_SIGINFO_SIZE 104u

/* OCERZ_WSIG diagnostic: which path delivers a guest signal to a NO-TEB worker
 * (sig_altstack_sp==0 && gs is not a Wine TEB). Set by the 3 callers; read+cleared
 * in ocerz_signal_deliver so the vm.c fault caller (which never sets it) reads 0. */
__thread int g_ocerz_deliver_src; /* 0=fault/other, 1=async(#21), 2=class0 SIGSYS */

int ocerz_signal_deliver(OcerzCPU *cpu, int sig, uint64_t fault_addr, int si_code,
                         uint32_t err)
{
    if (sig <= 0 || sig >= OCERZ_NSIG)
        return 0;
    GuestSigact *sa = &guest_sigact[sig];
    if (sa->handler <= 1 || sa->tramp == 0)
        return 0;
    {
        int src = g_ocerz_deliver_src;
        g_ocerz_deliver_src = 0;
        if (getenv("OCERZ_WSIG") && cpu->sig_altstack_sp == 0 &&
            !ocerz_gs_is_teb_band(cpu->gs_base))
            fprintf(stderr,
                    "ocerz: WSIG sig=%d src=%d faddr=%#llx code=%d gs=%#llx rsp=%#llx rip=%#llx handler=%#llx wteb=%#llx\n",
                    sig, src, (unsigned long long)fault_addr, si_code,
                    (unsigned long long)cpu->gs_base,
                    (unsigned long long)cpu->gpr[OCERZ_RSP], (unsigned long long)cpu->rip,
                    (unsigned long long)sa->handler, (unsigned long long)cpu->wine_teb_base);
    }
    /* These deliveries are SYNCHRONOUS CPU faults (the host SIGSEGV/SIGBUS
     * handler is the only caller). On Darwin a synchronous fault cannot be held
     * pending by the thread's signal mask against the faulting instruction, so
     * the mask does NOT suppress delivery -- which is what lets Wine's SIGSEGV
     * handler service the nested faults it deliberately takes (guard pages,
     * commit-on-demand). A handler that keeps faulting on the SAME unfixable
     * address is caught by the consecutive-repeat guard in crash_handler, not
     * here. */

    /* Use the registered sigaltstack when SA_ONSTACK is set and either we are
     * not already on it OR the current SP has LEFT its bounds. The second clause
     * is what a pure sticky "on_stack" flag misses: Wine's handler switches rsp
     * onto the 32-bit WoW64 stack mid-handler, and a nested fault must then be
     * re-delivered onto the unix altstack -- Wine recovers its thread data via
     * *(rsp & ~0xFFFF) at the altstack's 64KB-aligned base, which only holds the
     * thread pointer on the altstack. Matches Linux on_sig_stack(sp) semantics. */
    uint64_t asp = cpu->sig_altstack_sp, asz = cpu->sig_altstack_size;
    int sp_on_alt = asp && (cpu->gpr[OCERZ_RSP] - asp < asz);
    int use_alt = (sa->flags & DARWIN_SA_ONSTACK) && asp &&
                  (!cpu->sig_on_stack || !sp_on_alt);
    if (getenv("OCERZ_SIGTRACE"))
        fprintf(stderr,
                "ocerz:   altstk sig=%d flags=%#x ONSTACK=%d altsp=%#llx altsz=%#llx on_stack=%d sp_on_alt=%d -> use_alt=%d\n",
                sig, sa->flags, (sa->flags & DARWIN_SA_ONSTACK) ? 1 : 0,
                (unsigned long long)cpu->sig_altstack_sp,
                (unsigned long long)cpu->sig_altstack_size, cpu->sig_on_stack,
                sp_on_alt, use_alt);
    uint64_t top = use_alt ? (cpu->sig_altstack_sp + cpu->sig_altstack_size)
                           : cpu->gpr[OCERZ_RSP];
    uint64_t mc = (top - OCERZ_MCTX_SIZE) & ~15ull;
    uint64_t uc = (mc - OCERZ_UCTX_SIZE) & ~15ull;
    uint64_t si = (uc - OCERZ_SIGINFO_SIZE) & ~15ull;
    uint64_t newsp = si - 8;

    memset(ocerz_g2h(mc), 0, OCERZ_MCTX_SIZE);
    memset(ocerz_g2h(uc), 0, OCERZ_UCTX_SIZE);
    memset(ocerz_g2h(si), 0, OCERZ_SIGINFO_SIZE);

    int trapno = (sig == SIGSEGV || sig == SIGBUS) ? 14
               : (sig == SIGILL ? 6
               : (sig == OCERZ_SIGTRAP ? 3 : 0));
    ocerz_st(mc + 0, 4, (uint32_t)trapno);
    ocerz_st(mc + 4, 4, err);
    ocerz_st(mc + 8, 8, fault_addr);
    ocerz_st(mc + 16, 8, cpu->gpr[OCERZ_RAX]);
    ocerz_st(mc + 24, 8, cpu->gpr[OCERZ_RBX]);
    ocerz_st(mc + 32, 8, cpu->gpr[OCERZ_RCX]);
    ocerz_st(mc + 40, 8, cpu->gpr[OCERZ_RDX]);
    ocerz_st(mc + 48, 8, cpu->gpr[OCERZ_RDI]);
    ocerz_st(mc + 56, 8, cpu->gpr[OCERZ_RSI]);
    ocerz_st(mc + 64, 8, cpu->gpr[OCERZ_RBP]);
    ocerz_st(mc + 72, 8, cpu->gpr[OCERZ_RSP]);
    ocerz_st(mc + 80, 8, cpu->gpr[OCERZ_R8]);
    ocerz_st(mc + 88, 8, cpu->gpr[OCERZ_R9]);
    ocerz_st(mc + 96, 8, cpu->gpr[OCERZ_R10]);
    ocerz_st(mc + 104, 8, cpu->gpr[OCERZ_R11]);
    ocerz_st(mc + 112, 8, cpu->gpr[OCERZ_R12]);
    ocerz_st(mc + 120, 8, cpu->gpr[OCERZ_R13]);
    ocerz_st(mc + 128, 8, cpu->gpr[OCERZ_R14]);
    ocerz_st(mc + 136, 8, cpu->gpr[OCERZ_R15]);
    ocerz_st(mc + 144, 8, cpu->rip);
    ocerz_st(mc + 152, 8, cpu->rflags);
    ocerz_st(mc + 160, 8, 0x2b);
    ocerz_st(mc + 216, 4, cpu->mxcsr);
    for (int i = 0; i < 16; i++)
        ocerz_st128(mc + 352 + (uint64_t)i * 16, cpu->xmm[i]);

    uint64_t old_mask = cpu->sig_mask;
    ocerz_st(uc + 0, 4, (use_alt || cpu->sig_on_stack) ? 1u : 0u);
    ocerz_st(uc + 4, 4, (uint32_t)old_mask);
    ocerz_st(uc + 8, 8, cpu->sig_altstack_sp);
    ocerz_st(uc + 16, 8, cpu->sig_altstack_size);
    ocerz_st(uc + 24, 4, cpu->sig_on_stack ? 1u : 0u);
    ocerz_st(uc + 40, 8, OCERZ_MCTX_SIZE);
    ocerz_st(uc + 48, 8, mc);
    /* Carry the segment bases across the handler. On macOS 14+ the kernel saves
     * the fs/gs base in the signal state and restores it on sigreturn; Wine's
     * handlers RELY on this -- init_handler sets gs to the pthread base (cthread)
     * for the unix handler's own TLS, expecting the original (TEB) base to come
     * back when the interrupted code resumes. Without restoring it, a handler
     * that resumes into __wine_syscall_dispatcher (the SIGSYS path, or a syscall
     * issued from within a handler) runs `mov %gs:0x30,%r13` with gs=cthread,
     * reads cthread:0x30 = 0, and the dispatcher's later [r13+0x320] derefs near
     * NULL. Stored past the 56-byte Darwin ucontext where nothing else lives, so
     * it nests per-frame; sys_sigreturn restores it. */
    ocerz_st(uc + 56, 8, cpu->gs_base);
    ocerz_st(uc + 64, 8, cpu->fs_base);
    if (getenv("OCERZ_GSTRACE") && cpu->gs_base < 0x100000)
        fprintf(stderr, "ocerz: GS deliver-save sig=%d gs=%#llx (SMALL) rip=%#llx\n",
                sig, (unsigned long long)cpu->gs_base, (unsigned long long)cpu->rip);

    ocerz_st(si + 0, 4, (uint32_t)sig);
    ocerz_st(si + 8, 4, (uint32_t)si_code);
    ocerz_st(si + 24, 8, fault_addr);

    cpu->gpr[OCERZ_RDI] = sa->handler;
    cpu->gpr[OCERZ_RDX] = (uint32_t)sig;
    cpu->gpr[OCERZ_RCX] = si;
    cpu->gpr[OCERZ_R8] = uc;
    cpu->gpr[OCERZ_R9] = uc;
    cpu->gpr[OCERZ_RSP] = newsp;
    cpu->rip = sa->tramp;
    if (use_alt)
        cpu->sig_on_stack = 1;
    cpu->sig_mask = old_mask | sa->mask;
    if (!(sa->flags & DARWIN_SA_NODEFER) && sig > 0)
        cpu->sig_mask |= 1ull << (sig - 1);
    return 1;
}

static int sys_sigreturn(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    uint64_t uc = a[0];
    if (!uc) {
        ret_err(cpu, EINVAL);
        return OCERZ_STEP_OK;
    }
    uint64_t mc = ocerz_ld(uc + 48, 8);
    if (!mc) {
        ret_err(cpu, EINVAL);
        return OCERZ_STEP_OK;
    }
    cpu->gpr[OCERZ_RAX] = ocerz_ld(mc + 16, 8);
    cpu->gpr[OCERZ_RBX] = ocerz_ld(mc + 24, 8);
    cpu->gpr[OCERZ_RCX] = ocerz_ld(mc + 32, 8);
    cpu->gpr[OCERZ_RDX] = ocerz_ld(mc + 40, 8);
    cpu->gpr[OCERZ_RDI] = ocerz_ld(mc + 48, 8);
    cpu->gpr[OCERZ_RSI] = ocerz_ld(mc + 56, 8);
    cpu->gpr[OCERZ_RBP] = ocerz_ld(mc + 64, 8);
    cpu->gpr[OCERZ_RSP] = ocerz_ld(mc + 72, 8);
    cpu->gpr[OCERZ_R8] = ocerz_ld(mc + 80, 8);
    cpu->gpr[OCERZ_R9] = ocerz_ld(mc + 88, 8);
    cpu->gpr[OCERZ_R10] = ocerz_ld(mc + 96, 8);
    cpu->gpr[OCERZ_R11] = ocerz_ld(mc + 104, 8);
    cpu->gpr[OCERZ_R12] = ocerz_ld(mc + 112, 8);
    cpu->gpr[OCERZ_R13] = ocerz_ld(mc + 120, 8);
    cpu->gpr[OCERZ_R14] = ocerz_ld(mc + 128, 8);
    cpu->gpr[OCERZ_R15] = ocerz_ld(mc + 136, 8);
    cpu->rip = ocerz_ld(mc + 144, 8);
    cpu->rflags = ocerz_ld(mc + 152, 8) | 0x2;
    for (int i = 0; i < 16; i++)
        cpu->xmm[i] = ocerz_ld128(mc + 352 + (uint64_t)i * 16);
    cpu->mxcsr = (uint32_t)ocerz_ld(mc + 216, 4);
    cpu->sig_mask = (uint32_t)ocerz_ld(uc + 4, 4);
    /* Restore the fs/gs base saved at delivery (see ocerz_signal_deliver): the
     * macOS-14 kernel restores the segment base on sigreturn, undoing the handler's
     * init_handler swap to the pthread base so the interrupted code resumes with
     * its original TEB base. */
    cpu->gs_base = ocerz_ld(uc + 56, 8);
    cpu->fs_base = ocerz_ld(uc + 64, 8);
    if (ocerz_gs_is_teb_band(cpu->gs_base))
        cpu->wine_teb_base = cpu->gs_base;
    if (getenv("OCERZ_GSTRACE") && cpu->gs_base < 0x100000)
        fprintf(stderr, "ocerz: GS sigreturn-restore gs=%#llx (SMALL) uc=%#llx rip=%#llx\n",
                (unsigned long long)cpu->gs_base, (unsigned long long)uc,
                (unsigned long long)cpu->rip);
    if (!(uint32_t)ocerz_ld(uc + 0, 4))
        cpu->sig_on_stack = 0;
    return OCERZ_STEP_OK;
}

static int sys_sigpending(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    uint64_t set = a[0];
    if (set != 0)
        memset(ocerz_g2h(set), 0, 4);
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

static int forward_with_scratch(OcerzCPU *cpu, int num, uint64_t a[8], int dual_ret)
{
    int err = 0;
    uint64_t ret2 = 0;
    uint64_t r = ocerz_host_syscall(num, a, &ret2, &err);
    if (err) {
        ret_err(cpu, r);
    } else if (dual_ret) {
        ret_ok2(cpu, r, ret2);
    } else {
        ret_ok(cpu, r);
    }
    return err ? (int)r : 0;
}

static int sys_iov(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8], int num)
{
    (void)vm;
    uint64_t giov = a[1];
    uint64_t cnt = a[2];
    if (cnt > OCERZ_IOV_MAX)
        cnt = OCERZ_IOV_MAX;
    struct ocerz_iovec scratch[OCERZ_IOV_MAX];
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t base = ocerz_ld(giov + i * 16, 8);
        uint64_t len = ocerz_ld(giov + i * 16 + 8, 8);
        scratch[i].iov_base = base ? (uint64_t)(uintptr_t)ocerz_g2h(base) : 0;
        scratch[i].iov_len = len;
    }
    uint64_t fa[8] = { a[0], (uint64_t)(uintptr_t)scratch, a[2], 0, 0, 0, 0, 0 };
    forward_with_scratch(cpu, num, fa, 0);
    return OCERZ_STEP_OK;
}

static int sys_readv(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    return sys_iov(vm, cpu, a, 120);
}

static int sys_writev(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    return sys_iov(vm, cpu, a, 121);
}

/* recvmsg(27)/sendmsg(28) and their _nocancel forms (401/402). A guest msghdr
 * has interior pointers (msg_name, the msg_iov array and every iov_base, and
 * msg_control) that can live in low guest memory under the wine low-shadow
 * window, so each must go through ocerz_g2h — the old shallow ptr_mask
 * translation (which only moved the msghdr pointer itself) corrupts them. The
 * x86_64 and arm64 macOS struct msghdr/iovec/cmsghdr layouts are byte-identical
 * (msghdr 48: name@0 namelen@8 iov@16 iovlen@24 control@32 controllen@40
 * flags@44; iovec 16: base@0 len@8), so a host struct msghdr whose pointers are
 * the g2h images of the guest's lets the host syscall read/write directly into
 * guest memory. SCM_RIGHTS file descriptors in the control buffer need no
 * translation: guest fds ARE host fds in ocerz. recvmsg writes msg_namelen /
 * msg_controllen / msg_flags back, so those are copied out to the guest msghdr
 * afterward. */
static int sys_msg(OcerzCPU *cpu, uint64_t a[8], int num, int is_send)
{
    uint64_t gmsg = a[1];
    if (!gmsg) {
        ret_err(cpu, EFAULT);
        return OCERZ_STEP_OK;
    }
    int iovlen = (int)(int32_t)ocerz_ld(gmsg + 24, 4);
    if (iovlen < 0)
        iovlen = 0;
    if (iovlen > OCERZ_IOV_MAX)
        iovlen = OCERZ_IOV_MAX;
    uint64_t giov = ocerz_ld(gmsg + 16, 8);
    struct ocerz_iovec iovs[OCERZ_IOV_MAX];
    for (int i = 0; i < iovlen; i++) {
        uint64_t base = ocerz_ld(giov + (uint64_t)i * 16, 8);
        uint64_t len = ocerz_ld(giov + (uint64_t)i * 16 + 8, 8);
        iovs[i].iov_base = base ? (uint64_t)(uintptr_t)ocerz_g2h(base) : 0;
        iovs[i].iov_len = len;
    }
    uint64_t gname = ocerz_ld(gmsg + 0, 8);
    uint64_t gctrl = ocerz_ld(gmsg + 32, 8);
    struct msghdr h;
    memset(&h, 0, sizeof h);
    h.msg_name = gname ? ocerz_g2h(gname) : NULL;
    h.msg_namelen = (socklen_t)ocerz_ld(gmsg + 8, 4);
    h.msg_iov = (struct iovec *)iovs;
    h.msg_iovlen = iovlen;
    h.msg_control = gctrl ? ocerz_g2h(gctrl) : NULL;
    h.msg_controllen = (socklen_t)ocerz_ld(gmsg + 40, 4);
    h.msg_flags = (int)(int32_t)ocerz_ld(gmsg + 44, 4);

    uint64_t fa[8] = { a[0], (uint64_t)(uintptr_t)&h, a[2], 0, 0, 0, 0, 0 };
    int err = 0;
    uint64_t ret2 = 0;
    uint64_t r = ocerz_host_syscall(num, fa, &ret2, &err);
    if (err) {
        ret_err(cpu, r);
        return OCERZ_STEP_OK;
    }
    if (!is_send) {
        ocerz_st(gmsg + 8, 4, h.msg_namelen);
        ocerz_st(gmsg + 40, 4, h.msg_controllen);
        ocerz_st(gmsg + 44, 4, (uint32_t)h.msg_flags);
    }
    ret_ok(cpu, r);
    return OCERZ_STEP_OK;
}

static int sys_recvmsg(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    return sys_msg(cpu, a, 27, 0);
}

static int sys_sendmsg(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    return sys_msg(cpu, a, 28, 1);
}

static int sys_recvmsg_nocancel(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    return sys_msg(cpu, a, 401, 0);
}

static int sys_sendmsg_nocancel(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    return sys_msg(cpu, a, 402, 1);
}

static int sys_ioctl(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    uint64_t arg = a[2];
    uint64_t fa[8] = { a[0], a[1], arg ? (uint64_t)(uintptr_t)ocerz_g2h(arg) : 0, 0, 0, 0, 0, 0 };
    forward_with_scratch(cpu, 54, fa, 0);
    return OCERZ_STEP_OK;
}

static int sys_fcntl(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    int cmd = (int)a[1];
    uint64_t fa[8] = { a[0], a[1], a[2], 0, 0, 0, 0, 0 };
    if ((cmd == OCERZ_F_GETPATH || cmd == OCERZ_F_PREALLOCATE) && a[2] != 0)
        fa[2] = (uint64_t)(uintptr_t)ocerz_g2h(a[2]);
    forward_with_scratch(cpu, 92, fa, 0);
    return OCERZ_STEP_OK;
}

static const ocerz_bsd_entry bsd_table[OCERZ_BSD_MAX] = {
    [1]   = { "exit",        1, 0x00, 0, sys_exit },
    [2]   = { "fork",        0, 0x00, 0, sys_fork },
    [3]   = { "read",        3, 0x02, 0, NULL },
    [4]   = { "write",       3, 0x02, 0, NULL },
    [5]   = { "open",        3, 0x01, 0, NULL },
    [6]   = { "close",       1, 0x00, 0, NULL },
    [7]   = { "wait4",       4, 0x0a, 0, NULL },
    [10]  = { "unlink",      1, 0x01, 0, NULL },
    [12]  = { "chdir",       1, 0x01, 0, NULL },
    [13]  = { "fchdir",      1, 0x00, 0, NULL },
    [15]  = { "chmod",       3, 0x01, 0, NULL },
    [16]  = { "chown",       3, 0x01, 0, NULL },
    [20]  = { "getpid",      0, 0x00, 0, NULL },
    [23]  = { "setuid",      1, 0x00, 0, NULL },
    [24]  = { "getuid",      0, 0x00, 0, NULL },
    [25]  = { "geteuid",     0, 0x00, 0, NULL },
    [26]  = { "ptrace",      4, 0x00, 0, sys_unsupported },
    [27]  = { "recvmsg",     3, 0x00, 0, sys_recvmsg },
    [28]  = { "sendmsg",     3, 0x00, 0, sys_sendmsg },
    [29]  = { "recvfrom",    6, 0x32, 0, NULL },
    [30]  = { "accept",      3, 0x06, 0, NULL },
    [31]  = { "getpeername", 3, 0x06, 0, NULL },
    [32]  = { "getsockname", 3, 0x06, 0, NULL },
    [33]  = { "access",      2, 0x01, 0, NULL },
    [34]  = { "chflags",     2, 0x01, 0, NULL },
    [35]  = { "fchflags",    2, 0x00, 0, NULL },
    [36]  = { "sync",        0, 0x00, 0, NULL },
    [37]  = { "kill",        2, 0x00, 0, NULL },
    [39]  = { "getppid",     0, 0x00, 0, NULL },
    [41]  = { "dup",         1, 0x00, 0, NULL },
    [42]  = { "pipe",        0, 0x00, 1, NULL },
    [43]  = { "getegid",     0, 0x00, 0, NULL },
    [46]  = { "sigaction",   3, 0x06, 0, sys_sigaction },
    [47]  = { "getgid",      0, 0x00, 0, NULL },
    [48]  = { "sigprocmask", 3, 0x06, 0, sys_sigprocmask },
    [49]  = { "getlogin",    2, 0x01, 0, NULL },
    [50]  = { "setlogin",    1, 0x01, 0, NULL },
    [52]  = { "sigpending",  1, 0x01, 0, sys_sigpending },
    [53]  = { "sigaltstack", 2, 0x03, 0, sys_sigaltstack },
    [184] = { "sigreturn",   2, 0x00, 0, sys_sigreturn },
    [54]  = { "ioctl",       3, 0x04, 0, sys_ioctl },
    [57]  = { "symlink",     2, 0x03, 0, NULL },
    [58]  = { "readlink",    3, 0x03, 0, NULL },
    [59]  = { "execve",      3, 0x00, 0, sys_execve },
    [60]  = { "umask",       1, 0x00, 0, NULL },
    [65]  = { "msync",       3, 0x01, 0, NULL },
    [73]  = { "munmap",      2, 0x00, 0, sys_munmap },
    [74]  = { "mprotect",    3, 0x00, 0, sys_mprotect },
    [75]  = { "madvise",     3, 0x00, 0, sys_madvise },
    [79]  = { "getgroups",   2, 0x02, 0, NULL },
    [80]  = { "setgroups",   2, 0x02, 0, NULL },
    [81]  = { "getpgrp",     0, 0x00, 0, NULL },
    [82]  = { "setpgid",     2, 0x00, 0, NULL },
    [89]  = { "getdtablesize", 0, 0x00, 0, NULL },
    [90]  = { "dup2",        2, 0x00, 0, NULL },
    [92]  = { "fcntl",       3, 0x00, 0, sys_fcntl },
    [93]  = { "select",      5, 0x1e, 0, NULL },
    [95]  = { "fsync",       1, 0x00, 0, NULL },
    [96]  = { "setpriority", 3, 0x00, 0, NULL },
    [97]  = { "socket",      3, 0x00, 0, NULL },
    [98]  = { "connect",     3, 0x02, 0, NULL },
    [100] = { "getpriority", 2, 0x00, 0, NULL },
    [104] = { "bind",        3, 0x02, 0, NULL },
    [105] = { "setsockopt",  5, 0x08, 0, NULL },
    [106] = { "listen",      2, 0x00, 0, NULL },
    [116] = { "gettimeofday",2, 0x03, 0, NULL },
    [117] = { "getrusage",   2, 0x02, 0, NULL },
    [118] = { "getsockopt",  5, 0x18, 0, NULL },
    [120] = { "readv",       3, 0x00, 0, sys_readv },
    [121] = { "writev",      3, 0x00, 0, sys_writev },
    [123] = { "fchown",      3, 0x00, 0, NULL },
    [124] = { "fchmod",      2, 0x00, 0, NULL },
    [128] = { "rename",      2, 0x03, 0, NULL },
    [139] = { "futimes",     2, 0x02, 0, NULL },
    [133] = { "sendto",      6, 0x12, 0, NULL },
    [134] = { "shutdown",    2, 0x00, 0, NULL },
    [135] = { "socketpair",  4, 0x08, 0, NULL },
    [136] = { "mkdir",       2, 0x01, 0, NULL },
    [137] = { "rmdir",       1, 0x01, 0, NULL },
    [138] = { "utimes",      2, 0x03, 0, NULL },
    [140] = { "adjtime",     2, 0x03, 0, NULL },
    [142] = { "gethostuuid", 2, 0x03, 0, NULL },
    [147] = { "setsid",      0, 0x00, 0, NULL },
    [151] = { "getpgid",     1, 0x00, 0, NULL },
    [153] = { "pread",       4, 0x02, 0, NULL },
    [154] = { "pwrite",      4, 0x02, 0, NULL },
    [169] = { "csops",       4, 0x04, 0, NULL },
    [170] = { "csops_audittoken", 5, 0x14, 0, NULL },
    [180] = { "kdebug_trace", 5, 0x00, 0, NULL },
    [191] = { "pathconf",    2, 0x01, 0, NULL },
    [192] = { "fpathconf",   2, 0x00, 0, NULL },
    [194] = { "getrlimit",   2, 0x02, 0, NULL },
    [195] = { "setrlimit",   2, 0x02, 0, NULL },
    [197] = { "mmap",        6, 0x00, 0, sys_mmap },
    [199] = { "lseek",       3, 0x00, 0, NULL },
    [200] = { "truncate",    2, 0x01, 0, NULL },
    [201] = { "ftruncate",   2, 0x00, 0, NULL },
    [202] = { "sysctl",      6, 0x1d, 0, sys_sysctl },
    [216] = { "open_dprotected_np", 5, 0x01, 0, NULL },
    [220] = { "getattrlist", 5, 0x07, 0, NULL },
    [228] = { "fgetattrlist", 5, 0x06, 0, NULL },
    [229] = { "fsetattrlist", 5, 0x06, 0, NULL },
    [230] = { "poll",        3, 0x01, 0, NULL },
    [234] = { "getxattr",    6, 0x07, 0, NULL },
    [466] = { "faccessat",   4, 0x02, 0, NULL },
    [476] = { "getattrlistat", 6, 0x0e, 0, NULL },
    [235] = { "fgetxattr",   6, 0x06, 0, NULL },
    [236] = { "setxattr",    6, 0x07, 0, NULL },
    [237] = { "fsetxattr",   6, 0x06, 0, NULL },
    [238] = { "removexattr", 3, 0x03, 0, NULL },
    [239] = { "fremovexattr",3, 0x02, 0, NULL },
    [240] = { "listxattr",   4, 0x03, 0, NULL },
    [241] = { "flistxattr",  4, 0x02, 0, NULL },
    [244] = { "posix_spawn", 5, 0x00, 0, sys_posix_spawn },
    [266] = { "shm_open",    3, 0x01, 0, NULL },
    [267] = { "shm_unlink",  1, 0x01, 0, NULL },
    [268] = { "sem_open",    4, 0x01, 0, NULL },
    [269] = { "sem_close",   1, 0x00, 0, NULL },
    [270] = { "sem_unlink",  1, 0x01, 0, NULL },
    [271] = { "sem_wait",    1, 0x00, 0, NULL },
    [272] = { "sem_trywait", 1, 0x00, 0, NULL },
    [273] = { "sem_post",    1, 0x00, 0, NULL },
    [274] = { "sysctlbyname",6, 0x1d, 0, sys_sysctlbyname },
    [381] = { "__mac_syscall", 3, 0x05, 0, NULL },
    [483] = { "csrctl", 3, 0x02, 0, NULL },
    [441] = { "guarded_open_np", 5, 0x03, 0, NULL },
    [442] = { "guarded_close_np", 2, 0x02, 0, NULL },
    [443] = { "guarded_kqueue_np", 2, 0x01, 0, NULL },
    [444] = { "change_fdguard_np", 6, 0x2a, 0, NULL },
    [501] = { "necp_open", 1, 0x00, 0, NULL },
    [502] = { "necp_client_action", 6, 0x14, 0, NULL },
    /* persona(op, flags, kinfo*, id*, idlen*): identity query (op 4 = PERSONA_OP_INFO).
     * Forwarded to the host kernel -- the guest IS the host process, so the host's persona
     * state is the faithful answer. Args 2/3/4 are pointers (mask 0x1c). Was a fatal gap. */
    [494] = { "persona", 5, 0x1c, 0, NULL },
    [524] = { "setattrlistat", 6, 0x0e, 0, NULL },
    [521] = { "abort_with_payload", 6, 0x04, 0, sys_abort_payload },
    [283] = { "fchmod_extended", 5, 0x10, 0, NULL },
    [286] = { "gettid",      2, 0x03, 0, NULL },
    [294] = { "shared_region_check_np", 1, 0x01, 0, sys_shared_region_check_np },
    [322] = { "iopolicysys", 2, 0x02, 0, NULL },
    [327] = { "issetugid",   0, 0x00, 0, NULL },
    [336] = { "proc_info",   6, 0x10, 0, NULL },
    [338] = { "stat64",      2, 0x03, 0, NULL },
    [339] = { "fstat64",     2, 0x02, 0, NULL },
    [340] = { "lstat64",     2, 0x03, 0, NULL },
    [341] = { "stat64_extended",  4, 0x0f, 0, NULL },
    [342] = { "lstat64_extended", 4, 0x0f, 0, NULL },
    [343] = { "fstat64_extended", 4, 0x0e, 0, NULL },
    [344] = { "getdirentries64", 4, 0x0a, 0, NULL },
    [345] = { "statfs64",    2, 0x03, 0, NULL },
    [346] = { "fstatfs64",   2, 0x02, 0, NULL },
    [347] = { "getfsstat64", 3, 0x01, 0, NULL },
    [350] = { "audit",        2, 0x01, 0, NULL },
    [351] = { "auditon",      3, 0x02, 0, NULL },
    [353] = { "getauid",      1, 0x01, 0, NULL },
    [357] = { "getaudit_addr", 2, 0x01, 0, NULL },
    [358] = { "setaudit_addr", 2, 0x01, 0, NULL },
    [359] = { "auditctl",     1, 0x01, 0, NULL },
    [360] = { "bsdthread_create", 5, 0x00, 0, sys_bsdthread_create },
    [361] = { "bsdthread_terminate", 4, 0x00, 0, sys_bsdthread_terminate },
    [362] = { "kqueue",      0, 0x00, 0, NULL },
    [363] = { "kevent",      6, 0x2a, 0, NULL },
    [366] = { "bsdthread_register", 7, 0x00, 0, sys_bsdthread_register },
    [367] = { "workq_open",  0, 0x00, 0, sys_workq_stub },
    [368] = { "workq_kernreturn", 4, 0x00, 0, sys_workq_kernreturn },
    [372] = { "thread_selfid", 0, 0x00, 0, NULL },
    [328] = { "__pthread_kill", 2, 0x00, 0, sys_pthread_kill },
    [329] = { "__pthread_sigmask", 3, 0x06, 0, sys_sigprocmask },
    [331] = { "__disable_threadsignal", 1, 0x00, 0, sys_workq_stub },
    /* pthread cancellation pair. __pthread_canceled(action) is the cancellation-point epilogue
     * libpthread runs after a cancelable syscall (e.g. the read() a libdispatch worker just did);
     * it must be FORWARDED, not stubbed: the kernel returns 0 only when the thread is actually
     * canceled (libpthread then pthread_exit()s) and EINVAL otherwise (continue). ocerz's worker
     * host threads are never pthread_cancel'd, so the kernel returns EINVAL -> the guest continues.
     * Stubbing to 0 would falsely mean "canceled" and silently kill the worker -> macdrv's Cocoa
     * app loses its servicing worker -> "Failed to start Cocoa app main loop". __pthread_canceled
     * only READS state, so with no thread ever marked it cleanly returns "not canceled" and the
     * worker continues. __pthread_markcancel(port) must NOT be forwarded: it would cancel the real
     * HOST thread backing the guest thread, which then exits mid-ocerz_vm_run_cpu and corrupts the
     * host stack (observed: SIGBUS, host PC driven into guest memory at ~0x2d3c4 during dylib
     * init). Stub it (ignore cancellation -- ocerz never pthread_cancel's guest threads, so leaving
     * them unmarked is the correct observable state). Proper guest-level cancellation is a TODO. */
    [332] = { "__pthread_markcancel", 1, 0x00, 0, sys_workq_stub },
    [333] = { "__pthread_canceled", 1, 0x00, 0, NULL },
    [334] = { "__semwait_signal", 6, 0x00, 0, NULL },
    [374] = { "kevent_qos",  8, 0x00, 0, sys_kevent_qos },
    [375] = { "kevent_id",   6, 0x00, 0, sys_kevent_id },
    [406] = { "fcntl_nocancel", 3, 0x00, 0, sys_fcntl },
    [409] = { "connect_nocancel", 3, 0x02, 0, NULL },
    [478] = { "bsdthread_ctl", 4, 0x00, 0, sys_workq_stub },
    [515] = { "ulock_wait",  4, 0x02, 0, NULL },
    [516] = { "ulock_wake",  3, 0x02, 0, NULL },
    [544] = { "ulock_wait2", 5, 0x02, 0, NULL },
    [301] = { "psynch_mutexwait", 5, 0x01, 0, NULL },
    [302] = { "psynch_mutexdrop", 5, 0x01, 0, NULL },
    [303] = { "psynch_cvbroad",   7, 0x11, 0, NULL },
    [304] = { "psynch_cvsignal",  7, 0x11, 0, NULL },
    [305] = { "psynch_cvwait",    8, 0x09, 0, NULL },
    [306] = { "psynch_rw_longrdlock", 5, 0x01, 0, NULL },
    [307] = { "psynch_rw_yieldwrlock", 5, 0x01, 0, NULL },
    [309] = { "psynch_rw_upgrade", 5, 0x01, 0, NULL },
    [310] = { "psynch_rw_rdlock",  5, 0x01, 0, NULL },
    [311] = { "psynch_rw_wrlock",  5, 0x01, 0, NULL },
    [312] = { "psynch_rw_unlock",  5, 0x01, 0, NULL },
    [313] = { "psynch_rw_unlock2", 5, 0x01, 0, NULL },
    [529] = { "psynch_cvclrprepost", 7, 0x01, 0, NULL },
    [396] = { "read_nocancel",  3, 0x02, 0, NULL },
    [397] = { "write_nocancel", 3, 0x02, 0, NULL },
    [398] = { "open_nocancel",  3, 0x01, 0, NULL },
    [399] = { "close_nocancel", 1, 0x00, 0, NULL },
    [401] = { "recvmsg_nocancel", 3, 0x00, 0, sys_recvmsg_nocancel },
    [402] = { "sendmsg_nocancel", 3, 0x00, 0, sys_sendmsg_nocancel },
    [403] = { "recvfrom_nocancel", 6, 0x32, 0, NULL },
    [404] = { "accept_nocancel", 3, 0x02, 0, NULL },
    [407] = { "select_nocancel", 5, 0x1e, 0, NULL },
    [413] = { "sendto_nocancel", 6, 0x12, 0, NULL },
    [414] = { "pread_nocancel", 4, 0x02, 0, NULL },
    [415] = { "pwrite_nocancel",4, 0x02, 0, NULL },
    [417] = { "poll_nocancel", 3, 0x02, 0, NULL },
    [420] = { "sem_wait_nocancel", 1, 0x00, 0, sys_workq_stub },
    [423] = { "__semwait_signal_nocancel", 6, 0x00, 0, NULL },
    [427] = { "fsgetpath",   4, 0x05, 0, NULL },
    [428] = { "audit_session_self", 0, 0x00, 0, NULL },
    [461] = { "getattrlistbulk", 5, 0x06, 0, NULL },
    [463] = { "openat",      4, 0x02, 0, NULL },
    [464] = { "openat_nocancel", 4, 0x02, 0, NULL },
    [470] = { "fstatat64",   4, 0x06, 0, NULL },
    [472] = { "unlinkat",    3, 0x02, 0, NULL },
    [473] = { "readlinkat",  4, 0x06, 0, NULL },
    [475] = { "mkdirat",     3, 0x02, 0, NULL },
    [500] = { "getentropy",  2, 0x01, 0, NULL },
};

static void strace_bsd(OcerzVM *vm, const ocerz_bsd_entry *e, int num,
                       const uint64_t orig[8], OcerzCPU *cpu)
{
    if (!vm->strace)
        return;
    fprintf(stderr, "ocerz: [t%d] syscall %s(", cpu->cpu_number, e && e->name ? e->name : "?");
    int n = e ? e->nargs : 6;
    if (n > 6)
        n = 6;
    for (int i = 0; i < n; i++)
        fprintf(stderr, "%s%#llx", i ? ", " : "", (unsigned long long)orig[i]);
    if (cpu->rflags & OCERZ_CF)
        fprintf(stderr, ") = -1 %s\n", errno_name((int)cpu->gpr[OCERZ_RAX]));
    else
        fprintf(stderr, ") = %#llx\n", (unsigned long long)cpu->gpr[OCERZ_RAX]);
    if (e && e->name && (strcmp(e->name, "open") == 0 || strcmp(e->name, "access") == 0)) {
        char pbuf[128];
        for (int i = 0; i < 127; i++) {
            char c = (char)ocerz_ld(orig[0] + (uint64_t)i, 1);
            pbuf[i] = c;
            if (!c) break;
            pbuf[i + 1] = 0;
        }
        fprintf(stderr, "ocerz:    path[%#llx] = \"%s\"\n", (unsigned long long)orig[0], pbuf);
    }
    (void)num;
}

static int dispatch_bsd(OcerzVM *vm, OcerzCPU *cpu, int num)
{
    if (num == 0) {
        int real = (int)(cpu->gpr[OCERZ_RDI] & 0xffffff);
        uint64_t r9_save = cpu->gpr[OCERZ_R9];
        cpu->gpr[OCERZ_RDI] = cpu->gpr[OCERZ_RSI];
        cpu->gpr[OCERZ_RSI] = cpu->gpr[OCERZ_RDX];
        cpu->gpr[OCERZ_RDX] = cpu->gpr[OCERZ_R10];
        cpu->gpr[OCERZ_R10] = cpu->gpr[OCERZ_R8];
        cpu->gpr[OCERZ_R8] = r9_save;
        cpu->gpr[OCERZ_R9] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        return dispatch_bsd(vm, cpu, real);
    }

    const ocerz_bsd_entry *e = NULL;
    if (num >= 0 && num < OCERZ_BSD_MAX && bsd_table[num].name)
        e = &bsd_table[num];

    if (!e) {
        /* Probe (OCERZ_SYSPROBE): report a missing entry and hand the guest ENOSYS
         * instead of aborting, so one pass enumerates every syscall a target needs
         * rather than one fatal per rebuild. Each number reports once. This is a
         * table-building diagnostic, NOT a substitute for real entries -- ENOSYS is
         * a lie to the guest, and callers that cannot cope with it fail downstream
         * in ways that look like unrelated bugs. Never enable it for a real run. */
        if (getenv("OCERZ_SYSPROBE")) {
            static uint8_t probed[OCERZ_BSD_MAX];
            if (num >= 0 && num < OCERZ_BSD_MAX && !probed[num]) {
                probed[num] = 1;
                fprintf(stderr, "ocerz: SYSPROBE missing class=2 num=%d rip=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r10=%#llx\n",
                        num, (unsigned long long)cpu->rip,
                        (unsigned long long)cpu->gpr[OCERZ_RDI], (unsigned long long)cpu->gpr[OCERZ_RSI],
                        (unsigned long long)cpu->gpr[OCERZ_RDX], (unsigned long long)cpu->gpr[OCERZ_R10]);
            }
            ret_err(cpu, ENOSYS);
            return OCERZ_STEP_OK;
        }
        OCERZ_FATAL("unknown BSD syscall: class=2 num=%d (no table entry) rip=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r10=%#llx ret=%#llx\n", num,
                    (unsigned long long)cpu->rip, (unsigned long long)cpu->gpr[OCERZ_RDI],
                    (unsigned long long)cpu->gpr[OCERZ_RSI], (unsigned long long)cpu->gpr[OCERZ_RDX],
                    (unsigned long long)cpu->gpr[OCERZ_R10],
                    (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP], 8));
        return OCERZ_STEP_FATAL;
    }

    uint64_t a[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    a[0] = cpu->gpr[OCERZ_RDI];
    a[1] = cpu->gpr[OCERZ_RSI];
    a[2] = cpu->gpr[OCERZ_RDX];
    a[3] = cpu->gpr[OCERZ_R10];
    a[4] = cpu->gpr[OCERZ_R8];
    a[5] = cpu->gpr[OCERZ_R9];
    if (e->nargs > 6) {
        uint64_t rsp = cpu->gpr[OCERZ_RSP];
        for (int i = 6; i < e->nargs && i < 8; i++)
            a[i] = ocerz_ld(rsp + 8 * (uint64_t)(i - 6 + 1), 8);
    }

    uint64_t orig[8];
    memcpy(orig, a, sizeof orig);

    if (e->intercept) {
        int r = e->intercept(vm, cpu, a);
        if (r == OCERZ_STEP_FATAL) {
            OCERZ_FATAL("not yet supported: class=2 num=%d name=%s\n", num, e->name);
            return OCERZ_STEP_FATAL;
        }
        strace_bsd(vm, e, num, orig, cpu);
        return r;
    }

    for (int i = 0; i < 8; i++) {
        if ((e->ptr_mask & (1u << i)) && a[i] != 0)
            a[i] = (uint64_t)(uintptr_t)ocerz_g2h(a[i]);
    }

    int err = 0;
    uint64_t ret2 = 0;
    /* ULOCK trace (gated): who waits/wakes which os_unfair_lock word and with what
     * owner value. orig[] holds the GUEST args (orig[1]=lock addr guest-side). The
     * guest kport (this thread's published owner identity) lives at gs_base+0x18
     * (pth+0xf8). The wait fprintf runs BEFORE the (possibly infinite) blocking call
     * so a permanently-parked waiter still prints its lock+expected-owner. */
    if ((num == 515 || num == 516 || num == 544) && getenv("OCERZ_ULOCKLOG")) {
        uint64_t myport = cpu->gs_base ? ocerz_ld(cpu->gs_base + 0x18, 4) : 0;
        fprintf(stderr, "ocerz: ULOCK[%d] cpu#%u %s op=%#llx addr=%#llx val=%#llx myport=%#llx caller=%#llx\n",
                (int)getpid(), cpu->cpu_number, e->name,
                (unsigned long long)orig[0], (unsigned long long)orig[1],
                (unsigned long long)orig[2], (unsigned long long)myport,
                (unsigned long long)cpu->rip);
    }
    /* Log an INFINITE-timeout kevent WAIT before it blocks (orig[5]==NULL timeout,
     * nchanges==0, nevents>0): a wait that never returns (the EVFILT_USER lost-wakeup
     * hang) leaves no post-syscall KEV line, so this is the only way to see the kq the
     * stuck waiter is parked on. Excludes the 16ms libdispatch pollers (timeout!=NULL). */
    if ((num == 363 || num == 369) && getenv("OCERZ_KEVLOG") &&
        orig[2] == 0 && orig[4] == 1)
        fprintf(stderr, "ocerz: KEVWAIT-ENTER[%d] num=%d kq=%lld(=fd %d) nev=%lld timeout=%#llx caller=%#llx\n",
                (int)getpid(), num, (long long)orig[0], (int)orig[0], (long long)orig[4],
                (unsigned long long)orig[5], (unsigned long long)cpu->rip);
    uint64_t r = ocerz_host_syscall(num, a, &ret2, &err);
    if ((num == 362 || num == 363 || num == 369) && getenv("OCERZ_KEVLOG")) {
        static int kev_dumped = 0;
        if (!kev_dumped) { kev_dumped = 1; ocerz_dyld_dump_images(); }
        uint64_t rh[8]; unsigned rn = ocerz_vm_riphist(rh, 8);
        uint64_t cbase = 0; const char *cmod = NULL;
        for (unsigned i = 0; i < rn; i++) {
            const char *m = ocerz_dyld_name_for_addr(rh[i], &cbase);
            if (m) { cmod = m; break; }
        }
        int64_t ts0 = -1, ts1 = -1;
        if (num != 362 && orig[5]) { ts0 = (int64_t)ocerz_ld(orig[5], 8); ts1 = (int64_t)ocerz_ld(orig[5] + 8, 8); }
        (void)cmod; (void)cbase;
        static int kev_poll_seen = 0;
        if (num != 362 && orig[2] == 0 && orig[4] > 0) kev_poll_seen++;
        if (kev_poll_seen == 400 && num != 362) {
            uint64_t rh32[32]; unsigned rn32 = ocerz_vm_riphist(rh32, 32);
            fprintf(stderr, "ocerz: KEVSTACK[%d] kq=%lld late-poll frames:", (int)getpid(), (long long)orig[0]);
            for (unsigned i = 0; i < rn32; i++) fprintf(stderr, " %#llx", (unsigned long long)rh32[i]);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "ocerz: KEV[%d] %s kq=%lld nchanges=%lld nevents=%lld timeout=%lld.%09lld ret=%lld err=%d caller=%#llx\n",
                (int)getpid(), e->name, (long long)orig[0], (long long)orig[2], (long long)orig[4],
                (long long)ts0, (long long)ts1, (long long)r, err,
                (unsigned long long)(rn ? rh[0] : 0));
        if (num != 362 && orig[1] && orig[2]) {
            for (uint64_t i = 0; i < orig[2] && i < 4; i++) {
                uint64_t ke = orig[1] + i * 32;
                fprintf(stderr, "ocerz:   change ident=%#llx filter=%d flags=%#x fflags=%#x\n",
                        (unsigned long long)ocerz_ld(ke, 8), (int)(int16_t)ocerz_ld(ke + 8, 2),
                        (unsigned)(uint16_t)ocerz_ld(ke + 10, 2), (unsigned)(uint32_t)ocerz_ld(ke + 12, 4));
            }
        }
        if (num != 362 && orig[3] && !err && (int64_t)r > 0) {
            for (int64_t i = 0; i < (int64_t)r && i < 2; i++) {
                uint64_t ke = orig[3] + (uint64_t)i * 32;
                fprintf(stderr, "ocerz:   EVENT ident=%#llx filter=%d flags=%#x fflags=%#x data=%#llx\n",
                        (unsigned long long)ocerz_ld(ke, 8), (int)(int16_t)ocerz_ld(ke + 8, 2),
                        (unsigned)(uint16_t)ocerz_ld(ke + 10, 2), (unsigned)(uint32_t)ocerz_ld(ke + 12, 4),
                        (unsigned long long)ocerz_ld(ke + 16, 8));
            }
        }
    }
    if (err) {
        ret_err(cpu, r);
    } else if (e->dual_ret) {
        ret_ok2(cpu, r, ret2);
    } else {
        ret_ok(cpu, r);
    }
    strace_bsd(vm, e, num, orig, cpu);
    return OCERZ_STEP_OK;
}

static const char *mach_trap_name(int num)
{
    switch (num) {
    case 10: return "_kernelrpc_mach_vm_allocate_trap";
    case 11: return "_kernelrpc_mach_vm_purgable_control_trap";
    case 12: return "_kernelrpc_mach_vm_deallocate_trap";
    case 14: return "_kernelrpc_mach_vm_protect_trap";
    case 15: return "_kernelrpc_mach_vm_map_trap";
    case 16: return "_kernelrpc_mach_port_allocate_trap";
    case 18: return "_kernelrpc_mach_port_deallocate_trap";
    case 19: return "_kernelrpc_mach_port_mod_refs_trap";
    case 26: return "mach_reply_port";
    case 40: return "_kernelrpc_mach_port_get_attributes_trap";
    case 27: return "thread_self_trap";
    case 28: return "task_self_trap";
    case 29: return "host_self_trap";
    case 31: return "mach_msg_trap";
    case 33: return "semaphore_signal_trap";
    case 34: return "semaphore_signal_all_trap";
    case 36: return "semaphore_wait_trap";
    case 37: return "semaphore_wait_signal_trap";
    case 38: return "semaphore_timedwait_trap";
    case 43: return "mach_generate_activity_id";
    case 44: return "task_name_for_pid";
    case 45: return "task_for_pid";
    case 46: return "pid_for_task";
    case 47: return "mach_msg2_trap";
    case 50: return "thread_get_special_reply_port";
    case 76: return "_kernelrpc_mach_port_type_trap";
    case 77: return "_kernelrpc_mach_port_request_notification_trap";
    case 59: return "swtch_pri";
    case 60: return "swtch";
    case 61: return "thread_switch";
    case 89: return "mach_timebase_info_trap";
    case 90: return "mach_wait_until_trap";
    case 91: return "mk_timer_create_trap";
    case 92: return "mk_timer_destroy_trap";
    case 93: return "mk_timer_arm_trap";
    case 94: return "mk_timer_cancel_trap";
    case 95: return "mk_timer_arm_leeway_trap";
    default: return NULL;
    }
}

static void mig_vm_reply_relocate(OcerzVM *vm, uint64_t reply_buf)
{
    uint64_t haddr = ocerz_ld(reply_buf + 0x24, 8);
    if (haddr == 0 || (haddr >= ocerz_arena_lo && haddr < ocerz_arena_hi))
        return;
    mach_vm_address_t raddr = haddr;
    mach_vm_size_t rsize = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objname = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &raddr, &rsize, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&info, &cnt, &objname) != KERN_SUCCESS)
        return;
    if (objname != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), objname);
    if (raddr > haddr)
        return;
    /* The shared mapping (e.g. the LaunchServices CSStore DB delivered as "lsinfopage=SharedMemory")
     * can be SPLIT by the kernel into multiple CONTIGUOUS vm regions with different protections
     * (observed: a 0x1060000 RW region + a contiguous 0x20000 region). mach_vm_region returns only
     * the FIRST, so relocating just that truncates the guest's view of the object and a later read of
     * the full object (CSStore::VM::AllocateCopy's ~17MB memmove) faults on the un-relocated tail /
     * inter-donate gap. Coalesce every region CONTIGUOUS with the first and BACKED BY A MEMORY OBJECT
     * (a real gap, or an anonymous region, ends the shared object -- the kernel places a freshly
     * mapped object in free space, so its contiguous object-backed regions all belong to it). */
    uint64_t rend = (uint64_t)raddr + (uint64_t)rsize;
    int nregions = 1;
    for (int i = 0; i < 4096; i++) {
        mach_vm_address_t na = rend; mach_vm_size_t ns = 0;
        vm_region_basic_info_data_64_t ni; mach_msg_type_number_t nc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t no = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &na, &ns, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&ni, &nc, &no) != KERN_SUCCESS)
            break;
        if (no != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), no);
        if (na != rend)   /* a real GAP ends this mapping; the kernel placed the whole object in
                           * one free span, so every region contiguous with the first belongs to it
                           * (the split tail can be an ANONYMOUS COW region, not object-backed) */
            break;
        rend += ns;
        nregions++;
    }
    uint64_t size = rend - haddr;
    if (getenv("OCERZ_MACHMSG"))
        fprintf(stderr, "ocerz: MIGRELOC id=%u haddr=%#llx raddr=%#llx first_rsize=%#llx coalesced_size=%#llx nregions=%d prot=%d\n",
                (uint32_t)ocerz_ld(reply_buf + 0x14, 4), (unsigned long long)haddr,
                (unsigned long long)raddr, (unsigned long long)rsize, (unsigned long long)size,
                nregions, info.protection);
    uint64_t gaddr = ocerz_map_donate(size);
    if (gaddr == 0)
        return;
    mach_vm_address_t dst = gaddr;
    vm_prot_t curp = 0, maxp = 0;
    ocerz_jit_require_ordered(vm);
    kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, size, 0,
                                     VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                     mach_task_self(), haddr, FALSE,
                                     &curp, &maxp, VM_INHERIT_DEFAULT);
    if (kr != KERN_SUCCESS || dst != gaddr)
        return;
    mach_vm_deallocate(mach_task_self(), haddr, size);
    ocerz_st(reply_buf + 0x24, 8, gaddr);
    if (vm->strace)
        fprintf(stderr, "ocerz: mig_vm relocate host=%#llx size=%#llx -> guest=%#llx\n",
                (unsigned long long)haddr, (unsigned long long)size,
                (unsigned long long)gaddr);
}

/* Relocate inbound OOL descriptors of a RECEIVED COMPLEX mach_msg2 reply (case 47).
 * ocerz_host_mach_trap forwards send+recv to the host kernel, which places any OOL data
 * (OOL / OOL_PORTS / OOL_VOLATILE) at HOST addresses IT chose (e.g. 0x102bec000, below the
 * arena, tracked by NO ocerz MemRegion). case 47 previously relocated only the non-complex
 * reply scalars (mig_vm ids 4900/4911/4913 at +0x24, the 3712 thread handle at +0x30) and
 * NEVER walked a complex body -- so each received OOL descriptor kept the raw host VA. Under
 * identity g2h the guest can read it only while that transient host mapping is alive, and the
 * guest's MIG-contract vm_deallocate of it (case 12 -> ocerz_unmap) no-ops (region_for_range
 * NULL) so the region is never freed; a later kernel/ocerz VA reuse then makes libxpc fault on
 * the stale pointer (AppKit->LaunchServices check-in). Mirror ocerz_bridge_mach_msg (the async
 * kevent path's relocator): copy each inbound OOL into a fresh guest-arena buffer and patch the
 * descriptor, giving received OOL a first-class guest-owned lifecycle (case 12/ocerz_unmap then
 * frees the arena copy correctly), exactly like a native macOS receiver -- no change needed
 * elsewhere. recv_size bounds the walk to the actual receive buffer. */
static void ocerz_reply_relocate_ool(uint64_t reply_buf, uint32_t recv_size)
{
    uint32_t bits = (uint32_t)ocerz_ld(reply_buf, 4);
    if (!(bits & 0x80000000u))                          /* not COMPLEX */
        return;
    uint32_t sz = (uint32_t)ocerz_ld(reply_buf + 4, 4); /* msgh_size */
    if (recv_size && recv_size < sz)                    /* bound to the recv buffer */
        sz = recv_size;
    uint32_t dcnt = (uint32_t)ocerz_ld(reply_buf + 0x18, 4);
    if (dcnt == 0 || dcnt > 4096)
        return;
    static int rlog = -1;
    if (rlog < 0) rlog = getenv("OCERZ_MACHMSG") ? 1 : 0;
    static int ooltrace = -1;
    if (ooltrace < 0) ooltrace = getenv("OCERZ_OOLTRACE") ? 1 : 0;
    uint32_t reply_id = (uint32_t)ocerz_ld(reply_buf + 0x14, 4);
    uint64_t off = 0x1c;
    for (uint32_t d = 0; d < dcnt; d++) {
        if (off + 12 > sz)
            break;
        uint8_t type = (uint8_t)ocerz_ld(reply_buf + off + 11, 1);
        if (type == 0) { off += 12; continue; }         /* PORT: name only (shared namespace) */
        if (type == 4) { off += 16; continue; }         /* GUARDED_PORT: name+guard, no OOL data */
        if (type == 1 || type == 2 || type == 3) {      /* OOL / OOL_PORTS / OOL_VOLATILE */
            if (off + 16 > sz)
                break;
            uint64_t ool_addr = ocerz_ld(reply_buf + off, 8);
            uint32_t ool_n = (uint32_t)ocerz_ld(reply_buf + off + 12, 4);
            /* type 2 = OOL_PORTS: the +12 field is a COUNT of port names (4 bytes each). */
            uint64_t bytes = (type == 2) ? (uint64_t)ool_n * 4u : ool_n;
            int host_owned = ool_addr &&
                             !(ool_addr >= ocerz_arena_lo && ool_addr < ocerz_arena_hi);
            if (rlog)
                fprintf(stderr, "ocerz: REPLY-OOL type=%u ool_addr=%#llx bytes=%#llx host=%d%s\n",
                        type, (unsigned long long)ool_addr, (unsigned long long)bytes, host_owned,
                        bytes > 0x1000000 ? "  **OVER-16MB-LEFT-RAW**" : "");
            if (ooltrace && host_owned && bytes > 0x1000000)
                fprintf(stderr, "ocerz: OOLTRACE-RAW16 id=%u type=%u ool_addr=%#llx bytes=%#llx LEFT-RAW\n",
                        reply_id, type, (unsigned long long)ool_addr, (unsigned long long)bytes);
            if (host_owned && bytes && bytes <= 0x1000000) {
                uint64_t gb = ocerz_map_anywhere((bytes + 0x3fffull) & ~0x3fffull,
                                                 PROT_READ | PROT_WRITE);
                if (gb) {
                    memcpy(ocerz_g2h(gb), (const void *)(uintptr_t)ool_addr, (size_t)bytes);
                    ocerz_st(reply_buf + off, 8, gb);       /* desc.addr -> guest copy */
                    if (ooltrace)
                        fprintf(stderr, "ocerz: OOLTRACE-COPY id=%u type=%u gb=%#llx..%#llx bytes=%#llx\n",
                                reply_id, type, (unsigned long long)gb,
                                (unsigned long long)(gb + bytes), (unsigned long long)bytes);
                } else {
                    ocerz_st(reply_buf + off, 8, 0);        /* alloc failed: deliver an EMPTY */
                    ocerz_st(reply_buf + off + 12, 4, 0);   /* OOL, never a raw host pointer */
                }
            }
            off += 16;
            continue;
        }
        if (ooltrace)
            fprintf(stderr, "ocerz: OOLTRACE-BREAK id=%u off=%#llx type=%u (remaining desc left raw)\n",
                    reply_id, (unsigned long long)off, (uint8_t)ocerz_ld(reply_buf + off + 11, 1));
        break;                                          /* unknown stride: stop */
    }
}

static int dispatch_mach(OcerzVM *vm, OcerzCPU *cpu, int num)
{
    uint64_t a[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    a[0] = cpu->gpr[OCERZ_RDI];
    a[1] = cpu->gpr[OCERZ_RSI];
    a[2] = cpu->gpr[OCERZ_RDX];
    a[3] = cpu->gpr[OCERZ_R10];
    a[4] = cpu->gpr[OCERZ_R8];
    a[5] = cpu->gpr[OCERZ_R9];

    switch (num) {
    case 10: {
        uint64_t size = a[2];
        uint64_t flags = a[3];
        if (!(flags & 1)) {
            uint64_t want = a[1] ? ocerz_ld(a[1], 8) : 0;
            memtrace("vm_alloc", want, size, 0, (int)flags);
            if (want == 0 ||
                (ocerz_map_claim_fixed(want, size, PROT_READ | PROT_WRITE) != OCERZ_OK &&
                 ocerz_map_claim_region(want, size, PROT_READ | PROT_WRITE) != OCERZ_OK &&
                 (ocerz_mem_register_range(want, want + size) != OCERZ_OK ||
                  ocerz_map_claim_region(want, size, PROT_READ | PROT_WRITE) != OCERZ_OK))) {
                if (vm->strace)
                    fprintf(stderr, "ocerz: mach_vm_allocate FIXED denied want=%#llx size=%#llx flags=%#llx\n",
                            (unsigned long long)want, (unsigned long long)size,
                            (unsigned long long)flags);
                mach_ret(cpu, OCERZ_MACH_KERN_NO_SPACE);
                break;
            }
            mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS);
            break;
        }
        uint64_t gaddr = ocerz_map_anywhere(size, PROT_READ | PROT_WRITE);
        if (gaddr == 0) {
            mach_ret(cpu, OCERZ_MACH_KERN_NO_SPACE);
            break;
        }
        if (a[1] != 0)
            ocerz_st(a[1], 8, gaddr);
        mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS);
        break;
    }
    case 11: { /* _kernelrpc_mach_vm_purgable_control_trap(task, address, control, int *state) */
        /* ocerz manages the guest AS itself and never purges it, so guest purgeable memory
         * is always effectively NONVOLATILE. Report success and write NONVOLATILE (0) to the
         * state out-param (the new current state for GET, the previous state for SET) so the
         * guest never believes its purgeable data was discarded (EMPTY). */
        if (a[3] != 0)
            ocerz_st(a[3], 4, 0 /* VM_PURGABLE_NONVOLATILE */);
        mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS);
        break;
    }
    case 12: {
        memtrace("vm_dealloc", a[1], a[2], 0, 0);
        ocerz_unmap(a[1], a[2]);
        mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS);
        break;
    }
    case 14: {
        memtrace("vm_protect", a[1], a[2], (int)a[4], 0);
        ocerz_protect(a[1], a[2], (int)a[4]);
        mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS);
        break;
    }
    case 15: {
        uint64_t size = a[2];
        uint64_t mask = a[3];
        uint64_t flags = a[4];
        if (!(flags & 1)) {
            uint64_t want = a[1] ? ocerz_ld(a[1], 8) : 0;
            memtrace("vm_map", want, size, 0, (int)flags);
            if (want == 0 ||
                (ocerz_map_claim_fixed(want, size, PROT_READ | PROT_WRITE) != OCERZ_OK &&
                 ocerz_map_claim_region(want, size, PROT_READ | PROT_WRITE) != OCERZ_OK &&
                 (ocerz_mem_register_range(want, want + size) != OCERZ_OK ||
                  ocerz_map_claim_region(want, size, PROT_READ | PROT_WRITE) != OCERZ_OK))) {
                if (vm->strace)
                    fprintf(stderr, "ocerz: mach_vm_map FIXED denied want=%#llx size=%#llx mask=%#llx flags=%#llx\n",
                            (unsigned long long)want, (unsigned long long)size,
                            (unsigned long long)mask, (unsigned long long)flags);
                mach_ret(cpu, OCERZ_MACH_KERN_NO_SPACE);
                break;
            }
            mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS);
            break;
        }
        uint64_t gaddr = mask ? ocerz_map_anywhere_aligned(size, PROT_READ | PROT_WRITE, mask + 1)
                              : ocerz_map_anywhere(size, PROT_READ | PROT_WRITE);
        if (gaddr == 0) {
            mach_ret(cpu, OCERZ_MACH_KERN_NO_SPACE);
            break;
        }
        if (a[1] != 0)
            ocerz_st(a[1], 8, gaddr);
        mach_ret(cpu, OCERZ_MACH_KERN_SUCCESS);
        break;
    }
    case 16: {
        if (a[2] != 0)
            a[2] = (uint64_t)(uintptr_t)ocerz_g2h(a[2]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 24: {
        if (a[1] != 0)
            a[1] = (uint64_t)(uintptr_t)ocerz_g2h(a[1]);
        if (a[3] != 0)
            a[3] = (uint64_t)(uintptr_t)ocerz_g2h(a[3]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 40: {
        /* _kernelrpc_mach_port_get_attributes_trap(target, name, flavor,
         * port_info_out, port_info_outCnt): the kernel writes the info array and
         * its count back through args 3 and 4, so both are guest buffers that need
         * translating. target/name are port names in the shared namespace and pass
         * through untranslated like every other _kernelrpc port trap. */
        if (a[3] != 0)
            a[3] = (uint64_t)(uintptr_t)ocerz_g2h(a[3]);
        if (a[4] != 0)
            a[4] = (uint64_t)(uintptr_t)ocerz_g2h(a[4]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 33:
    case 34:
    case 36:
    case 37:
    case 38:
    case 50:
    case 59:
    case 60:
    case 61: /* thread_switch: the yield sibling of swtch(60)/swtch_pri(59). Newly reached
              * once the workloop actually drains -- libdispatch's contended os_unfair_lock
              * slow-path yields via thread_switch; without forwarding it, that path hit the
              * fatal default. Forward to the host kernel exactly like its siblings. */
    case 70: {
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 31: {
        uint64_t gmsg31 = a[0];
        /* legacy mach_msg_trap: flat message, send size = header msgh_size@+4 */
        struct ocerz_ool_save sv31[64];
        int nsv31 = 0;
        if (gmsg31 != 0 && ocerz_low_base)
            nsv31 = ocerz_send_xlate_descriptors(gmsg31, (uint32_t)ocerz_ld(gmsg31 + 4, 4), sv31, 64);
        if (a[0] != 0)
            a[0] = (uint64_t)(uintptr_t)ocerz_g2h(a[0]);
        uint64_t r31 = ocerz_host_mach_trap(num, a);
        mach_ret(cpu, r31);
        if (nsv31)
            ocerz_send_restore_descriptors(gmsg31, sv31, nsv31);
        if (gmsg31 != 0 && ocerz_mach_err_interesting(r31))
            ocerz_log_mach_send_err(31, r31, a, gmsg31);
        break;
    }
    case 43: {
        if (a[2] != 0)
            a[2] = (uint64_t)(uintptr_t)ocerz_g2h(a[2]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 44:   /* task_name_for_pid(target_tport, pid, tn*) */
    case 45: { /* task_for_pid(target_tport, pid, t*) */
        if (a[2] != 0)
            a[2] = (uint64_t)(uintptr_t)ocerz_g2h(a[2]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 46: { /* pid_for_task(t, pid*) */
        if (a[1] != 0)
            a[1] = (uint64_t)(uintptr_t)ocerz_g2h(a[1]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 47: {
        uint32_t msgh_id = (uint32_t)(a[4] >> 32);
        uint64_t reply_buf = a[0];
        /* a[6] (rcv_size_and_priority) and a[7] (timeout) are the 7th/8th trap args and arrive on
         * the guest stack. Load them BEFORE the send-side translation: for a vector message the
         * RECEIVE entry count lives in a[6] and the translation below needs it. */
        a[6] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        a[7] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 16, 8);
        /* SEND-side OOL translation (faithful inverse of the inbound bridge). Skip MACH64_MSG_VECTOR
         * sends (opt bit 32): their a[0] is a vector array, not a flat message. send_size = a[2]>>32. */
        struct ocerz_ool_save sv47[64];
        int nsv47 = 0;
        if (reply_buf != 0 && ocerz_low_base) {
            if (a[1] & 0x100000000ull) {
                /* MACH64_MSG_VECTOR: a[0] is an array of mach_msg_vector_t, and for a vector
                 * message the two "sizes" are ENTRY COUNTS, not bytes -- send count in a[2]>>32,
                 * receive count in the low half of a[6]. Both directions index the SAME array, and
                 * they differ precisely when the message carries auxiliary data: a send of 1 entry
                 * (data) paired with a receive of 2 (data + aux) used to translate entry[0] only,
                 * leaving the aux entry's msgv_rcv_addr as a guest pointer. The kernel then never
                 * wrote the aux header into the guest's buffer, so libdispatch read a stale
                 * msgdh_size and killed the worker with "BUG IN LIBDISPATCH: Invalid msg aux data
                 * size". Cover the union of both directions. */
                uint32_t sc = (a[1] & 0x1) ? (uint32_t)(a[2] >> 32) : 0;
                uint32_t rc = (a[1] & 0x2) ? (uint32_t)(a[6] & 0xffffffffu) : 0;
                nsv47 = ocerz_send_xlate_vector(reply_buf, sc > rc ? sc : rc,
                                                (a[1] & 0x1) != 0, (a[1] & 0x2) != 0, sv47, 64);
            } else
                nsv47 = ocerz_send_xlate_descriptors(reply_buf, (uint32_t)(a[2] >> 32), sv47, 64);
        }
        if (a[0] != 0)
            a[0] = (uint64_t)(uintptr_t)ocerz_g2h(a[0]);
        /* OCERZ_LSTIMEOUT (diagnostic): the LaunchServices _LSApplicationCheckIn family (msgh_id
         * 10000-range, SEND|RCV, no app-set RCV_TIMEOUT) deadlocks because the real coreservicesd
         * never replies to a non-LS-registered Wine guest. Force a bounded RCV timeout: if the trap
         * returns MACH_RCV_TIMED_OUT the send was delivered and only the reply is missing (policy
         * gap); if it returns a reply that then faults, it is the inbound-OOL-relocation bug instead.
         * Also reveals whether AppKit can proceed past a timed-out check-in. */
        {
            static int g_lstimeout = -1;
            if (g_lstimeout < 0) g_lstimeout = getenv("OCERZ_LSTIMEOUT") ? 1 : 0;
            if (g_lstimeout && reply_buf != 0 && (a[1] & 0x1) && (a[1] & 0x2) && !(a[1] & 0x100)) {
                uint32_t mid = (uint32_t)(a[4] >> 32);
                if (mid >= 10000 && mid < 10100) {
                    a[1] |= 0x100;   /* MACH_RCV_TIMEOUT */
                    a[7] = 1500;     /* ms */
                    if (getenv("OCERZ_MACHMSG"))
                        fprintf(stderr, "ocerz: LSTIMEOUT forcing 1500ms RCV timeout on id=%u\n", mid);
                }
            }
        }
        if (getenv("OCERZ_MSGTIMEOUT"))
            fprintf(stderr, "ocerz: MSG2 opts=%#llx rcv_timeout_arg=%#llx a6=%#llx rsp=%#llx\n",
                    (unsigned long long)a[1], (unsigned long long)a[7],
                    (unsigned long long)a[6], (unsigned long long)cpu->gpr[OCERZ_RSP]);
        /* Log the message header BEFORE the (possibly blocking) trap, so a mach_msg2
         * that never returns (the boot keystone deadlock) still shows its dest port /
         * id / options. msgh_bits@0, remote_port@8, local_port@0xc, msgh_id@0x14. */
        if (reply_buf != 0 && OCERZ_ENV_ON("OCERZ_MACHMSG"))
            fprintf(stderr, "ocerz: MACHMSG-ENTER[%d] opts=%#llx voucher|id=%#llx bits=%#x rport=%#x lport=%#x id=%u xlated=%d\n",
                    (int)getpid(), (unsigned long long)a[1], (unsigned long long)a[4],
                    (uint32_t)ocerz_ld(reply_buf, 4), (uint32_t)ocerz_ld(reply_buf + 8, 4),
                    (uint32_t)ocerz_ld(reply_buf + 0xc, 4), (uint32_t)ocerz_ld(reply_buf + 0x14, 4), nsv47);
        if (reply_buf != 0 && OCERZ_ENV_ON("OCERZ_VMMAPPROBE") &&
            (uint32_t)ocerz_ld(reply_buf + 0x14, 4) == 4811) {
            fprintf(stderr, "ocerz: VMMAPREQ id=4811 dcnt=%u portname=%#x raw18:",
                    (uint32_t)ocerz_ld(reply_buf + 0x18, 4), (uint32_t)ocerz_ld(reply_buf + 0x1c, 4));
            for (int k = 0x18; k < 0x60; k += 4)
                fprintf(stderr, " %08x", (uint32_t)ocerz_ld(reply_buf + (uint64_t)k, 4));
            fprintf(stderr, "\n");
        }
        /* OCERZ_MSGDUMP: print the message body as ASCII when it carries a recognizable service name
         * (bootstrap look-ups inline the name), to identify which daemon a stalling MIG call targets. */
        if (reply_buf != 0 && OCERZ_ENV_ON("OCERZ_MSGDUMP")) {
            char asc[321]; int ai = 0;
            for (int k = 0x18; k < 0x158 && ai < 320; k++) {
                uint8_t c = (uint8_t)ocerz_ld(reply_buf + (uint64_t)k, 1);
                asc[ai++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            asc[ai] = 0;
            uint32_t dmid = (uint32_t)(a[4] >> 32);
            if (strstr(asc, "com.apple") || strstr(asc, "apple.") || strstr(asc, "Server") ||
                strstr(asc, "font") || strstr(asc, "WindowServer") ||
                (dmid >= 10000 && dmid < 10100))
                fprintf(stderr, "ocerz: MSGDUMP[%d] id=%u rport=%#x: %s\n", (int)getpid(),
                        (uint32_t)(a[4] >> 32), (uint32_t)ocerz_ld(reply_buf + 8, 4), asc);
        }
        uint64_t r47 = ocerz_host_mach_trap(num, a);
        mach_ret(cpu, r47);
        if (nsv47)
            ocerz_send_restore_descriptors(reply_buf, sv47, nsv47);
        /* MACH_SEND_* failures (kr 0x1000000x; the 0x10004xxx range is benign RCV results)
         * on a message ocerz forwarded: libxpc treats MACH_SEND_INVALID_DATA (0x10000002) as
         * fatal "Malformed Mach message" and os_crashes. Dump the outgoing message structure. */
        if (reply_buf != 0 && ocerz_mach_err_interesting(r47))
            ocerz_log_mach_send_err(47, r47, a, reply_buf);
        static int machleak = -1;
        if (machleak < 0) machleak = getenv("OCERZ_MACHLEAK") != NULL ? 1 : 0;
        if (reply_buf != 0 && machleak) {
            /* Floor lowered from 0x140000000 to the 4GB executable base, and the scan
             * widened past the first eight words. The observed LaunchServices leak
             * dereferences ~0x104e00000 -- BELOW the old floor and past off 0x80 -- so
             * this scanner was blind to precisely the case that crashes. Screening on
             * ocerz_addr_committed still separates a leaked HOST pointer (never
             * committed in the guest arena) from a legitimate guest pointer in the
             * same numeric range, which is what the old floor was crudely approximating. */
            for (uint64_t off = 0x18; off <= 0x158; off += 8) {
                uint64_t v = ocerz_ld(reply_buf + off, 8);
                if (v >= 0x100000000ull && v < OCERZ_LOW_LIMIT &&
                    ocerz_addr_committed(v) == 0)
                    fprintf(stderr,
                            "ocerz: MACHLEAK reply_id=%u off=%#llx host_val=%#llx icount=%#llx\n",
                            (uint32_t)ocerz_ld(reply_buf + 0x14, 4),
                            (unsigned long long)off, (unsigned long long)v,
                            (unsigned long long)vm->insn_count);
            }
        }
        /* Inbound-OOL relocate for a successfully RECEIVED flat COMPLEX reply: copy each
         * host-owned OOL descriptor into a guest-arena buffer so the guest never holds a
         * transient host pointer (see ocerz_reply_relocate_ool). Guard: a[1]&0x2 =
         * MACH64_RCV_MSG (a reply was received); !(a[1]&bit32) = flat, not MACH64_MSG_VECTOR
         * (a vector recv lands in seg buffers, not reply_buf -- follow-up); r47==0 =
         * MACH_MSG_SUCCESS; (uint32_t)a[6] = the trap's rcv_size arg bounding the walk.
         * Orthogonal to the non-complex 4900/4911/4913/3712/8000 fixups (helper early-returns
         * on any non-complex reply). */
        if (reply_buf != 0 && (a[1] & 0x2) && !(a[1] & 0x100000000ull) && r47 == 0)
            ocerz_reply_relocate_ool(reply_buf, (uint32_t)a[6]);
        if (reply_buf != 0) {
            uint32_t rid = (uint32_t)ocerz_ld(reply_buf + 0x14, 4);
            if ((rid == 4900 || rid == 4911 || rid == 4913) &&
                (uint32_t)ocerz_ld(reply_buf + 0x20, 4) == OCERZ_MACH_KERN_SUCCESS)
                mig_vm_reply_relocate(vm, reply_buf);
            /* Phase 4 K1: thread_info(THREAD_IDENTIFIER_INFO) reply (id 3712)
             * carries thread_handle (the thread's cthread_self / %gs base) at
             * +0x30 and the dispatch-queue address at +0x38. The host kernel
             * fills them with the HOST arm64 thread's values; Wine installs
             * thread_handle into %gs via thread_fast_set_cthread_self and then
             * faults reading the guest-mapped shadow page (%gs:-8). Substitute
             * the GUEST thread's cthread_self (cpu->gs_base, set at thread
             * creation) and carry the dispatch-qaddr delta. Gated on the kernel
             * actually having leaked an uncommitted host-range handle so other
             * thread_info flavors reusing id 3712 are left untouched. */
            if (rid == 3712) {
                uint64_t h = ocerz_ld(reply_buf + 0x30, 8);
                if (h >= 0x140000000ull && h < OCERZ_LOW_LIMIT &&
                    ocerz_addr_committed(h) == 0) {
                    uint64_t qa = ocerz_ld(reply_buf + 0x38, 8);
                    ocerz_st(reply_buf + 0x30, 8, cpu->gs_base);
                    ocerz_st(reply_buf + 0x38, 8, cpu->gs_base + (qa - h));
                    if (getenv("OCERZ_SIGTRACE"))
                        fprintf(stderr,
                                "ocerz: K1 thread_info handle %#llx -> gs_base %#llx (comm=%d)\n",
                                (unsigned long long)h,
                                (unsigned long long)cpu->gs_base,
                                ocerz_addr_committed(cpu->gs_base));
                }
            }
        }
        if (msgh_id == 8000 && reply_buf != 0 &&
            (uint32_t)ocerz_ld(reply_buf + 4, 4) == 0x24 &&
            (uint32_t)ocerz_ld(reply_buf + 0x20, 4) == OCERZ_MACH_KERN_NOT_SUPPORTED)
            ocerz_st(reply_buf + 0x20, 4, OCERZ_MACH_KERN_SUCCESS);
        break;
    }
    case 76: { /* _kernelrpc_mach_port_type_trap(task, name, ptype*) */
        if (a[2] != 0)
            a[2] = (uint64_t)(uintptr_t)ocerz_g2h(a[2]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 77: {
        a[6] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        if (a[6] != 0)
            a[6] = (uint64_t)(uintptr_t)ocerz_g2h(a[6]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 89: {
        /* mach_timebase_info: report 1/1, NOT the host's.
         *
         * The guest reads mach_absolute_time in NANOSECONDS (measured: over the same busy
         * loop the raw delta was 1.4836e10 against 14.836 s of real time). Apple silicon's
         * own timebase is 125/3, because the HOST's mach_absolute_time counts 24 MHz ticks.
         * Forwarding that to a guest whose clock is already in nanoseconds made every
         * tick->ns conversion 41.67x too large, so every computed deadline landed ~42x too
         * far in the future. CoreFoundation's run loop is the visible casualty:
         * CFRunLoopRunInMode(mode, 1.0, false) never reached its deadline and blocked in
         * mach_msg2 forever at 0% CPU, so winemac.drv's run_cocoa_app could never be
         * serviced and Wine reported "Failed to start Cocoa app main loop".
         *
         * 1/1 is exactly what Rosetta 2 reports to a translated x86_64 process, and what a
         * real Intel Mac reports, for the same reason: there, mach_absolute_time is already
         * nanoseconds. Writing the struct directly rather than forwarding also keeps the
         * two halves consistent by construction. */
        if (a[0] != 0) {
            ocerz_st(a[0] + 0, 4, 1);   /* numer */
            ocerz_st(a[0] + 4, 4, 1);   /* denom */
        }
        mach_ret(cpu, 0);               /* KERN_SUCCESS */
        break;
    }
    case 90:   /* mach_wait_until_trap(deadline) */
    case 91:   /* mk_timer_create_trap() */
    case 92:   /* mk_timer_destroy_trap(name) */
    case 93:   /* mk_timer_arm_trap(name, expire_time) */
    case 95: { /* mk_timer_arm_leeway_trap(name, flags, expire_time, leeway) */
        /* ABSOLUTE DEADLINES CROSS A UNIT BOUNDARY and must be converted.
         *
         * The guest's mach_absolute_time is NANOSECONDS -- the same magnitude a real Intel
         * Mac and Rosetta report (measured side by side: 5.4617e14 under both). The HOST is
         * Apple silicon, where mach_absolute_time counts 24 MHz ticks and the timebase is
         * 125/3. Forwarding a nanosecond deadline to a kernel that reads ticks multiplies it
         * by 125/3 -- not a 42x-longer wait, but 42x the whole uptime: a deadline ~6 days out
         * became ~263 days out, so the timer simply never fired.
         *
         * That is the whole of the Phase-5 wall. CoreFoundation's run loop arms an mk_timer
         * and blocks in mach_msg2 with MACH_RCV_TIMEOUT deliberately NOT set, waiting for
         * that timer's message; when the timer never fires the main thread parks forever at
         * 0% CPU, run_cocoa_app is never serviced, and winemac.drv reports "Failed to start
         * Cocoa app main loop" after its 5s deadline. Verified directly:
         * mach_wait_until(now + 1s) returns in 1005 ms natively and never returned here. */
        uint64_t d0 = a[0], d1 = a[1], d2 = a[2], d3 = a[3];
        if (num == 90)
            a[0] = ocerz_guest_ns_to_host_ticks(a[0]);
        else if (num == 93)
            a[1] = ocerz_guest_ns_to_host_ticks(a[1]);
        else if (num == 95) {
            a[2] = ocerz_guest_ns_to_host_ticks(a[2]);
            a[3] = ocerz_guest_ns_to_host_ticks(a[3]);   /* leeway is a duration, same scale */
        }
        (void)d0; (void)d1; (void)d2; (void)d3;
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 94: { /* mk_timer_cancel_trap(name, result_time*) */
        if (a[1] != 0)
            a[1] = (uint64_t)(uintptr_t)ocerz_g2h(a[1]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 41: { /* _kernelrpc_mach_port_guard_trap(task, name, guard*, strict);
                * the guard cookie/struct (a[2]) is a guest pointer. */
        if (a[2] != 0)
            a[2] = (uint64_t)(uintptr_t)ocerz_g2h(a[2]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 100: { /* iokit_user_client_trap(connection, index, p1..p6): the IOKit fast-path trap.
                 * CoreAnimation's IOSurface-backed layer commit (IOSurfaceClientLock, via CoreUI
                 * image providers) reaches it during CA::Context::commit_transaction; without it
                 * a Cocoa window's content never renders. connection (a[0]) is a real host port
                 * (shared namespace); p1..p6 are scalars the user client's trap method interprets
                 * (surface refs/options, not guest pointers). p5/p6 are stack args. Forward. */
        a[6] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        a[7] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 16, 8);
        uint64_t iokr = ocerz_host_mach_trap(num, a);
        static int iokitlog = -1;
        if (iokitlog < 0) iokitlog = getenv("OCERZ_IOKITLOG") ? 1 : 0;
        if (iokitlog)
            fprintf(stderr, "ocerz: IOKIT-TRAP conn=%#llx index=%llu a=%#llx,%#llx,%#llx,%#llx,%#llx,%#llx -> ret=%#llx\n",
                    (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2],
                    (unsigned long long)a[3], (unsigned long long)a[4], (unsigned long long)a[5],
                    (unsigned long long)a[6], (unsigned long long)a[7], (unsigned long long)iokr);
        mach_ret(cpu, iokr);
        break;
    }
    default: {
        const char *nm = mach_trap_name(num);
        OCERZ_FATAL("unknown Mach trap: class=1 num=%d name=%s rip=%#llx rdi=%#llx rsi=%#llx rdx=%#llx r10=%#llx ret=%#llx\n", num, nm ? nm : "?",
                    (unsigned long long)cpu->rip, (unsigned long long)a[0], (unsigned long long)a[1],
                    (unsigned long long)a[2], (unsigned long long)a[3],
                    (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP], 8));
        return OCERZ_STEP_FATAL;
    }
    }

    if (vm->strace) {
        const char *nm = mach_trap_name(num);
        fprintf(stderr, "ocerz: mach_trap %s(num=%d) = %#llx\n",
                nm ? nm : "?", num, (unsigned long long)cpu->gpr[OCERZ_RAX]);
    }
    return OCERZ_STEP_OK;
}

/* SYNTHETIC LDT (machdep 5 = i386_set_ldt, 6 = i386_get_ldt).
 *
 * Wine's WoW64 gets its 32-bit code selector and its per-thread 32-bit TEB from the LDT on
 * macOS: dlls/ntdll/unix/signal_x86_64.c allocates a cs32 entry once and an fs32 entry per
 * thread, both through i386_set_ldt. ocerz used to FATAL on this syscall, so Wine could
 * never even describe 32-bit code, let alone enter it.
 *
 * The table is SYNTHETIC and is never forwarded to the host -- deliberately. Rosetta 2
 * itself refuses the call ("rosetta error: i386_set_ldt(kLdtAutoAlloc) not supported"),
 * which is precisely why 32-bit Windows software does not work under Wine on Apple silicon
 * today. ocerz does not need the hardware LDT: it executes every guest instruction itself,
 * so a selector only has to mean something to ocerz. Remembering base/limit/D-bit per index
 * is enough to later resolve a segment load and a far branch.
 *
 * Descriptor layout is the standard Intel one packed into 8 bytes:
 *   limit 0-15 | base 16-39 | access 40-47 | limit 48-51 | flags 52-55 | base 56-63  */
#define OCERZ_LDT_MAX 512
struct ocerz_ldt_entry {
    uint64_t base;
    uint32_t limit;
    uint8_t  access;    /* type/DPL/present byte */
    uint8_t  big;       /* D/B: 1 = 32-bit default operand+address size */
    uint8_t  gran;      /* G: limit is in 4KB units */
    uint8_t  present;
};
static struct ocerz_ldt_entry g_ldt[OCERZ_LDT_MAX];
static int g_ldt_next = 1;                 /* index 0 is the null selector */
static pthread_mutex_t g_ldt_lock = PTHREAD_MUTEX_INITIALIZER;

/* Base of an LDT selector, or 0. sel is a full selector value (index<<3 | TI | RPL). */
uint64_t ocerz_ldt_base(uint32_t sel)
{
    uint32_t idx = sel >> 3;
    if (!(sel & 4) || idx == 0 || idx >= OCERZ_LDT_MAX)   /* bit 2 set = LDT, clear = GDT */
        return 0;
    return g_ldt[idx].present ? g_ldt[idx].base : 0;
}

/* Is this selector a 32-bit (D=1) code/data selector? Used by the mode switch. */
int ocerz_ldt_is_big(uint32_t sel)
{
    uint32_t idx = sel >> 3;
    if (!(sel & 4) || idx == 0 || idx >= OCERZ_LDT_MAX)
        return 0;
    return g_ldt[idx].present && g_ldt[idx].big;
}

static void ocerz_ldt_unpack(uint64_t d, struct ocerz_ldt_entry *e)
{
    e->limit   = (uint32_t)((d & 0xffffu) | (((d >> 48) & 0xfu) << 16));
    e->base    = (uint64_t)(((d >> 16) & 0xffffffu) | (((d >> 56) & 0xffu) << 24));
    e->access  = (uint8_t)((d >> 40) & 0xff);
    e->big     = (uint8_t)((d >> 54) & 1);        /* flags bit 2 = D/B */
    e->gran    = (uint8_t)((d >> 55) & 1);
    e->present = (uint8_t)((e->access >> 7) & 1);
    /* limit is kept RAW; G is remembered separately so i386_get_ldt can hand back the
     * exact descriptor the guest wrote. Scale only where an effective limit is needed. */
}

static uint64_t ocerz_ldt_pack(const struct ocerz_ldt_entry *e)
{
    uint64_t lim = e->limit, base = e->base;
    return (lim & 0xffffu)
         | ((base & 0xffffffu) << 16)
         | ((uint64_t)e->access << 40)
         | (((lim >> 16) & 0xfu) << 48)
         | ((uint64_t)(e->big & 1) << 54)
         | ((uint64_t)(e->gran & 1) << 55)
         | (((base >> 24) & 0xffu) << 56);
}

static int dispatch_machdep_ldt(OcerzCPU *cpu, int num)
{
    /* The prototype is i386_set_ldt(int, const union ldt_entry *, int) -- 32-bit ints, so
     * the register halves must be read as int32_t. Reading rdi as int64_t makes the
     * LDT_AUTO_ALLOC sentinel (-1, passed zero-extended as 0xffffffff) look POSITIVE. */
    int32_t start = (int32_t)cpu->gpr[OCERZ_RDI];
    uint64_t descs = cpu->gpr[OCERZ_RSI];
    int32_t count = (int32_t)cpu->gpr[OCERZ_RDX];
    if (count < 0 || count > OCERZ_LDT_MAX || descs == 0) {
        ret_err(cpu, EINVAL);
        return OCERZ_STEP_OK;
    }
    pthread_mutex_lock(&g_ldt_lock);
    if (num == 5) {                                  /* i386_set_ldt */
        int idx;
        if (start < 0) {                             /* LDT_AUTO_ALLOC */
            if (g_ldt_next + (int)count > OCERZ_LDT_MAX) {
                pthread_mutex_unlock(&g_ldt_lock);
                ret_err(cpu, ENOMEM);
                return OCERZ_STEP_OK;
            }
            idx = g_ldt_next;
            g_ldt_next += (int)count;
        } else {
            idx = (int)start;
            if (idx <= 0 || idx + (int)count > OCERZ_LDT_MAX) {
                pthread_mutex_unlock(&g_ldt_lock);
                ret_err(cpu, EINVAL);
                return OCERZ_STEP_OK;
            }
            if (idx + (int)count > g_ldt_next)
                g_ldt_next = idx + (int)count;
        }
        for (int i = 0; i < count; i++)
            ocerz_ldt_unpack(ocerz_ld(descs + (uint64_t)i * 8, 8), &g_ldt[idx + i]);
        pthread_mutex_unlock(&g_ldt_lock);
        if (getenv("OCERZ_LDTLOG"))
            fprintf(stderr, "ocerz: LDT set idx=%d count=%lld base=%#llx big=%d access=%#x\n",
                    idx, (long long)count, (unsigned long long)g_ldt[idx].base,
                    g_ldt[idx].big, g_ldt[idx].access);
        ret_ok(cpu, (uint64_t)(uint32_t)idx);        /* XNU returns the first index */
        return OCERZ_STEP_OK;
    }
    /* i386_get_ldt */
    int idx = (int)start;
    if (idx < 0 || idx + (int)count > OCERZ_LDT_MAX) {
        pthread_mutex_unlock(&g_ldt_lock);
        ret_err(cpu, EINVAL);
        return OCERZ_STEP_OK;
    }
    for (int i = 0; i < count; i++)
        ocerz_st(descs + (uint64_t)i * 8, 8, ocerz_ldt_pack(&g_ldt[idx + i]));
    pthread_mutex_unlock(&g_ldt_lock);
    ret_ok(cpu, (uint64_t)(uint32_t)count);
    return OCERZ_STEP_OK;
}

static int dispatch_machdep(OcerzVM *vm, OcerzCPU *cpu, int num)
{
    if (num == 5 || num == 6)
        return dispatch_machdep_ldt(cpu, num);
    if (num == 3) {
        uint64_t newgs = cpu->gpr[OCERZ_RDI];
        /* _thread_set_tsd_base with an obviously-invalid base (< 0x100000) while
         * the thread already has a valid gs: this is Wine's __wine_syscall_
         * dispatcher gs-restore reading pthread_teb from a Wine TEB that does not
         * exist (an ocerz workqueue/libdispatch worker running Wine code with no
         * Wine TEB -- the gs:0x320 crash). A real macOS thread in the dispatcher
         * always has a valid TEB; here the only sane base is the thread's own
         * pthread/cthread base (its current gs), so keep it instead of faulting. */
        if (newgs < 0x100000 && cpu->gs_base >= 0x100000) {
            if (getenv("OCERZ_GSTRACE"))
                fprintf(stderr, "ocerz: GS machdep KEEP gs=%#llx (rejected junk %#llx) rip=%#llx\n",
                        (unsigned long long)cpu->gs_base, (unsigned long long)newgs,
                        (unsigned long long)cpu->rip);
            ret_ok(cpu, 0x60);
            return OCERZ_STEP_OK;
        }
        cpu->gs_base = newgs;
        if (ocerz_gs_is_teb_band(newgs))
            cpu->wine_teb_base = newgs;
        ret_ok(cpu, 0x60);
        if (getenv("OCERZ_GSTRACE"))
            fprintf(stderr, "ocerz: GS machdep gs=%#llx rip=%#llx icount=%#llx%s\n",
                    (unsigned long long)cpu->gs_base, (unsigned long long)cpu->rip,
                    (unsigned long long)vm->insn_count,
                    cpu->gs_base < 0x100000 ? "  <<< SMALL/INVALID" : "");
        if (getenv("OCERZ_GSTRACE") && cpu->gs_base < 0x100000) {
            uint64_t r14 = cpu->gpr[OCERZ_R14];
            uint64_t td = ocerz_ld(r14, 8);
            fprintf(stderr, "ocerz:   GSBAD caller-ret=%#llx rdi=%#llx r14=%#llx rsp=%#llx [r14]=td=%#llx td_commit=%d [td+0x320]=%#llx [td-0]=%#llx [td+8]=%#llx\n",
                    (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP], 8),
                    (unsigned long long)cpu->gpr[OCERZ_RDI], (unsigned long long)r14,
                    (unsigned long long)cpu->gpr[OCERZ_RSP], (unsigned long long)td,
                    ocerz_addr_committed(td),
                    (unsigned long long)(td ? ocerz_ld(td + 0x320, 8) : 0),
                    (unsigned long long)(td ? ocerz_ld(td, 8) : 0),
                    (unsigned long long)(td ? ocerz_ld(td + 8, 8) : 0));
        }
        if (vm->strace || getenv("OCERZ_SIGTRACE"))
            fprintf(stderr,
                    "ocerz: machdep set_cthread_self gs=%#llx comm(gs)=%d comm(gs-8)=%d icount=%#llx\n",
                    (unsigned long long)cpu->gs_base,
                    ocerz_addr_committed(cpu->gs_base),
                    ocerz_addr_committed(cpu->gs_base - 8),
                    (unsigned long long)vm->insn_count);
        return OCERZ_STEP_OK;
    }
    OCERZ_FATAL("unknown machine-dependent syscall: class=3 num=%d\n", num);
    return OCERZ_STEP_FATAL;
}

/* Defined in vm.c: atomically take+clear this thread's pending async guest signal
 * mask (set by async_sig_handler when a host SIGUSR1/SIGQUIT cross-thread kick lands). */
extern uint32_t ocerz_take_pending_async_sig(void);

int ocerz_handle_syscall(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t rax = cpu->gpr[OCERZ_RAX];
    int class = (int)((rax >> 24) & 0xff);
    int num = (int)(rax & 0xffffff);

    /* OCERZ_SCCOUNT: low-overhead busy-phase profiler. Every 2M class-2 BSD syscalls,
     * dump the cumulative per-number histogram + the current loop's return address, to
     * find which syscall the multi-minute boot loop is hammering and where it lives. */
    static int g_sccount = -1;
    if (g_sccount < 0)
        g_sccount = getenv("OCERZ_SCCOUNT") ? 1 : 0;
    if (g_sccount && class == 2) {
        static uint64_t g_schist[640];
        static uint64_t g_sctotal;
        if (num >= 0 && num < 640)
            __atomic_fetch_add(&g_schist[num], 1, __ATOMIC_RELAXED);
        uint64_t t = __atomic_add_fetch(&g_sctotal, 1, __ATOMIC_RELAXED);
        if ((t & ((1ull << 16) - 1)) == 0) {
            uint64_t snap[640];
            for (int i = 0; i < 640; i++)
                snap[i] = __atomic_load_n(&g_schist[i], __ATOMIC_RELAXED);
            fprintf(stderr, "ocerz: SCCOUNT t=%lluM cur num=%d rip=%#llx ret=%#llx a0=%#llx a1=%#llx a2=%#llx | top:",
                    (unsigned long long)(t >> 20), num, (unsigned long long)cpu->rip,
                    (unsigned long long)ocerz_ld(cpu->gpr[OCERZ_RSP], 8),
                    (unsigned long long)cpu->gpr[OCERZ_RDI], (unsigned long long)cpu->gpr[OCERZ_RSI],
                    (unsigned long long)cpu->gpr[OCERZ_RDX]);
            for (int k = 0; k < 6; k++) {
                int best = -1;
                uint64_t bestv = 0;
                for (int i = 0; i < 640; i++)
                    if (snap[i] > bestv) { bestv = snap[i]; best = i; }
                if (best < 0) break;
                fprintf(stderr, " [%d]=%lluM", best, (unsigned long long)(bestv >> 20));
                snap[best] = 0;
            }
            fprintf(stderr, "\n");
        }
    }

    int rc;
    switch (class) {
    case 1:
        rc = dispatch_mach(vm, cpu, num);
        break;
    case 2:
        if (g_sccount) {
            uint64_t a0 = cpu->gpr[OCERZ_RDI], a1 = cpu->gpr[OCERZ_RSI], a2 = cpu->gpr[OCERZ_RDX];
            uint64_t ret = ocerz_ld(cpu->gpr[OCERZ_RSP], 8), ic0 = vm->insn_count;
            uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            rc = dispatch_bsd(vm, cpu, num);
            uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
            if (dt > 20000000ull) /* >20ms in one BSD call -> a busy-phase culprit */
                fprintf(stderr, "ocerz: SCSLOW num=%d dt=%llums addr=%#llx len=%#llx a2=%#llx ret=%#llx dicount=%#llx\n",
                        num, (unsigned long long)(dt / 1000000ull), (unsigned long long)a0,
                        (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)ret,
                        (unsigned long long)(vm->insn_count - ic0));
        } else {
            rc = dispatch_bsd(vm, cpu, num);
        }
        break;
    case 3:
        rc = dispatch_machdep(vm, cpu, num);
        break;
    default:
        /* A SYSCALL_CLASS_NONE (class 0) syscall is not a real macOS syscall: on
         * macOS 14+ the kernel raises SIGSYS for it, and Wine's WoW64 PE ntdll
         * deliberately relies on that — its NtXxx stubs do `syscall` with a raw
         * Windows syscall number (no macOS class bits), and Wine's registered
         * SIGSYS handler reads the saved rip/rax and routes into
         * __wine_syscall_dispatcher. We emulate the kernel faithfully: deliver
         * SIGSYS to the guest with the saved rip already past the syscall (the
         * handler computes frame->rip = rip+0xb and frame->rcx = rip). If no
         * SIGSYS handler is registered it is a genuine bad syscall -> fatal. */
        /* WoW64 PE class-0 syscalls always run with gs=TEB; __wine_syscall_
         * dispatcher resolves the thread's syscall_frame/TEB from gs. If gs is the
         * unix cthread base here (Wine's own signal handler swapped it via machdep
         * and ocerz never swapped back), the dispatcher reads cthread-relative junk
         * (rip 0x3fff66748 -> [0x8ffff8] uncommitted). Re-establish the gs=TEB
         * invariant the macOS-14 kernel guarantees, using the TEB this thread ran. */
        if (!ocerz_gs_is_teb_band(cpu->gs_base) && cpu->wine_teb_base &&
            ocerz_addr_committed(cpu->wine_teb_base)) {
            if (getenv("OCERZ_GSTRACE"))
                fprintf(stderr, "ocerz: SIGSYS gs %#llx -> TEB %#llx rip=%#llx\n",
                        (unsigned long long)cpu->gs_base,
                        (unsigned long long)cpu->wine_teb_base, (unsigned long long)cpu->rip);
            cpu->gs_base = cpu->wine_teb_base;
        }
        g_ocerz_deliver_src = 2;
        if (ocerz_signal_deliver(cpu, OCERZ_SIGSYS, 0, 0, 0))
            return OCERZ_STEP_OK;
        OCERZ_FATAL("unknown syscall class=%d num=%d (rax=%#llx) rip=%#llx\n",
                    class, num, (unsigned long long)rax, (unsigned long long)cpu->rip);
        return OCERZ_STEP_FATAL;
    }
    /* Async guest signal delivery at the syscall-return boundary: a wineserver
     * cross-thread kick (SIGUSR1/SIGQUIT, forwarded to the host kernel by
     * sys_pthread_kill) EINTRs this thread's blocking forwarded syscall (read(wait_fd),
     * kevent, ...) and sets a pending bit in async_sig_handler; build the guest signal
     * frame so the handler runs before the guest resumes -- the macOS contract the
     * wineserver relies on for system-APC / suspend / context delivery. */
    if (rc == OCERZ_STEP_OK) {
        uint32_t pend = ocerz_take_pending_async_sig();
        for (int s = 1; pend && s < 32; s++)
            if (pend & (1u << s)) {
                g_ocerz_deliver_src = 1;
                ocerz_signal_deliver(cpu, s, 0, 0, 0);
            }
    }
    return rc;
}
