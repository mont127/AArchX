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
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>

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
    (void)vm;
    uint64_t addr = a[0];
    uint64_t len = a[1];
    int prot = (int)a[2];
    int flags = (int)a[3];
    int fd = (int)(int32_t)a[4];
    uint64_t pos = a[5];
    int anon = (flags & MAP_ANON) != 0 || fd < 0;
    int fixed = (flags & MAP_FIXED) != 0;
    uint64_t gaddr;

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
            ret_ok(cpu, addr);
            return OCERZ_STEP_OK;
        }
        gaddr = ocerz_map_anywhere(len, prot);
        if (gaddr == 0) {
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
    ret_ok(cpu, 0x4000005f);
    return OCERZ_STEP_OK;
}

static int sys_workq_stub(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    (void)a;
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

#define OCERZ_KEVENT_QOS_S 0x40

static int sys_kevent_id(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8]);

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
    struct ocerz_worker *w = (struct ocerz_worker *)calloc(1, sizeof *w);
    if (!w)
        return -1;
    w->vm = vm;
    w->cpu = *tmpl;
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
}

static int sys_fork(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    (void)a;
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
    (void)vm;
    const char *self = ocerz_self_path();
    if (!self || !a[1]) {
        ret_err(cpu, EINVAL);
        return OCERZ_STEP_OK;
    }
    char *hargv[260];
    int n = 0;
    hargv[n++] = (char *)(uintptr_t)self;
    hargv[n++] = (char *)ocerz_g2h(a[1]);
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
#define OCERZ_PTHREAD_COOKIE 0x7ff8436bd750ull

static int sys_workq_kernreturn(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    uint64_t op = a[0];
    if (op == 0x4) {
        cpu->terminated = 1;
        ret_ok(cpu, 0);
        return OCERZ_STEP_OK;
    }
    if (op == 0x20) {
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

/* WQ_FLAG_THREAD_* word handed to _pthread_wqthread. Decoded directly from the
 * cache's _pthread_wqthread: bit14(0x4000)=QoS-class-in-low-byte, bit18(0x40000)
 * =NEWSPI, bit21(0x200000)=OUTSIDEQOS, bit19(0x80000)=WORKLOOP worker (the
 * branch that calls _dispatch_workloop_worker_thread draining the kqueue events
 * handed in via RCX/R9). The plain REQTHREADS worker omits bit19 and so drains
 * only the userspace root concurrent queue. */
#define OCERZ_WQ_FLAG_BASE   (0x40000ull | 0x200000ull | 0x4000ull)
#define OCERZ_WQ_FLAG_WORKLOOP 0x80000ull

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
    t.gpr[OCERZ_R8] = OCERZ_WQ_FLAG_BASE | OCERZ_WQ_FLAG_WORKLOOP | (uint64_t)qos_idx;
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

static void ocerz_hostwq_bridge(uint64_t extra_r8, const void *hev, int nev)
{
    OcerzVM *vm = g_hostwq_vm;
    if (!vm || nev <= 0 || !hev)
        return;
    if (nev > 16)
        nev = 16;
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
    for (int i = 0; i < nev; i++)
        memcpy(ocerz_g2h(evbuf + (uint64_t)i * OCERZ_KEVENT_QOS_S),
               (const char *)hev + (size_t)i * OCERZ_KEVENT_QOS_S, OCERZ_KEVENT_QOS_S);
    mach_port_t kp = mach_thread_self();
    ocerz_st(pth, 8, pth ^ cookie);
    ocerz_st(pth + 0xe0, 8, pth);
    ocerz_st(pth + 0xf8, 4, (uint64_t)(uint32_t)kp);
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
    t.gpr[OCERZ_R8] = OCERZ_WQ_FLAG_BASE | extra_r8 | 4u;
    t.gpr[OCERZ_R9] = (uint64_t)nev;
    t.gs_base = pth + 0xe0;
    ocerz_vm_run_cpu(vm, &t);
    mach_port_deallocate(mach_task_self(), kp);
}

static void ocerz_hostwq_queue_cb(ocerz_pthread_priority_t pri)
{
    (void)pri;
}

static void ocerz_hostwq_kevent_cb(void **events, int *nevents)
{
    ocerz_hostwq_bridge(0, events ? *events : NULL, nevents ? *nevents : 0);
    if (nevents)
        *nevents = 0;
}

static void ocerz_hostwq_workloop_cb(uint64_t *workloop_id, void **events, int *nevents)
{
    (void)workloop_id;
    ocerz_hostwq_bridge(OCERZ_WQ_FLAG_WORKLOOP, events ? *events : NULL, nevents ? *nevents : 0);
    if (nevents)
        *nevents = 0;
}

static void ocerz_hostwq_register(OcerzVM *vm)
{
    static int done;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    if (!done) {
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
        if (fa[1]) fa[1] = (uint64_t)(uintptr_t)ocerz_g2h(fa[1]);
        if (fa[3]) fa[3] = (uint64_t)(uintptr_t)ocerz_g2h(fa[3]);
        if (fa[5]) fa[5] = (uint64_t)(uintptr_t)ocerz_g2h(fa[5]);
        if (fa[6]) fa[6] = (uint64_t)(uintptr_t)ocerz_g2h(fa[6]);
        uint64_t ret2 = 0;
        int err = 0;
        uint64_t r = ocerz_host_syscall(375, fa, &ret2, &err);
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

static int sys_bsdthread_create(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    uint64_t func = a[0], funarg = a[1], stack = a[2], pth = a[3], flags = a[4];
    (void)func;
    (void)funarg;
    struct ocerz_worker *w = (struct ocerz_worker *)calloc(1, sizeof *w);
    if (!w) {
        ret_err(cpu, OCERZ_ENOMEM_V);
        return OCERZ_STEP_OK;
    }
    w->vm = vm;
    w->cpu = *cpu;
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
    if (signo > 0 && signo < OCERZ_NSIG) {
        uint64_t h = guest_sigact[signo].handler;
        if (h > 1) {
            OcerzCPU saved = *cpu;
            uint64_t args[1] = { signo };
            ocerz_vm_call(vm, h, args, 1, saved.gpr[OCERZ_RSP]);
            if (vm->exited)
                return OCERZ_STEP_OK;
            *cpu = saved;
        }
    }
    ret_ok(cpu, 0);
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
 * rax,rbx,rcx,rdx,rdi,rsi,rbp,rsp,r8..r15,rip@144,rflags@152). gs_base/fs_base are
 * NOT carried through the frame, so the handler keeps the faulting thread's TLS. */
#define OCERZ_MCTX_SIZE 1032u
#define OCERZ_UCTX_SIZE 768u
#define OCERZ_SIGINFO_SIZE 104u

int ocerz_signal_deliver(OcerzCPU *cpu, int sig, uint64_t fault_addr, int si_code,
                         uint32_t err)
{
    if (sig <= 0 || sig >= OCERZ_NSIG)
        return 0;
    GuestSigact *sa = &guest_sigact[sig];
    if (sa->handler <= 1 || sa->tramp == 0)
        return 0;
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

    int trapno = (sig == SIGSEGV || sig == SIGBUS) ? 14 : (sig == SIGILL ? 6 : 0);
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
    [202] = { "sysctl",      6, 0x1d, 0, NULL },
    [216] = { "open_dprotected_np", 5, 0x01, 0, NULL },
    [220] = { "getattrlist", 5, 0x07, 0, NULL },
    [228] = { "fgetattrlist", 5, 0x06, 0, NULL },
    [229] = { "fsetattrlist", 5, 0x06, 0, NULL },
    [230] = { "poll",        3, 0x01, 0, NULL },
    [234] = { "getxattr",    6, 0x07, 0, NULL },
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
    [274] = { "sysctlbyname",6, 0x1d, 0, NULL },
    [381] = { "__mac_syscall", 3, 0x05, 0, NULL },
    [483] = { "csrctl", 3, 0x02, 0, NULL },
    [524] = { "setattrlistat", 6, 0x0e, 0, NULL },
    [521] = { "abort_with_payload", 6, 0x04, 0, sys_abort_payload },
    [283] = { "fchmod_extended", 5, 0x10, 0, NULL },
    [286] = { "gettid",      2, 0x03, 0, NULL },
    [294] = { "shared_region_check_np", 1, 0x01, 0, sys_shared_region_check_np },
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
    [334] = { "__semwait_signal", 6, 0x00, 0, sys_workq_stub },
    [374] = { "kevent_qos",  8, 0x00, 0, sys_workq_stub },
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
    [423] = { "__semwait_signal_nocancel", 6, 0x00, 0, sys_workq_stub },
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
    uint64_t r = ocerz_host_syscall(num, a, &ret2, &err);
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
    case 12: return "_kernelrpc_mach_vm_deallocate_trap";
    case 14: return "_kernelrpc_mach_vm_protect_trap";
    case 15: return "_kernelrpc_mach_vm_map_trap";
    case 16: return "_kernelrpc_mach_port_allocate_trap";
    case 18: return "_kernelrpc_mach_port_deallocate_trap";
    case 19: return "_kernelrpc_mach_port_mod_refs_trap";
    case 26: return "mach_reply_port";
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
    uint64_t size = (uint64_t)rsize - (haddr - (uint64_t)raddr);
    uint64_t gaddr = ocerz_map_donate(size);
    if (gaddr == 0)
        return;
    mach_vm_address_t dst = gaddr;
    vm_prot_t curp = 0, maxp = 0;
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
    case 70: {
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 31: {
        if (a[0] != 0)
            a[0] = (uint64_t)(uintptr_t)ocerz_g2h(a[0]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
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
        if (a[0] != 0)
            a[0] = (uint64_t)(uintptr_t)ocerz_g2h(a[0]);
        a[6] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        a[7] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 16, 8);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        if (vm->strace && reply_buf != 0)
            fprintf(stderr, "ocerz: mach_msg2 id=%u reply_id=%u bits=%#x size=%#x\n",
                    msgh_id, (uint32_t)ocerz_ld(reply_buf + 0x14, 4),
                    (uint32_t)ocerz_ld(reply_buf, 4),
                    (uint32_t)ocerz_ld(reply_buf + 4, 4));
        static int migtrace = -1;
        if (migtrace < 0) migtrace = getenv("OCERZ_MIGTRACE") != NULL ? 1 : 0;
        if (reply_buf != 0 && migtrace &&
            ((uint32_t)ocerz_ld(reply_buf, 4) & 0x80000000u)) {
            uint32_t dcnt = (uint32_t)ocerz_ld(reply_buf + 0x18, 4);
            fprintf(stderr,
                    "ocerz: MIGCOMPLEX reply_id=%u bits=%#x size=%#x desc_count=%u d0.addr=%#llx d0.type=%#x icount=%#llx\n",
                    (uint32_t)ocerz_ld(reply_buf + 0x14, 4),
                    (uint32_t)ocerz_ld(reply_buf, 4),
                    (uint32_t)ocerz_ld(reply_buf + 4, 4), dcnt,
                    (unsigned long long)ocerz_ld(reply_buf + 0x1c, 8),
                    (uint32_t)ocerz_ld(reply_buf + 0x1c + 0xc, 4),
                    (unsigned long long)vm->insn_count);
        }
        static int machleak = -1;
        if (machleak < 0) machleak = getenv("OCERZ_MACHLEAK") != NULL ? 1 : 0;
        if (reply_buf != 0 && machleak) {
            for (uint64_t off = 0x18; off <= 0x80; off += 8) {
                uint64_t v = ocerz_ld(reply_buf + off, 8);
                if (v >= 0x140000000ull && v < OCERZ_LOW_LIMIT &&
                    ocerz_addr_committed(v) == 0)
                    fprintf(stderr,
                            "ocerz: MACHLEAK reply_id=%u off=%#llx host_val=%#llx icount=%#llx\n",
                            (uint32_t)ocerz_ld(reply_buf + 0x14, 4),
                            (unsigned long long)off, (unsigned long long)v,
                            (unsigned long long)vm->insn_count);
            }
        }
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
        if (a[0] != 0)
            a[0] = (uint64_t)(uintptr_t)ocerz_g2h(a[0]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 90:   /* mach_wait_until_trap(deadline) */
    case 91:   /* mk_timer_create_trap() */
    case 92:   /* mk_timer_destroy_trap(name) */
    case 93:   /* mk_timer_arm_trap(name, expire_time) */
    case 95: { /* mk_timer_arm_leeway_trap(name, flags, expire_time, leeway) */
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

static int dispatch_machdep(OcerzVM *vm, OcerzCPU *cpu, int num)
{
    if (num == 3) {
        cpu->gs_base = cpu->gpr[OCERZ_RDI];
        ret_ok(cpu, 0x60);
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

int ocerz_handle_syscall(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t rax = cpu->gpr[OCERZ_RAX];
    int class = (int)((rax >> 24) & 0xff);
    int num = (int)(rax & 0xffffff);

    switch (class) {
    case 1:
        return dispatch_mach(vm, cpu, num);
    case 2:
        return dispatch_bsd(vm, cpu, num);
    case 3:
        return dispatch_machdep(vm, cpu, num);
    default:
        OCERZ_FATAL("unknown syscall class=%d num=%d (rax=%#llx)\n",
                    class, num, (unsigned long long)rax);
        return OCERZ_STEP_FATAL;
    }
}
