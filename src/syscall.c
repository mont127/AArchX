/* The guest-to-host syscall boundary: emulated x86_64 syscalls onto the arm64 kernel. */
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

        gaddr = 0;
        if (addr != 0 && !getenv("OCERZ_NO_MMAP_HINT") &&
            ocerz_map_hint(addr, len, prot) == OCERZ_OK)
            gaddr = addr & ~0x3fffull;
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

    uint64_t feat = 0x4000005f;
    if (getenv("OCERZ_HOSTWQ"))
        feat |= 0x80ull;
    ret_ok(cpu, feat);
    return OCERZ_STEP_OK;
}

static int sys_workq_stub(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    (void)a;
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

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
    { "hw.cputype",             0, NULL,     7,           {0}, 0, 0 },
    { "hw.cpusubtype",          0, NULL,     4,           {0}, 0, 0 },
    { "hw.cpufamily",           0, NULL,     0x573B5EECu, {0}, 0, 0 },
    { "sysctl.proc_translated", 0, NULL,     1,           {0}, 0, 0 },
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

static int sys_sysctl(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    (void)vm;
    uint64_t nlen = a[1];
    static int sclog2 = -1;
    if (sclog2 < 0) sclog2 = getenv("OCERZ_SYSCTLLOG") ? 1 : 0;
    if (a[4] == 0 && nlen >= 2 && nlen <= 8) {
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

static uint64_t ocerz_kev_stride(void)
{
    static int s40 = -1;
    if (s40 < 0)
        s40 = getenv("OCERZ_STRIDE40") != NULL ? 1 : 0;
    return s40 ? (uint64_t)0x40 : (uint64_t)0x48;
}
#define OCERZ_KEVENT_QOS_S (ocerz_kev_stride())

#define OCERZ_WQ_KEVENT_LIST_LEN 16

#define OCERZ_KEVENT_QOS_REARM ((uint64_t)0x48)

static int sys_kevent_id(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8]);
static void ocerz_hostwq_register(OcerzVM *vm);
static void ocerz_kev_timer_to_host(void *hev, int nev);

#define OCERZ_EVFILT_TIMER (-7)
#define OCERZ_NOTE_LEEWAY   0x00000010u
#define OCERZ_NOTE_MACHTIME 0x00000100u

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

static __thread void **g_hostwq_tl_events;
static __thread int   *g_hostwq_tl_nevents;
static __thread int    g_hostwq_tl_evcap;

#define OCERZ_THREAD_START 0x7ff802e6f820ull

struct ocerz_worker {
    OcerzVM *vm;
    OcerzCPU cpu;
};

static volatile int g_wq_running;

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

#define OCERZ_PTHREAD_COOKIE 0x7ff8436bd690ull

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

        static int rearm40 = -1;
        if (rearm40 < 0)
            rearm40 = getenv("OCERZ_REARM40") != NULL;

        const uint64_t rstride = rearm40 ? (uint64_t)0x40 : OCERZ_KEVENT_QOS_REARM;

        void **evp = g_hostwq_tl_events;
        int   *nvp = g_hostwq_tl_nevents;
        if (evp && nvp) {
            int n = (int)(int64_t)a[2];

            if (n < 0) n = 0;

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
                            (unsigned long long)(wl ? ocerz_ld(wl + 0x38, 8) : 0),
                            i >= g_hostwq_tl_evcap ? "<<DROPPED" : "");
                }
                fprintf(stderr, "\n");
            }

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
                ocerz_kev_timer_to_host(hbuf, n);
            } else {
                n = 0;
            }
            *nvp = n;
            g_hostwq_tl_events = NULL;
            g_hostwq_tl_nevents = NULL;
        }
        cpu->terminated = 1;
        ret_ok(cpu, 0);
        return OCERZ_STEP_OK;
    }
    if (op == 0x20) {

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

#define OCERZ_WQ_FLAG_BASE   (0x40000ull | 0x200000ull | 0x4000ull)
#define OCERZ_WQ_FLAG_WORKLOOP 0x400000ull
#define OCERZ_WQ_FLAG_KEVENT   0x80000ull

#define OCERZ_WQ_FLAG_REUSE    0x20000ull
#define OCERZ_WQ_FLAG_EVENT_MANAGER 0x100000ull

#define OCERZ_PTHREAD_PRIORITY_EVENT_MANAGER 0x02000000u
#define OCERZ_PTHREAD_TSD_SLOT_QOS ((pthread_key_t)4)

static int ocerz_hostwq_is_manager(void)
{
    uintptr_t pri = (uintptr_t)pthread_getspecific(OCERZ_PTHREAD_TSD_SLOT_QOS);
    return (pri & OCERZ_PTHREAD_PRIORITY_EVENT_MANAGER) != 0;
}

#define OCERZ_PTHREAD_WORKLOOP_SLOT 0x7ff8436bd638ull

#define OCERZ_EVFILT_WORKLOOP (-17)

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
    ocerz_st(evbuf - 8, 8, workloop_id);
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

typedef unsigned long ocerz_pthread_priority_t;
extern int _pthread_workqueue_init_with_workloop(
    void (*queue_func)(ocerz_pthread_priority_t),
    void (*kevent_func)(void **events, int *nevents),
    void (*workloop_func)(uint64_t *workloop_id, void **events, int *nevents),
    int offset, int flags);

static OcerzVM *g_hostwq_vm;

static __thread uint64_t g_hostwq_tl_region;

static __thread int g_hostwq_tl_fatal;

static int ocerz_no_stackbounds(void)
{
    static int v = -1;
    if (v < 0) v = getenv("OCERZ_NO_STACKBOUNDS") ? 1 : 0;
    return v;
}

static uint64_t ocerz_bridge_mach_msg(uint64_t hbuf, uint64_t sz)
{
    if (!hbuf || !sz || sz > 0x100000)
        return 0;

    uint64_t total = sz;
    if (!getenv("OCERZ_NO_AUXTAIL")) {

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
    if (bits & 0x80000000u) {
        uint32_t dcnt;
        memcpy(&dcnt, g + 0x18, 4);
        uint64_t off = 0x1c;
        for (uint32_t d = 0; d < dcnt && off + 12 <= sz; d++) {
            uint8_t type = g[off + 11];
            if (type == 0) { off += 12; continue; }
            if (type == 4) { off += 16; continue; }
            if (type == 1 || type == 2 || type == 3) {
                if (off + 16 > sz)
                    break;
                uint64_t ool_addr;
                uint32_t ool_n;
                memcpy(&ool_addr, g + off, 8);
                memcpy(&ool_n, g + off + 12, 4);

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
                        memcpy(g + off, &ogb, 8);
                        mm_ool++;
                    } else {

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
            break;
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

    return gbuf;
}

static int ocerz_mach_err_interesting(uint64_t r)
{
    if ((r & 0xfffff000ull) == 0x10000000ull)
        return 1;
    if ((r & 0xfffff000ull) == 0x10004000ull)
        return r != 0x10004003ull && r != 0x10004004ull;
    return 0;
}

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

static void ocerz_kev_timer_to_host(void *hev, int nev)
{
    if (!hev || nev <= 0)
        return;
    for (int i = 0; i < nev; i++) {
        unsigned char *e = (unsigned char *)hev + (size_t)i * OCERZ_KEVENT_QOS_S;
        int16_t filt;
        uint32_t fflags;
        memcpy(&filt, e + 0x08, 2);
        if (filt != OCERZ_EVFILT_TIMER)
            continue;
        memcpy(&fflags, e + 0x18, 4);
        if (!(fflags & OCERZ_NOTE_MACHTIME))
            continue;
        uint64_t v;
        memcpy(&v, e + 0x20, 8);
        v = ocerz_guest_ns_to_host_ticks(v);
        memcpy(e + 0x20, &v, 8);
        if (fflags & OCERZ_NOTE_LEEWAY) {
            memcpy(&v, e + 0x30, 8);
            v = ocerz_guest_ns_to_host_ticks(v);
            memcpy(e + 0x30, &v, 8);
        }
    }
}

static void ocerz_log_mach_send_err(int trap, uint64_t kr, const uint64_t *a, uint64_t gmsg)
{
    static int slog = -1;
    if (slog < 0) slog = getenv("OCERZ_MACHMSG") ? 1 : 0;
    if (!slog) return;

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

static int g_sendxlate_off = -1;

static int ocerz_send_xlate_descriptors(uint64_t gmsg, uint32_t send_size,
                                        struct ocerz_ool_save *saved, int max_saved)
{
    if (g_sendxlate_off < 0)
        g_sendxlate_off = getenv("OCERZ_NO_SENDXLATE") ? 1 : 0;
    if (g_sendxlate_off || gmsg == 0)
        return 0;
    uint32_t bits = (uint32_t)ocerz_ld(gmsg, 4);
    if (!(bits & 0x80000000u))
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
        if (type == 0) { off += 12; continue; }
        if (type == 1 || type == 2 || type == 3) {
            if (send_size && off + 16 > send_size)
                break;
            uint64_t ga = ocerz_ld(gmsg + off, 8);
            if (ga != 0) {
                uint64_t ha = (uint64_t)(uintptr_t)ocerz_g2h(ga);
                if (ha != ga && n < max_saved) {
                    saved[n].off = off;
                    saved[n].orig = ga;
                    n++;
                    ocerz_st(gmsg + off, 8, ha);
                }
            }
            off += 16;
            continue;
        }
        if (type == 4) { off += 16; continue; }
        break;
    }
    return n;
}

static void ocerz_send_restore_descriptors(uint64_t gmsg, const struct ocerz_ool_save *saved, int n)
{
    for (int i = 0; i < n; i++) {
        uint64_t ha = (uint64_t)(uintptr_t)ocerz_g2h(saved[i].orig);
        if (ocerz_ld(gmsg + saved[i].off, 8) == ha)
            ocerz_st(gmsg + saved[i].off, 8, saved[i].orig);
    }
}

static int ocerz_send_xlate_vector(uint64_t gvec, uint32_t count, int sending, int receiving,
                                   struct ocerz_ool_save *saved, int max_saved)
{
    if (g_sendxlate_off < 0)
        g_sendxlate_off = getenv("OCERZ_NO_SENDXLATE") ? 1 : 0;
    if (g_sendxlate_off || gvec == 0 || count == 0 || count > 64)
        return 0;
    static int vlog = -1;
    if (vlog < 0) vlog = getenv("OCERZ_MACHMSG") ? 1 : 0;
    uint64_t seg0 = ocerz_ld(gvec, 8);
    uint32_t seg0_size = (uint32_t)ocerz_ld(gvec + 0x10, 4);
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t e = gvec + (uint64_t)i * 24;
        if (vlog && i < 4)
            fprintf(stderr, "ocerz: VEC[%u] data=%#llx rcv_addr=%#llx ssize=%#x rsize=%#x\n", i,
                    (unsigned long long)ocerz_ld(e, 8), (unsigned long long)ocerz_ld(e + 8, 8),
                    (uint32_t)ocerz_ld(e + 0x10, 4), (uint32_t)ocerz_ld(e + 0x14, 4));

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

    uint64_t region = g_hostwq_tl_region;
    if (region == 0) {
        region = ocerz_map_anywhere(0x200000, PROT_READ | PROT_WRITE);
        if (region == 0)
            return;
        g_hostwq_tl_region = region;
    }
    uint64_t pth = region + 0x1f0000;
    uint64_t evbuf = pth + 0x8000;
    ocerz_st(evbuf - 8, 8, workloop_id);

    if (nev > 1 && OCERZ_ENV_ON("OCERZ_NEVLOG")) {
        fprintf(stderr, "ocerz: HOSTWQ-NEV nev=%d filters=", nev);
        for (int i = 0; i < nev; i++)
            fprintf(stderr, "%s%d", i ? "," : "",
                    (int)(int16_t)ocerz_ld(evbuf + (uint64_t)i * OCERZ_KEVENT_QOS_S + 8, 2));
        fprintf(stderr, "\n");
    }

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

    if (!getenv("OCERZ_NO_MACHBRIDGE"))
        for (int i = 0; i < nev; i++) {
            uint64_t dst = evbuf + (uint64_t)i * OCERZ_KEVENT_QOS_S;
            if ((int16_t)ocerz_ld(dst + 0x08, 2) != -8) continue;
            uint64_t hbuf = ocerz_ld(dst + 0x28, 8);
            uint64_t sz = ocerz_ld(dst + 0x30, 8);
            uint64_t gbuf = ocerz_bridge_mach_msg(hbuf, sz);
            if (gbuf)
                ocerz_st(dst + 0x28, 8, gbuf);
            else if (hbuf)
                ocerz_st(dst + 0x28, 8, 0);

            if (gbuf && getenv("OCERZ_WSIG"))
                fprintf(stderr, "ocerz: MACHBRIDGE ev[%d] hbuf=%#llx sz=%#llx -> gbuf=%#llx%s\n",
                        i, (unsigned long long)hbuf, (unsigned long long)sz,
                        (unsigned long long)gbuf,
                        (*(volatile uint32_t *)(uintptr_t)hbuf & 0x80000000u) ? " COMPLEX" : "");
        }
    mach_port_t kp = mach_thread_self();

    if (g_hostwq_tl_fatal || !getenv("OCERZ_NO_DDIRESET")) {
        ocerz_st(pth + 0x1c8, 8, 0);
        ocerz_st(pth + 0x1b8, 8, 0);
        g_hostwq_tl_fatal = 0;
    }
    int registered = ocerz_ld(pth + 0xd8, 8) != 0;
    if (!registered) {
        ocerz_st(pth, 8, pth ^ cookie);
        ocerz_st(pth + 0xe0, 8, pth);
    }
    ocerz_st(pth + 0xf8, 4, (uint64_t)(uint32_t)kp);

    if (!ocerz_no_stackbounds()) {
        ocerz_st(pth + 0xb0, 8, pth - 0x100);
        ocerz_st(pth + 0xb8, 8, region);
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
    uint64_t mgr = ocerz_hostwq_is_manager() && !getenv("OCERZ_NO_MGRFLAG")
                 ? OCERZ_WQ_FLAG_EVENT_MANAGER : 0;
    t.gpr[OCERZ_R8] = OCERZ_WQ_FLAG_BASE | extra_r8 | mgr | (mgr ? 0 : 4u)
                    | (registered ? OCERZ_WQ_FLAG_REUSE : 0);
    if (mgr && getenv("OCERZ_ULOCKLOG"))
        fprintf(stderr, "ocerz: WQ-MANAGER delivery nev=%d\n", nev);
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

    if (wrc == 125) {

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

    if (g_hostwq_tl_fatal || !getenv("OCERZ_NO_DDIRESET")) {
        ocerz_st(pth + 0x1c8, 8, 0);
        ocerz_st(pth + 0x1b8, 8, 0);
        g_hostwq_tl_fatal = 0;
    }
    mach_port_t kp = mach_thread_self();
    ocerz_st(pth, 8, pth ^ cookie);
    ocerz_st(pth + 0xe0, 8, pth);
    ocerz_st(pth + 0xf8, 4, (uint64_t)(uint32_t)kp);

    if (!ocerz_no_stackbounds()) {
        ocerz_st(pth + 0xb0, 8, pth - 0x100);
        ocerz_st(pth + 0xb8, 8, region);
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
        g_hostwq_tl_fatal = 1;
    mach_port_deallocate(mach_task_self(), kp);
}

static void ocerz_hostwq_kevent_cb(void **events, int *nevents)
{

    g_hostwq_tl_events  = events;
    g_hostwq_tl_nevents = nevents;

    g_hostwq_tl_evcap   = OCERZ_WQ_KEVENT_LIST_LEN;
    ocerz_hostwq_bridge(OCERZ_WQ_FLAG_KEVENT, 0,
                        events ? *events : NULL, nevents ? *nevents : 0);

    if (g_hostwq_tl_nevents) {
        if (nevents) *nevents = 0;
        g_hostwq_tl_events = NULL;
        g_hostwq_tl_nevents = NULL;
    }
    if (getenv("OCERZ_REARMLOG") && nevents && *nevents > 0 && events && *events) {
        for (int i = 0; i < *nevents; i++) {
            const unsigned char *e = (const unsigned char *)*events
                                   + (size_t)i * OCERZ_KEVENT_QOS_S;
            uint64_t ident, udata; int16_t filt; uint16_t fl;
            uint32_t fflags; int64_t data;
            memcpy(&ident, e + 0x00, 8); memcpy(&filt, e + 0x08, 2);
            memcpy(&fl, e + 0x0a, 2);    memcpy(&udata, e + 0x10, 8);
            memcpy(&fflags, e + 0x18, 4); memcpy(&data, e + 0x20, 8);
            fprintf(stderr,
                    "ocerz: REARM[%d] filt=%d ident=%#llx flags=%#x fflags=%#x data=%lld udata=%#llx\n",
                    i, filt, (unsigned long long)ident, fl, fflags,
                    (long long)data, (unsigned long long)udata);
        }
    }
}

static void ocerz_hostwq_workloop_cb(uint64_t *workloop_id, void **events, int *nevents)
{

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

    g_hostwq_tl_evcap   = OCERZ_WQ_KEVENT_LIST_LEN;

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

static int sys_kevent_id(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{
    if (getenv("OCERZ_HOSTWQ")) {
        ocerz_hostwq_register(vm);

        uint64_t fa[8];
        memcpy(fa, a, sizeof fa);
        fa[6] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        fa[7] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 16, 8);
        if (getenv("OCERZ_WSIG") && a[1] && (int)a[2] > 0) {
            int nc = (int)a[2]; if (nc > 8) nc = 8;
            for (int i = 0; i < nc; i++) {
                uint64_t k = a[1] + (uint64_t)i * OCERZ_KEVENT_QOS_S;
                int16_t filt = (int16_t)ocerz_ld(k + 0x08, 2);
                if (filt != -8) continue;
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

        if (!err && (int64_t)r > 0 && a[3]) {
            int got = (int)r;
            if (got > 64) got = 64;
            for (int i = 0; i < got; i++) {
                uint64_t ev = a[3] + (uint64_t)i * OCERZ_KEVENT_QOS_S;
                if ((int16_t)ocerz_ld(ev + 0x08, 2) != -8) continue;
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

static int sys_kevent_qos(OcerzVM *vm, OcerzCPU *cpu, uint64_t a[8])
{

    uint64_t kq_flags = ocerz_ld(cpu->gpr[OCERZ_RSP] + 16, 8);
    if (getenv("OCERZ_MGRPROBE") && (kq_flags & 0x20ull)) {

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

    static int fwd_workq = -1;
    if (fwd_workq < 0) fwd_workq = getenv("OCERZ_NO_FWD_WORKQ") ? 0 : 1;
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
                if ((int16_t)ocerz_ld(ev + 0x08, 2) != -8) continue;
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

    if (getenv("OCERZ_PTKILL")) {
        uint64_t selfport = ocerz_host_mach_trap(27, a);
        fprintf(stderr, "ocerz: PTKILL[%d] target=%#llx signo=%llu self=%#llx cross=%d handler=%#llx\n",
                (int)getpid(), (unsigned long long)a[0], (unsigned long long)signo,
                (unsigned long long)selfport, (a[0] && a[0] != selfport) ? 1 : 0,
                (unsigned long long)(signo < OCERZ_NSIG ? guest_sigact[signo].handler : 0));
    }
    (void)vm; (void)signo;

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
        if (how == 1)
            cpu->sig_mask |= v;
        else if (how == 2)
            cpu->sig_mask &= ~(uint64_t)v;
        else
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
        if (flags & 0x0004u) {
            cpu->sig_altstack_sp = 0;
            cpu->sig_altstack_size = 0;
        } else {
            cpu->sig_altstack_sp = ocerz_ld(ss + 0, 8);
            cpu->sig_altstack_size = ocerz_ld(ss + 8, 8);

            if (cpu->sig_altstack_sp && cpu->sig_altstack_size)
                ocerz_protect(cpu->sig_altstack_sp, cpu->sig_altstack_size,
                              3 );
        }
    }
    ret_ok(cpu, 0);
    return OCERZ_STEP_OK;
}

#define OCERZ_MCTX_SIZE 1032u
#define OCERZ_UCTX_SIZE 768u
#define OCERZ_SIGINFO_SIZE 104u

__thread int g_ocerz_deliver_src;

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

    if ((num == 515 || num == 516 || num == 544) && getenv("OCERZ_ULOCKLOG")) {
        uint64_t myport = cpu->gs_base ? ocerz_ld(cpu->gs_base + 0x18, 4) : 0;
        fprintf(stderr, "ocerz: ULOCK[%d] cpu#%u %s op=%#llx addr=%#llx val=%#llx myport=%#llx caller=%#llx\n",
                (int)getpid(), cpu->cpu_number, e->name,
                (unsigned long long)orig[0], (unsigned long long)orig[1],
                (unsigned long long)orig[2], (unsigned long long)myport,
                (unsigned long long)cpu->rip);
    }

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
        if (na != rend)
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

static void ocerz_reply_relocate_ool(uint64_t reply_buf, uint32_t recv_size)
{
    uint32_t bits = (uint32_t)ocerz_ld(reply_buf, 4);
    if (!(bits & 0x80000000u))
        return;
    uint32_t sz = (uint32_t)ocerz_ld(reply_buf + 4, 4);
    if (recv_size && recv_size < sz)
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
        if (type == 0) { off += 12; continue; }
        if (type == 4) { off += 16; continue; }
        if (type == 1 || type == 2 || type == 3) {
            if (off + 16 > sz)
                break;
            uint64_t ool_addr = ocerz_ld(reply_buf + off, 8);
            uint32_t ool_n = (uint32_t)ocerz_ld(reply_buf + off + 12, 4);

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
                    ocerz_st(reply_buf + off, 8, gb);
                    if (ooltrace)
                        fprintf(stderr, "ocerz: OOLTRACE-COPY id=%u type=%u gb=%#llx..%#llx bytes=%#llx\n",
                                reply_id, type, (unsigned long long)gb,
                                (unsigned long long)(gb + bytes), (unsigned long long)bytes);
                } else {
                    ocerz_st(reply_buf + off, 8, 0);
                    ocerz_st(reply_buf + off + 12, 4, 0);
                }
            }
            off += 16;
            continue;
        }
        if (ooltrace)
            fprintf(stderr, "ocerz: OOLTRACE-BREAK id=%u off=%#llx type=%u (remaining desc left raw)\n",
                    reply_id, (unsigned long long)off, (uint8_t)ocerz_ld(reply_buf + off + 11, 1));
        break;
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
    case 11: {

        if (a[3] != 0)
            ocerz_st(a[3], 4, 0 );
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
    case 61:
    case 70: {
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 31: {
        uint64_t gmsg31 = a[0];

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
    case 44:
    case 45: {
        if (a[2] != 0)
            a[2] = (uint64_t)(uintptr_t)ocerz_g2h(a[2]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 46: {
        if (a[1] != 0)
            a[1] = (uint64_t)(uintptr_t)ocerz_g2h(a[1]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 47: {
        uint32_t msgh_id = (uint32_t)(a[4] >> 32);
        uint64_t reply_buf = a[0];

        a[6] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 8, 8);
        a[7] = ocerz_ld(cpu->gpr[OCERZ_RSP] + 16, 8);

        struct ocerz_ool_save sv47[64];
        int nsv47 = 0;
        if (reply_buf != 0 && ocerz_low_base) {
            if (a[1] & 0x100000000ull) {

                uint32_t sc = (a[1] & 0x1) ? (uint32_t)(a[2] >> 32) : 0;
                uint32_t rc = (a[1] & 0x2) ? (uint32_t)(a[6] & 0xffffffffu) : 0;
                nsv47 = ocerz_send_xlate_vector(reply_buf, sc > rc ? sc : rc,
                                                (a[1] & 0x1) != 0, (a[1] & 0x2) != 0, sv47, 64);
            } else
                nsv47 = ocerz_send_xlate_descriptors(reply_buf, (uint32_t)(a[2] >> 32), sv47, 64);
        }
        if (a[0] != 0)
            a[0] = (uint64_t)(uintptr_t)ocerz_g2h(a[0]);

        {
            static int g_lstimeout = -1;
            if (g_lstimeout < 0) g_lstimeout = getenv("OCERZ_LSTIMEOUT") ? 1 : 0;
            if (g_lstimeout && reply_buf != 0 && (a[1] & 0x1) && (a[1] & 0x2) && !(a[1] & 0x100)) {
                uint32_t mid = (uint32_t)(a[4] >> 32);
                if (mid >= 10000 && mid < 10100) {
                    a[1] |= 0x100;
                    a[7] = 1500;
                    if (getenv("OCERZ_MACHMSG"))
                        fprintf(stderr, "ocerz: LSTIMEOUT forcing 1500ms RCV timeout on id=%u\n", mid);
                }
            }
        }
        if (getenv("OCERZ_MSGTIMEOUT"))
            fprintf(stderr, "ocerz: MSG2 opts=%#llx rcv_timeout_arg=%#llx a6=%#llx rsp=%#llx\n",
                    (unsigned long long)a[1], (unsigned long long)a[7],
                    (unsigned long long)a[6], (unsigned long long)cpu->gpr[OCERZ_RSP]);

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

        if (reply_buf != 0 && ocerz_mach_err_interesting(r47))
            ocerz_log_mach_send_err(47, r47, a, reply_buf);
        static int machleak = -1;
        if (machleak < 0) machleak = getenv("OCERZ_MACHLEAK") != NULL ? 1 : 0;
        if (reply_buf != 0 && machleak) {

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

        if (reply_buf != 0 && (a[1] & 0x2) && !(a[1] & 0x100000000ull) && r47 == 0)
            ocerz_reply_relocate_ool(reply_buf, (uint32_t)a[6]);
        if (reply_buf != 0) {
            uint32_t rid = (uint32_t)ocerz_ld(reply_buf + 0x14, 4);
            if ((rid == 4900 || rid == 4911 || rid == 4913) &&
                (uint32_t)ocerz_ld(reply_buf + 0x20, 4) == OCERZ_MACH_KERN_SUCCESS)
                mig_vm_reply_relocate(vm, reply_buf);

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
    case 76: {
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

        if (a[0] != 0) {
            ocerz_st(a[0] + 0, 4, 1);
            ocerz_st(a[0] + 4, 4, 1);
        }
        mach_ret(cpu, 0);
        break;
    }
    case 90:
    case 91:
    case 92:
    case 93:
    case 95: {

        uint64_t d0 = a[0], d1 = a[1], d2 = a[2], d3 = a[3];
        if (num == 90)
            a[0] = ocerz_guest_ns_to_host_ticks(a[0]);
        else if (num == 93)
            a[1] = ocerz_guest_ns_to_host_ticks(a[1]);
        else if (num == 95) {
            a[2] = ocerz_guest_ns_to_host_ticks(a[2]);
            a[3] = ocerz_guest_ns_to_host_ticks(a[3]);
        }
        (void)d0; (void)d1; (void)d2; (void)d3;
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 94: {
        if (a[1] != 0)
            a[1] = (uint64_t)(uintptr_t)ocerz_g2h(a[1]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 41: {
        if (a[2] != 0)
            a[2] = (uint64_t)(uintptr_t)ocerz_g2h(a[2]);
        mach_ret(cpu, ocerz_host_mach_trap(num, a));
        break;
    }
    case 100: {
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

#define OCERZ_LDT_MAX 512
struct ocerz_ldt_entry {
    uint64_t base;
    uint32_t limit;
    uint8_t  access;
    uint8_t  big;
    uint8_t  gran;
    uint8_t  present;
};
static struct ocerz_ldt_entry g_ldt[OCERZ_LDT_MAX];
static int g_ldt_next = 1;
static pthread_mutex_t g_ldt_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t ocerz_ldt_base(uint32_t sel)
{
    uint32_t idx = sel >> 3;
    if (!(sel & 4) || idx == 0 || idx >= OCERZ_LDT_MAX)
        return 0;
    return g_ldt[idx].present ? g_ldt[idx].base : 0;
}

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
    e->big     = (uint8_t)((d >> 54) & 1);
    e->gran    = (uint8_t)((d >> 55) & 1);
    e->present = (uint8_t)((e->access >> 7) & 1);

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

    int32_t start = (int32_t)cpu->gpr[OCERZ_RDI];
    uint64_t descs = cpu->gpr[OCERZ_RSI];
    int32_t count = (int32_t)cpu->gpr[OCERZ_RDX];
    if (count < 0 || count > OCERZ_LDT_MAX || descs == 0) {
        ret_err(cpu, EINVAL);
        return OCERZ_STEP_OK;
    }
    pthread_mutex_lock(&g_ldt_lock);
    if (num == 5) {
        int idx;
        if (start < 0) {
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
        ret_ok(cpu, (uint64_t)(uint32_t)idx);
        return OCERZ_STEP_OK;
    }

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

extern uint32_t ocerz_take_pending_async_sig(void);

int ocerz_handle_syscall(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint64_t rax = cpu->gpr[OCERZ_RAX];
    int class = (int)((rax >> 24) & 0xff);
    int num = (int)(rax & 0xffffff);

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
            if (dt > 20000000ull)
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
