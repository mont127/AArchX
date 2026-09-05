/* The master run loop and crash containment. */
#include "ocerz/vm.h"
#include "ocerz/dyld.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/flags.h"
#include "ocerz/mem.h"
#include "ocerz/cache.h"
#include "ocerz/syscall.h"
#include "ocerz/dyldapi.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <setjmp.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <time.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#define OCERZ_CALL_SENTINEL 0x00000000deadca11ull

static OcerzVM *g_vm;
static __thread OcerzCPU *g_cur_cpu;

#define OCERZ_MAX_CPUS 512
static uint64_t g_image_slide;
static OcerzCPU *g_cpus[OCERZ_MAX_CPUS];
static pthread_t g_cpu_threads[OCERZ_MAX_CPUS];
static int g_cpus_n;
static pthread_mutex_t g_cpus_lock = PTHREAD_MUTEX_INITIALIZER;
/* OCERZ_BTRACE: SIGUSR1 (ripdump) asks the monitor thread to latch and print
 * the block-entry rings.  The handler only sets a flag: printing 64K entries
 * is not async-signal-safe, and the stuck thread is never idle (it spins in a
 * kevent wait loop), so a quiet-based latch cannot see it. */
uint64_t ocerz_exc_trap_rip;      /* OCERZ_EXCLOG: guest _objc_exception_throw entry */
uint64_t ocerz_cxa_throw_rip;    /* OCERZ_EXCLOG: guest ___cxa_throw entry */
static volatile int g_btrace_req;
static int g_btrace_on = -1;
static OcerzCPU *g_fork_surviving_cpu;
static int g_unstick_started;

static void ocerz_cpu_register(OcerzCPU *cpu)
{
    if (g_btrace_on < 0) g_btrace_on = getenv("OCERZ_BTRACE") ? 1 : 0;
    if (!cpu->btrace && g_btrace_on > 0) {
        unsigned n = 1u << 16;                       /* 64K entries = 512 KB */
        cpu->btrace = (uint64_t *)calloc(n, sizeof(uint64_t));
        if (cpu->btrace) {
            cpu->btrace_n = 0;
            __atomic_store_n(&cpu->btrace_mask, n - 1, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_lock(&g_cpus_lock);
    for (int i = 0; i < g_cpus_n; i++)
        if (g_cpus[i] == cpu) {

            static _Atomic unsigned dups;
            if (getenv("OCERZ_CPUREG_LOG"))
                fprintf(stderr, "ocerz: CPUREG dup #%u cpu=%p (would have dangled)\n",
                        ++dups, (void *)cpu);
            pthread_mutex_unlock(&g_cpus_lock);
            return;
        }
    if (g_cpus_n < OCERZ_MAX_CPUS) {
        g_cpus[g_cpus_n] = cpu;
        g_cpu_threads[g_cpus_n] = pthread_self();
        g_cpus_n++;
    }

    pthread_mutex_unlock(&g_cpus_lock);
}

static void ocerz_cpu_unregister(OcerzCPU *cpu)
{
    pthread_mutex_lock(&g_cpus_lock);
    for (int i = 0; i < g_cpus_n; i++) {
        if (g_cpus[i] == cpu) {
            int last = --g_cpus_n;
            g_cpus[i] = g_cpus[last];
            g_cpu_threads[i] = g_cpu_threads[last];
            g_cpus[last] = NULL;
            i--;
        }
    }
    pthread_mutex_unlock(&g_cpus_lock);
}

/* The JIT's RAS is a ring (index = top & (SIZE-1)): stale entries below the
 * top must not survive an invalidation, so purge clears the entries too. */
static void ras_clear(OcerzCPU *cpu)
{
    for (int i = 0; i < OCERZ_RAS_SIZE; i++) {
        __atomic_store_n(&cpu->ras[i].host_entry, (void *)NULL, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->ras[i].guest_rip, (uint64_t)0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&cpu->ras_top, 0, __ATOMIC_RELEASE);
}
void ocerz_vm_purge_jit_ras(OcerzVM *vm)
{
    if (!vm)
        return;
    ras_clear(&vm->cpu);
    if (g_cur_cpu && g_cur_cpu->vm == vm)
        ras_clear(g_cur_cpu);
    pthread_mutex_lock(&g_cpus_lock);
    for (int i = 0; i < g_cpus_n; i++)
        if (g_cpus[i] && g_cpus[i]->vm == vm)
            ras_clear(g_cpus[i]);
    pthread_mutex_unlock(&g_cpus_lock);
}

static __thread volatile uint32_t g_pending_async_mask;

uint32_t ocerz_peek_pending_async_sig(void)
{
    return __atomic_load_n(&g_pending_async_mask, __ATOMIC_SEQ_CST);
}

uint32_t ocerz_take_pending_async_sig(void)
{
    return __atomic_exchange_n(&g_pending_async_mask, 0u, __ATOMIC_SEQ_CST);
}

static __thread sigjmp_buf *g_sig_recover;

#define OCERZ_SIG_MAX_REPEAT 16

extern __thread int ocerz_jit_exec_state;
/* per-thread ring of fault-recovery events (what unwound guest execution
 * mid-flight); dumped by the UD2 diagnostics to correlate aborts like the
 * libplatform os_unfair_lock recursion with a preceding recovery */
static __thread struct { uint32_t n; struct { uint8_t kind; uint64_t rip; uint64_t icount; } e[16]; } g_recov_ring;
static const char *const g_recov_names[] = { "?", "ras-overflow", "align-interp", "worker-term", "commpage-interp", "sig-deliver", "wild-term", "cache-patch" };
void ocerz_recov_note(int kind, uint64_t rip)
{
    unsigned i = g_recov_ring.n++ & 15;
    g_recov_ring.e[i].kind = (uint8_t)kind;
    g_recov_ring.e[i].rip = rip;
    g_recov_ring.e[i].icount = g_vm ? g_vm->insn_count : 0;
}
void ocerz_recov_dump(FILE *f)
{
    fprintf(f, "ocerz: RECOV[%d] total=%u:", (int)getpid(), g_recov_ring.n);
    unsigned n = g_recov_ring.n < 16 ? g_recov_ring.n : 16;
    for (unsigned i = 0; i < n; i++) {
        unsigned j = (g_recov_ring.n - n + i) & 15;
        fprintf(f, " %s@%#llx/ic=%#llx", g_recov_names[g_recov_ring.e[j].kind],
                (unsigned long long)g_recov_ring.e[j].rip, (unsigned long long)g_recov_ring.e[j].icount);
    }
    fprintf(f, "\n");
}
static __thread uint64_t g_riphist[32];
static __thread unsigned g_riphist_n;
static int g_crash_stack;

static int g_fork_keepjit = -1;

void ocerz_vm_atfork_prepare(void)
{
    pthread_t self = pthread_self();
    if (g_fork_keepjit < 0)
        g_fork_keepjit = getenv("OCERZ_FORK_KEEPJIT") ? 1 : 0;

    pthread_mutex_lock(&g_cpus_lock);
    g_fork_surviving_cpu = NULL;
    for (int i = 0; i < g_cpus_n; i++) {
        if (pthread_equal(g_cpu_threads[i], self)) {
            g_fork_surviving_cpu = g_cpus[i];
            break;
        }
    }
}

void ocerz_vm_atfork_parent(void)
{
    g_fork_surviving_cpu = NULL;
    pthread_mutex_unlock(&g_cpus_lock);
}

void ocerz_vm_atfork_child(void)
{
    OcerzCPU *survivor = g_fork_surviving_cpu;

    for (int i = 0; i < g_cpus_n; i++)
        g_cpus[i] = NULL;
    g_cpus_n = 0;
    if (survivor) {
        g_cpus[0] = survivor;
        g_cpu_threads[0] = pthread_self();
        g_cpus_n = 1;
    }
    g_fork_surviving_cpu = NULL;
    g_pending_async_mask = 0;
    if (survivor)
        survivor->sig_pending = 0;   /* fork clears pending signals in the child */
    g_riphist_n = 0;
    __atomic_store_n(&g_unstick_started, 0, __ATOMIC_RELEASE);
    /* The MAP_JIT arena is inherited READ-ONLY-EXECUTABLE-WISE-BROKEN by a
     * fork child on Apple silicon: the pages read fine but executing them
     * raises SIGBUS (seen as the rare early-exit crash of wine's double-fork
     * intermediate at icount ~0x6ce).  A fork child that does not exec only
     * runs the fork-return + _exit()/execve() path, so drop it to the
     * interpreter. */
    if (g_vm && g_vm->jit_enabled && g_fork_keepjit <= 0) {
        ocerz_jit_forget(g_vm);
        if (survivor) survivor->interp_once = 1;
    }
    pthread_mutex_unlock(&g_cpus_lock);
}

unsigned ocerz_vm_riphist(uint64_t *out, unsigned max)
{
    unsigned n = g_riphist_n < 32 ? g_riphist_n : 32;
    if (n > max) n = max;
    for (unsigned i = 0; i < n; i++)
        out[i] = g_riphist[(g_riphist_n - 1 - i) & 31];
    return n;
}

static int g_sigtrace;
static int g_winefaultlog;

uint64_t ocerz_watch_addr;
uint64_t ocerz_watch_len = 8;
uint64_t ocerz_watch_val;
uint64_t ocerz_watch_shadow;
uint64_t ocerz_exc_trap;

int ocerz_init_tolerant;

static uint64_t ocerz_sel_trap;

static uint64_t ocerz_ctx_trap;

static void ctx_trap_report(const OcerzCPU *c)
{
    static int hits;
    if (hits >= 12)
        return;
    hits++;
    uint64_t f = c->gpr[OCERZ_RCX];
    uint64_t s = c->gpr[OCERZ_RSI];
    fprintf(stderr,
            "ocerz: CTXTRAP pid=%d rip=%#llx rdi=%#llx rsi(ctx_t)=%#llx "
            "ctx.machine=%#x ctx.flags=%#x ctl.rip=%#llx ctl.rsp=%#llx "
            "| f(rcx)=%#llx f.rip=%#llx f.rsp=%#llx gs=%#llx icount=%#llx\n",
            getpid(), (unsigned long long)c->rip,
            (unsigned long long)c->gpr[OCERZ_RDI], (unsigned long long)s,
            (uint32_t)ocerz_ld(s + 0x00, 4), (uint32_t)ocerz_ld(s + 0x04, 4),
            (unsigned long long)ocerz_ld(s + 0x08, 8),
            (unsigned long long)ocerz_ld(s + 0x10, 8),
            (unsigned long long)f,
            (unsigned long long)ocerz_ld(f + 0x70, 8),
            (unsigned long long)ocerz_ld(f + 0x88, 8),
            (unsigned long long)c->gs_base,
            (unsigned long long)(g_vm ? g_vm->insn_count : 0));
}
uint64_t ocerz_bt_lo, ocerz_bt_hi;
static int ocerz_bt_done;

void ocerz_bt_report(const OcerzCPU *c)
{
    if (ocerz_bt_done)
        return;
    ocerz_bt_done = 1;
    fprintf(stderr, "ocerz: BTTRAP rip=%#llx chain:", (unsigned long long)c->rip);
    uint64_t fp = c->gpr[OCERZ_RBP];
    for (int d = 0; d < 18 && fp >= 0x300000000ull; d++) {
        fprintf(stderr, " %#llx", (unsigned long long)ocerz_ld(fp + 8, 8));
        uint64_t nf = ocerz_ld(fp, 8);
        if (nf <= fp) break;
        fp = nf;
    }
    fprintf(stderr, "\n");
}

static void exc_dump_cfstr(const char *tag, uint64_t s)
{
    if (s < 0x100000000ull) {
        fprintf(stderr, " %s=<%#llx>", tag, (unsigned long long)s);
        return;
    }
    uint64_t cstr = ocerz_ld(s + 0x10, 8);
    uint64_t len = ocerz_ld(s + 0x18, 8);
    if (cstr >= 0x100000000ull && len > 0 && len < 4096) {
        fprintf(stderr, " %s=\"", tag);
        for (uint64_t i = 0; i < len; i++) {
            int ch = (int)ocerz_ld(cstr + i, 1);
            fputc(ch >= 32 && ch < 127 ? ch : '?', stderr);
        }
        fputc('"', stderr);
    } else {
        fprintf(stderr, " %s=inline\"", tag);
        for (uint64_t i = 0x10; i < 0x140; i++) {
            int ch = (int)ocerz_ld(s + i, 1);
            if (ch == 0) break;
            fputc(ch >= 32 && ch < 127 ? ch : '?', stderr);
        }
        fputc('"', stderr);
    }
}

void ocerz_exc_report(const OcerzCPU *c)
{
    uint64_t exc = c->gpr[OCERZ_RDI];
    fprintf(stderr, "ocerz: EXCTRAP exc=%#llx isa=%#llx",
            (unsigned long long)exc, (unsigned long long)ocerz_ld(exc, 8));
    exc_dump_cfstr("name", ocerz_ld(exc + 0x08, 8));
    exc_dump_cfstr("reason", ocerz_ld(exc + 0x10, 8));
    fputc('\n', stderr);
}

uint64_t ocerz_arg_trap;

static void arg_trap_report(const OcerzCPU *c)
{
    static const struct { const char *n; int r; } A[] = {
        { "rdi", OCERZ_RDI }, { "rsi", OCERZ_RSI }, { "rdx", OCERZ_RDX },
        { "rcx", OCERZ_RCX }, { "r8",  OCERZ_R8  }, { "r9",  OCERZ_R9  },
    };
    fprintf(stderr, "ocerz: ARGTRAP[%d] rip=%#llx", (int)getpid(), (unsigned long long)c->rip);
    {
        uint64_t rh[32]; unsigned rn = ocerz_vm_riphist(rh, 32);
        fprintf(stderr, " hist:");
        for (unsigned i = 0; i < rn && i < 16; i++)
            fprintf(stderr, " %#llx", (unsigned long long)rh[i]);
    }
    if (ocerz_addr_readable(c->gpr[OCERZ_RSP])) {
        fprintf(stderr, " ra=%#llx", (unsigned long long)ocerz_ld(c->gpr[OCERZ_RSP], 8));
        /* a few more return-address-looking stack words for context */
        for (uint64_t o = 8; o < 0x3000; o += 8) {
            if (!ocerz_addr_readable(c->gpr[OCERZ_RSP] + o)) break;
            uint64_t v = ocerz_ld(c->gpr[OCERZ_RSP] + o, 8);
            int codey = (v >= 0x7ff800000000ull && v < 0x7ffb00000000ull) ||   /* shared cache */
                        (v >= 0x700000000000ull && v < 0x710000000000ull) ||   /* wine .so images */
                        (v >= 0x6fff00000000ull && v < 0x700000000000ull) ||   /* PE builtins */
                        (v >= 0x140000000ull && v < 0x180000000ull) ||          /* PE exes */
                        (v >= 0x7ff000000000ull && v < 0x7ff100000000ull);      /* relocated builtins */
            if (codey)
                fprintf(stderr, " +%#llx:%#llx", (unsigned long long)o, (unsigned long long)v);
        }
    }
    for (unsigned i = 0; i < sizeof A / sizeof A[0]; i++) {
        uint64_t v = c->gpr[A[i].r];
        fprintf(stderr, " %s=%#llx", A[i].n, (unsigned long long)v);
        if (v && ocerz_addr_readable(v) &&
            ocerz_addr_readable(v + 8))
            fprintf(stderr, "->{%#llx,%#llx}", (unsigned long long)ocerz_ld(v, 8),
                    (unsigned long long)ocerz_ld(v + 8, 8));
        else if (v)
            fprintf(stderr, "->UNCOMMITTED");
    }
    fprintf(stderr, "\n");
}

static void sel_trap_report(const OcerzCPU *c)
{
    uint64_t recv = c->gpr[OCERZ_RDI];
    uint64_t sel = c->gpr[OCERZ_RSI];
    uint64_t isa = recv ? (ocerz_ld(recv, 8) & 0x00007ffffffffff8ull) : 0;
    char nm[80];
    int i = 0;
    for (; i < 79 && sel; i++) {
        uint8_t b = (uint8_t)ocerz_ld(sel + (uint64_t)i, 1);
        nm[i] = (char)b;
        if (!b)
            break;
    }
    nm[i] = 0;
    fprintf(stderr,
            "ocerz: SELTRAP rip=%#llx recv=%#llx isa=%#llx sel=%#llx \"%s\" gs=%#llx icount=%llu\n",
            (unsigned long long)c->rip, (unsigned long long)recv, (unsigned long long)isa,
            (unsigned long long)sel, nm, (unsigned long long)c->gs_base,
            g_vm ? (unsigned long long)g_vm->insn_count : 0);
    if (isa && getenv("OCERZ_METHDUMP"))
        ocerz_dyldapi_dump_method(isa, "allocWithZone:");
}

uint64_t ocerz_current_guest_rip(void)
{
    return g_cur_cpu ? g_cur_cpu->rip : 0;
}

uint64_t ocerz_current_guest_rsp(void)
{
    return g_cur_cpu ? g_cur_cpu->gpr[OCERZ_RSP] : 0;
}

uint64_t ocerz_current_guest_gpr(int i)
{
    return (g_cur_cpu && i >= 0 && i < 16) ? g_cur_cpu->gpr[i] : 0;
}
uint64_t ocerz_current_dbg_ind_src(void)
{
    return g_cur_cpu ? g_cur_cpu->dbg_ind_src : 0;
}

void ocerz_watch_hit(uint64_t gaddr, int size, uint64_t lo, uint64_t hi)
{
    OcerzCPU *c = g_cur_cpu ? g_cur_cpu : (g_vm ? &g_vm->cpu : NULL);
    fprintf(stderr, "ocerz: WATCH[pid=%d] st [%#llx] size=%d val=%#llx:%#llx rip=%#llx icount=%llu"
            " rdi=%#llx rsi=%#llx rax=%#llx rbx=%#llx r14=%#llx\n",
            (int)getpid(),
            (unsigned long long)gaddr, size,
            (unsigned long long)hi, (unsigned long long)lo,
            c ? (unsigned long long)c->rip : 0,
            g_vm ? (unsigned long long)g_vm->insn_count : 0,
            c ? (unsigned long long)c->gpr[OCERZ_RDI] : 0,
            c ? (unsigned long long)c->gpr[OCERZ_RSI] : 0,
            c ? (unsigned long long)c->gpr[OCERZ_RAX] : 0,
            c ? (unsigned long long)c->gpr[OCERZ_RBX] : 0,
            c ? (unsigned long long)c->gpr[OCERZ_R14] : 0);
    if (c && getenv("OCERZ_WATCHBT")) {
        uint64_t fp = c->gpr[OCERZ_RBP];
        fprintf(stderr, "ocerz:   WATCHBT gs+0x18=%#llx bt:",
                (unsigned long long)ocerz_ld(c->gs_base + 0x18, 8));
        for (int d = 0; d < 14 && fp >= 0x300000000ull; d++) {
            fprintf(stderr, " %#llx", (unsigned long long)ocerz_ld(fp + 8, 8));
            uint64_t nf = ocerz_ld(fp, 8);
            if (nf <= fp) break;
            fp = nf;
        }
        fprintf(stderr, "\n");
    }
}

static char *hex_into(char *p, uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    *p++ = '0';
    *p++ = 'x';
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        int d = (int)((v >> shift) & 0xf);
        if (d || started || shift == 0) {
            *p++ = digits[d];
            started = 1;
        }
    }
    return p;
}

static char *str_into(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static void async_sig_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)si; (void)ctx;
    if (sig > 0 && sig < 32)
        __atomic_or_fetch(&g_pending_async_mask, 1u << sig, __ATOMIC_SEQ_CST);
}

/* Mirror a guest sigaction() onto the host for plain asynchronous signals.
 * The guest table alone is not enough: the HOST kernel decides what SIGPIPE
 * (write to a closed pipe) does to the process, and its default is to kill
 * it silently.  wineserver ignores SIGPIPE and relies on EPIPE; without
 * this mirror the whole server vanished the first time a client died with
 * a reply in flight.  kind: 0 = SIG_DFL, 1 = SIG_IGN, 2 = guest handler
 * (delivered through the pending-async mask like SIGQUIT/SIGUSR1). */
void ocerz_vm_mirror_host_signal(int sig, int kind)
{
    switch (sig) {
    case SIGPIPE: case SIGHUP: case SIGINT: case SIGTERM: case SIGALRM:
    case SIGCHLD: case SIGWINCH: case SIGURG: case SIGIO: case SIGVTALRM:
    case SIGPROF: case SIGXCPU: case SIGXFSZ: case SIGTSTP: case SIGTTIN:
    case SIGTTOU: case SIGCONT: case SIGINFO:
        break;
    case SIGUSR1:
        /* wine's ntdll delivers server APCs/suspend requests via SIGUSR1;
         * without mirroring, a guest-directed SIGUSR1 either killed the
         * process (default action) or was eaten by the ripdump handler.
         * Keep SIGUSR1 for the ripdump diagnostics only when that env is
         * set (those sessions accept the distortion). */
        if (getenv("OCERZ_RIPDUMP"))
            return;
        break;
    case SIGUSR2:
        break;
    default:
        return;  /* SEGV/BUS/QUIT/ILL/TRAP/FPE/ABRT: ocerz owns these */
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    if (kind == 1)
        sa.sa_handler = SIG_IGN;
    else if (kind == 2) {
        sa.sa_sigaction = async_sig_handler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
        /* wine's SIGUSR1 (server APC/suspend) must interrupt blocked
         * syscalls: EINTR bubbles the thread out of its wait, the vm loop
         * delivers the guest handler, then the guest retries the call. */
        if (sig == SIGUSR1 || sig == SIGUSR2)
            sa.sa_flags &= ~SA_RESTART;
    } else
        sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
}

static void ripdump_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si; (void)ctx;
    if (g_btrace_on > 0)
        __atomic_store_n(&g_btrace_req, 1, __ATOMIC_RELEASE);
    {   /* fan out to every cpu thread once per second at most */
        static _Atomic uint64_t last_fan;
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        uint64_t prev = last_fan;
        if (now - prev > 1000000000ull &&
            __c11_atomic_compare_exchange_strong(&last_fan, &prev, now, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            pthread_t self = pthread_self();
            for (int i = 0; i < g_cpus_n; i++)
                if (!pthread_equal(g_cpu_threads[i], self))
                    pthread_kill(g_cpu_threads[i], SIGUSR1);
        }
    }
    if (!g_cur_cpu)
        return;
    char b[640];
    char *p = b;
    {   /* one thread also dumps every port set in the task */
        static _Atomic int once;
        int exp = 0;
        if (__c11_atomic_compare_exchange_strong(&once, &exp, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            mach_port_name_array_t names = NULL; mach_port_type_array_t types = NULL;
            mach_msg_type_number_t nn = 0, nt = 0;
            if (mach_port_names(mach_task_self(), &names, &nn, &types, &nt) == KERN_SUCCESS) {
                for (unsigned i = 0; i < nn; i++) {
                    if (!(types[i] & MACH_PORT_TYPE_PORT_SET)) continue;
                    char rb[512]; char *q = rb;
                    q = str_into(q, "ocerz: PORTSET ");
                    q = hex_into(q, names[i]);
                    q = str_into(q, " ->");
                    mach_port_name_t *mem = NULL; mach_msg_type_number_t nm = 0;
                    if (mach_port_get_set_status(mach_task_self(), names[i], &mem, &nm) == KERN_SUCCESS) {
                        for (unsigned j = 0; j < nm && j < 32; j++) {
                            q = str_into(q, " ");
                            q = hex_into(q, mem[j]);
                            mach_port_status_t pst;
                            mach_msg_type_number_t pn = MACH_PORT_RECEIVE_STATUS_COUNT;
                            if (mach_port_get_attributes(mach_task_self(), mem[j],
                                                         MACH_PORT_RECEIVE_STATUS,
                                                         (mach_port_info_t)&pst, &pn) == KERN_SUCCESS &&
                                pst.mps_msgcount) {
                                q = str_into(q, ":n=");
                                q = hex_into(q, pst.mps_msgcount);
                            }
                        }
                        if (mem) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)mem, nm * sizeof(*mem));
                    }
                    q = str_into(q, "\n");
                    write(2, rb, (size_t)(q - rb));
                }
                mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)names, nn * sizeof(*names));
                mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)types, nt * sizeof(*types));
            }
            for (int fd = 0; fd < 96; fd++) {
                struct stat st;
                int nread = 0;
                if (fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
                    continue;
                if (ioctl(fd, FIONREAD, &nread) != 0)
                    continue;
                if (!nread)
                    continue;
                char fb[96]; char *fq = fb;
                fq = str_into(fq, "ocerz: PIPE fd=");
                fq = hex_into(fq, (uint64_t)fd);
                fq = str_into(fq, " nread=");
                fq = hex_into(fq, (uint64_t)nread);
                fq = str_into(fq, "\n");
                write(2, fb, (size_t)(fq - fb));
            }
            once = 0;
        }
    }
    if (g_cur_cpu->last_rcv_name) {
        char rb[400]; char *q = rb;
        q = str_into(q, "ocerz: RIPDUMP rcv-port=");
        q = hex_into(q, g_cur_cpu->last_rcv_name);
        mach_port_name_t *members = NULL;
        mach_msg_type_number_t nmem = 0;
        if (mach_port_get_set_status(mach_task_self(), g_cur_cpu->last_rcv_name, &members, &nmem) == KERN_SUCCESS) {
            q = str_into(q, " set-members:");
            for (unsigned i = 0; i < nmem && i < 24; i++) { q = str_into(q, " "); q = hex_into(q, members[i]); }
            if (members) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)members, nmem * sizeof(*members));
        } else
            q = str_into(q, " (not-a-set)");
        q = str_into(q, "\n");
        write(2, rb, (size_t)(q - rb));
    }
    p = str_into(p, "ocerz: RIPDUMP cpu#");
    p = hex_into(p, g_cur_cpu->cpu_number);
    p = str_into(p, " slot3=");
    p = hex_into(p, ocerz_addr_readable(g_cur_cpu->gs_base + 0x18)
                     ? ocerz_ld(g_cur_cpu->gs_base + 0x18, 8) : 0);
    p = str_into(p, " ctid=");    /* cached thread_selfid at TSD[-1] */
    p = hex_into(p, ocerz_addr_readable(g_cur_cpu->gs_base - 8)
                     ? ocerz_ld(g_cur_cpu->gs_base - 8, 8) : 0);
    p = str_into(p, " rip=");
    p = hex_into(p, g_cur_cpu->rip);
    p = str_into(p, " gs=");
    p = hex_into(p, g_cur_cpu->gs_base);
    p = str_into(p, " hist:");
    for (int i = 1; i <= 12; i++) {
        p = str_into(p, " ");
        p = hex_into(p, g_riphist[(g_riphist_n - (unsigned)i) & 31]);
    }
    *p++ = '\n';
    write(2, b, (size_t)(p - b));
    p = b;
    static const char *const rn[] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                      "r8","r9","r10","r11","r12","r13","r14","r15" };
    p = str_into(p, "ocerz:   regs");
    for (int i = 0; i < 16; i++) {
        p = str_into(p, " ");
        p = str_into(p, rn[i]);
        p = str_into(p, "=");
        p = hex_into(p, g_cur_cpu->gpr[i]);
    }
    *p++ = '\n';
    write(2, b, (size_t)(p - b));

    {   /* OCERZ_MACDRVDUMP=<winemac.so base>: dump the WineApplicationController
         * request machinery (singleton at +0x560f0; +0x8 requestsSource,
         * +0x10 requests NSMutableArray, +0x18 requestsManipQueue). */
        static uint64_t mbase; static int minit;
        if (!minit) {
            const char *e = getenv("OCERZ_MACDRVDUMP");
            mbase = e ? strtoull(e, NULL, 0) : 0;
            minit = 1;
        }
        static _Atomic int monce;
        int mexp = 0;
        if (mbase &&
            __c11_atomic_compare_exchange_strong(&monce, &mexp, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            char mb[512]; char *mq = mb;
            mq = str_into(mq, "ocerz: MACDRV token=");
            mq = hex_into(mq, ocerz_addr_readable(mbase + 0x560f8)
                              ? ocerz_ld(mbase + 0x560f8, 8) : 0);
            mq = str_into(mq, " ctrl=");
            uint64_t slot = mbase + 0x560f0;
            uint64_t ctrl = ocerz_addr_readable(slot) ? ocerz_ld(slot, 8) : 0;
            mq = hex_into(mq, ctrl);
            if (ctrl && ocerz_addr_readable(ctrl) && ocerz_addr_readable(ctrl + 0x1f)) {
                uint64_t src = ocerz_ld(ctrl + 0x8, 8);
                uint64_t arr = ocerz_ld(ctrl + 0x10, 8);
                uint64_t mnq = ocerz_ld(ctrl + 0x18, 8);
                mq = str_into(mq, " src="); mq = hex_into(mq, src);
                mq = str_into(mq, " arr="); mq = hex_into(mq, arr);
                mq = str_into(mq, " q=");   mq = hex_into(mq, mnq);
                if (src && ocerz_addr_readable(src) && ocerz_addr_readable(src + 0x5f)) {
                    mq = str_into(mq, " src.info=");
                    mq = hex_into(mq, ocerz_ld(src + 8, 8));
                    mq = str_into(mq, " src.sig58=");
                    mq = hex_into(mq, ocerz_ld(src + 0x58, 8));
                }
                if (arr && ocerz_addr_readable(arr) && ocerz_addr_readable(arr + 0x2f)) {
                    mq = str_into(mq, " arrw:");
                    for (int k = 0; k < 6; k++) { mq = str_into(mq, " "); mq = hex_into(mq, ocerz_ld(arr + 8ull * k, 8)); }
                    uint64_t st = ocerz_ld(arr + 0x10, 8);
                    if (st && ocerz_addr_readable(st) && ocerz_addr_readable(st + 0x2f)) {
                        mq = str_into(mq, " deque:");
                        for (int k = 0; k < 6; k++) { mq = str_into(mq, " "); mq = hex_into(mq, ocerz_ld(st + 8ull * k, 8)); }
                        for (int k = 0; k < 4; k++) {
                            uint64_t obj = ocerz_ld(st + 8ull * k, 8);
                            if (obj && ocerz_addr_readable(obj) && ocerz_addr_readable(obj + 0x17)) {
                                mq = str_into(mq, " inv");
                                mq = hex_into(mq, (uint64_t)k);
                                mq = str_into(mq, "=");
                                mq = hex_into(mq, ocerz_ld(obj + 0x10, 8));
                            }
                        }
                        /* DEFINITIVE: NSMutableArray count (w4>>32), head (w3 low32),
                         * and each live element (storage[head..head+count]) with its
                         * block invoke pointer so enqueue-loss vs lost-wakeup is settled. */
                        {
                            uint64_t w3 = ocerz_ld(arr + 0x18, 8);
                            uint64_t w4 = ocerz_ld(arr + 0x20, 8);
                            uint64_t cnt = w4 >> 32;
                            uint64_t head = w3 & 0xffffffffu;
                            uint64_t cap  = w3 >> 32;
                            mq = str_into(mq, " COUNT="); mq = hex_into(mq, cnt);
                            mq = str_into(mq, " head="); mq = hex_into(mq, head);
                            mq = str_into(mq, " cap="); mq = hex_into(mq, cap);
                            if (st && cap && ocerz_addr_readable(st)) {
                                mq = str_into(mq, " elems:");
                                for (uint64_t e = 0; e < cnt && e < 6; e++) {
                                    uint64_t idx = cap ? (head + e) % cap : e;
                                    uint64_t el = ocerz_ld(st + 8 * idx, 8);
                                    mq = str_into(mq, " ");
                                    mq = hex_into(mq, el);
                                    if (el && ocerz_addr_readable(el + 0x17)) {
                                        mq = str_into(mq, "/inv=");
                                        mq = hex_into(mq, ocerz_ld(el + 0x10, 8));
                                    }
                                }
                            }
                        }
                    }
                }
            }
            mq = str_into(mq, "\n");
            write(2, mb, (size_t)(mq - mb));
            if (getenv("OCERZ_UNFREEZE") && ctrl && ocerz_addr_readable(ctrl + 8)) {
                /* force-signal the macdrv request source: if a request is
                 * still queued despite count==0, the next runloop pass will
                 * perform it and the session unfreezes. */
                uint64_t fsrc = ocerz_ld(ctrl + 8, 8);
                if (fsrc && ocerz_addr_readable(fsrc + 0x58)) {
                    ocerz_st(fsrc + 0x58, 8, 0x123456789abull);
                    const char *msg = "ocerz: UNFREEZE signal word forced\n";
                    write(2, msg, strlen(msg));
                }
            }
            {   /* hunt live OnMainThread wrapper blocks (invoke = winemac
                 * +0x11a50) and the thunk blocks (+0x1e770) in the guest
                 * heap; print their captured words so the frozen request's
                 * actual target queue/array can be identified. */
                uint64_t inv1 = mbase + 0x11a50, inv2 = mbase + 0x1e770;
                uint64_t ctrl_isa = 0;
                {   /* count WineApplicationController instances: compare isa
                     * words against the known singleton's isa */
                    uint64_t sl = mbase + 0x560f0;
                    uint64_t c0 = ocerz_addr_readable(sl) ? ocerz_ld(sl, 8) : 0;
                    if (c0 && ocerz_addr_readable(c0))
                        ctrl_isa = ocerz_ld(c0, 8);
                }
                static const uint64_t ranges[][2] = {
                    { 0x100000000ull, 0x140000000ull },
                    { 0x7040000000ull, 0x7070000000ull },
                };
                for (int ri = 0; ri < 2; ri++)
                for (uint64_t page = ranges[ri][0]; page < ranges[ri][1];
                     page += 0x1000) {
                    if (!ocerz_addr_readable(page))
                        continue;
                    for (uint64_t off = 0; off < 0x1000; off += 16) {
                        uint64_t a = page + off;
                        uint64_t w = ocerz_ld(a, 8);
                        if (ctrl_isa && w == ctrl_isa && off == 0 + (a & 0xff0) - (a & 0xff0)) {
                            /* isa match at any 16-aligned slot: report */
                            char cb[80]; char *cq = cb;
                            cq = str_into(cq, "ocerz: CTRLOBJ ");
                            cq = hex_into(cq, a);
                            cq = str_into(cq, "\n");
                            write(2, cb, (size_t)(cq - cb));
                        }
                        if (w != inv1 && w != inv2)
                            continue;
                        /* candidate block: invoke at +0x10 => block base a-0x10 */
                        uint64_t blk = a - 0x10;
                        uint64_t isa = ocerz_ld(blk, 8);
                        uint64_t fl = ocerz_ld(blk + 8, 8);
                        char hb[300]; char *hq = hb;
                        hq = str_into(hq, "ocerz: WRAPBLK ");
                        hq = hex_into(hq, blk);
                        hq = str_into(hq, w == inv1 ? " kind=wrapper" : " kind=thunk");
                        hq = str_into(hq, " isa=");
                        hq = hex_into(hq, isa);
                        hq = str_into(hq, " fl=");
                        hq = hex_into(hq, fl);
                        hq = str_into(hq, " caps:");
                        for (int k = 0; k < 5; k++) {
                            hq = str_into(hq, " ");
                            hq = hex_into(hq, ocerz_addr_readable(blk + 0x20 + 8ull * k)
                                              ? ocerz_ld(blk + 0x20 + 8ull * k, 8) : 0);
                        }
                        hq = str_into(hq, "\n");
                        write(2, hb, (size_t)(hq - hb));
                        if (w == inv1 && (uint32_t)fl == 0xc3000002u) {
                            /* heap wrapper: find every holder of a pointer
                             * to it (the array backing store that owns it) */
                            for (int rj = 0; rj < 2; rj++)
                            for (uint64_t p2 = ranges[rj][0]; p2 < ranges[rj][1];
                                 p2 += 0x1000) {
                                if (!ocerz_addr_readable(p2))
                                    continue;
                                for (uint64_t o2 = 0; o2 < 0x1000; o2 += 8) {
                                    if (ocerz_ld(p2 + o2, 8) != blk)
                                        continue;
                                    char rb2[100]; char *rq = rb2;
                                    rq = str_into(rq, "ocerz: WRAPREF holder=");
                                    rq = hex_into(rq, p2 + o2);
                                    rq = str_into(rq, "\n");
                                    write(2, rb2, (size_t)(rq - rb2));
                                }
                            }
                        }
                    }
                }
            }
            monce = 0;
        }
    }
    {   /* guest backtrace by rbp-chain walk (system frameworks keep
         * frame pointers, so this is reliable through CF/AppKit/wine) */
        p = b;
        p = str_into(p, "ocerz:   gbt");
        uint64_t fp = g_cur_cpu->gpr[5];   /* rbp */
        for (int i = 0; i < 24; i++) {
            if (!fp || (fp & 7) || !ocerz_addr_readable(fp) ||
                !ocerz_addr_readable(fp + 15))
                break;
            uint64_t ra = ocerz_ld(fp + 8, 8);
            uint64_t nfp = ocerz_ld(fp, 8);
            if (!ra)
                break;
            {   /* OnMainThread frame (returns into the thunk at winemac
                 * +0x1e763): dump the __block "finished" byref cell. */
                static uint64_t wbase; static int winit;
                if (!winit) {
                    const char *e = getenv("OCERZ_MACDRVDUMP");
                    wbase = e ? strtoull(e, NULL, 0) : 0;
                    winit = 1;
                }
                if (wbase && ra == wbase + 0x1e763 &&
                    ocerz_addr_readable(fp - 0x40) &&
                    ocerz_addr_readable(fp - 0x39)) {
                    uint64_t fwd = ocerz_ld(fp - 0x40, 8);
                    char fb[200]; char *fq = fb;
                    fq = str_into(fq, "\nocerz:   FINISHED cell=");
                    fq = hex_into(fq, fp - 0x48);
                    fq = str_into(fq, " fwd=");
                    fq = hex_into(fq, fwd);
                    if (fwd && ocerz_addr_readable(fwd) &&
                        ocerz_addr_readable(fwd + 0x1f)) {
                        fq = str_into(fq, " flags=");
                        fq = hex_into(fq, ocerz_ld(fwd + 0x10, 8));
                        fq = str_into(fq, " val=");
                        fq = hex_into(fq, ocerz_ld(fwd + 0x18, 1));
                    }
                    fq = str_into(fq, " stackval=");
                    fq = hex_into(fq, ocerz_ld(fp - 0x48 + 0x18, 1));
                    fq = str_into(fq, "\n");
                    write(2, fb, (size_t)(fq - fb));
                }
            }
            p = str_into(p, " ");
            p = hex_into(p, ra);
            if (nfp <= fp)
                break;
            fp = nfp;
            if ((size_t)(p - b) > sizeof b - 40)
                break;
        }
        *p++ = '\n';
        write(2, b, (size_t)(p - b));
    }

    for (int i = 0; i < 24; i++) {
        uint64_t r = g_riphist[(g_riphist_n - 1 - (unsigned)i) & 31];
        if (!r) continue;
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (g_riphist[(g_riphist_n - 1 - (unsigned)j) & 31] == r) { dup = 1; break; }
        if (dup) continue;
        char x[160];
        char *q = x;
        q = str_into(q, "ocerz:   @");
        q = hex_into(q, r);
        if (!ocerz_addr_readable(r) ||
            !ocerz_addr_readable(r + 15)) {
            q = str_into(q, " (uncommitted)\n");
            write(2, x, (size_t)(q - x));
            continue;
        }
        q = str_into(q, " =");
        for (int k = 0; k < 16; k++) {
            uint64_t by = ocerz_ld(r + (uint64_t)k, 1);
            *q++ = ' ';
            *q++ = "0123456789abcdef"[(by >> 4) & 0xf];
            *q++ = "0123456789abcdef"[by & 0xf];
        }
        *q++ = '\n';
        write(2, x, (size_t)(q - x));
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx)
{
    static __thread volatile int depth;

    /* first touch of a lazily-slid shared-cache data page (host or guest
     * access alike): unpack it and retry the faulting instruction */
    int align_fault = 0;
    if (sig == SIGSEGV || sig == SIGBUS) {
        const ucontext_t *luc = (const ucontext_t *)ctx;
        uint64_t lesr = luc ? luc->uc_mcontext->__es.__esr : 0;
        align_fault = sig == SIGBUS && (lesr & 0x3f) == 0x21;
        if (!align_fault && ocerz_cache_lazy_fault((uintptr_t)si->si_addr))
            return;
    }
    /* A raw host pointer the guest was handed inline (IOSurface's per-client
     * page, mapped into the task by the kernel during io_connect_method and
     * returned in the output struct; anything IOKit maps for a client): the
     * guest address is unmapped in the shadow but the same numeric address
     * is a live device/shared mapping in the host.  Alias it at that guest
     * address and retry, the way the SkyLight universe pages are aliased. */
    if ((sig == SIGSEGV || sig == SIGBUS) && !align_fault && g_vm &&
        ocerz_host_in_guest_space(si->si_addr)) {
        static __thread uint64_t last_alias_page;
        uint64_t ga = ocerz_h2g(si->si_addr);
        uint64_t page = ga & ~0x3fffull;
        if (ga < OCERZ_LOW_LIMIT && page != last_alias_page && !ocerz_addr_readable(ga)) {
            extern int ocerz_host_region_is_device(uint64_t, int *);
            extern int ocerz_alias_raw_region(OcerzVM *, uint64_t);
            int hprot = 0;
            if (ocerz_host_region_is_device(ga, &hprot)) {
                last_alias_page = page;
                if (ocerz_alias_raw_region(g_vm, ga) == 0 && ocerz_addr_readable(ga)) {
                    if (g_sigtrace) {
                        char ab[120]; char *w = ab;
                        w = str_into(w, "ocerz: RAWALIAS-ON-FAULT gaddr=");
                        w = hex_into(w, ga);
                        w = str_into(w, "\n");
                        write(2, ab, (size_t)(w - ab));
                    }
                    return;
                }
            }
        }
    }

    if (ocerz_jit_decode_recover)
        siglongjmp(*ocerz_jit_decode_recover, 1);

    /* Alignment fault in translated code: an ordered (acquire/release)
     * access crossed a 16-byte boundary.  The address can be anything
     * readable (shared-cache constants included), so this is handled before
     * the guest-space test: mark the block for alignment-checked
     * retranslation, run the instruction in the interpreter, resume. */
    /* Host-stack RAS overflow: the CALL's shadow push (stp x, x, [sp, #-16]!)
     * hit the host stack guard.  Nothing of the CALL has executed yet (the
     * shadow push comes first), so run the CALL in the interpreter and
     * abandon the JIT frame; the shadow restarts empty. */
    if ((sig == SIGSEGV || sig == SIGBUS) && depth == 0 && g_cur_cpu && g_sig_recover && ctx) {
        const ucontext_t *uc = (const ucontext_t *)ctx;
        const uint32_t *hpc = (const uint32_t *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
        struct OcerzVM *fvm = g_cur_cpu->vm;
        if (fvm && ocerz_jit_pc_in_arena(fvm, hpc) && (*hpc & 0xffff83e0u) == 0xa9bf03e0u &&
            (uint64_t)(uintptr_t)si->si_addr - (uc->uc_mcontext->__ss.__sp - 16) < 32) {
            depth = 1;
            ocerz_jit_fault_recover_regs(fvm, hpc, uc->uc_mcontext->__ss.__x, g_cur_cpu);
            ocerz_jit_fault_recover_xmm(fvm, hpc, uc->uc_mcontext->__ns.__v, g_cur_cpu);
            ocerz_jit_fault_recover_flags(fvm, hpc, g_cur_cpu);
            ocerz_flags_materialize(g_cur_cpu);
            uint64_t jrip;
            if (ocerz_jit_fault_rip(fvm, hpc, &jrip)) {
                g_cur_cpu->rip = jrip;
                g_cur_cpu->sig_repeat = 0;
                g_cur_cpu->interp_once = 1;
                if (getenv("OCERZ_RASLOG"))
                    fprintf(stderr, "ocerz: host RAS overflow[%d] at rip=%#llx sp=%#llx: CALL via interpreter\n",
                            (int)getpid(), (unsigned long long)jrip, (unsigned long long)uc->uc_mcontext->__ss.__sp);
                ocerz_recov_note(1, jrip);
                depth = 0;
                siglongjmp(*g_sig_recover, 1);
            }
            depth = 0;
        }
    }

    if (align_fault && depth == 0 && g_cur_cpu && g_sig_recover && ctx) {
        const ucontext_t *uc = (const ucontext_t *)ctx;
        const void *hpc = (const void *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
        struct OcerzVM *fvm = g_cur_cpu->vm;
        if (fvm && ocerz_jit_pc_in_arena(fvm, hpc)) {
            /* preferred: hot-patch the one access into an alignment-checked
             * arm and re-execute it (nothing executed, nothing invalidated) */
            int hp = ocerz_jit_hotpatch_align(fvm, hpc);
            if (hp) {
                if (getenv("OCERZ_ALFAULTLOG"))
                    fprintf(stderr, "ocerz: ALFAULT hotpatch=%d hpc=%p addr=%p\n", hp, hpc, si->si_addr);
                return;
            }
            depth = 1;
            ocerz_jit_fault_recover_regs(fvm, hpc, uc->uc_mcontext->__ss.__x, g_cur_cpu);
            ocerz_jit_fault_recover_xmm(fvm, hpc, uc->uc_mcontext->__ns.__v, g_cur_cpu);
            ocerz_jit_fault_recover_flags(fvm, hpc, g_cur_cpu);
            ocerz_flags_materialize(g_cur_cpu);
            uint64_t jrip;
            if (ocerz_jit_fault_rip(fvm, hpc, &jrip) &&
                ocerz_jit_note_align_fault(fvm, hpc, jrip)) {
                g_cur_cpu->rip = jrip;
                g_cur_cpu->sig_repeat = 0;
                g_cur_cpu->interp_once = 1;
                if (getenv("OCERZ_ALFAULTLOG"))
                    fprintf(stderr, "ocerz: ALFAULT[%d] rip=%#llx addr=%p\n", (int)getpid(), (unsigned long long)jrip, si->si_addr);
                ocerz_recov_note(2, jrip);
                depth = 0;
                siglongjmp(*g_sig_recover, 1);
            }
            depth = 0;
        }
    }

    /* Store into a shared-cache page the guest made writable to patch it.  The
     * page is armed read-only once code has been translated out of it, so this
     * fault IS the notification: grant write, drop the stale translations and
     * restart the store. */
    if ((sig == SIGSEGV || sig == SIGBUS) && !align_fault && depth == 0 &&
        g_cur_cpu && g_sig_recover && ctx) {
        const ucontext_t *uc = (const ucontext_t *)ctx;
        uint64_t esr = uc->uc_mcontext->__es.__esr;
        uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);
        if (ec != 0x20 && ec != 0x21 && (esr & (1u << 6)) &&
            ocerz_cache_write_fault((uintptr_t)si->si_addr)) {
            struct OcerzVM *fvm = g_cur_cpu->vm;
            const void *hpc = (const void *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
            uint64_t page = (uint64_t)(uintptr_t)si->si_addr & ~(OCERZ_HOST_PAGE_SIZE - 1);
            uint64_t jrip = 0;
            int in_jit = fvm && ocerz_jit_pc_in_arena(fvm, hpc) &&
                         ocerz_jit_fault_rip(fvm, hpc, &jrip);
            if (in_jit) {
                depth = 1;
                ocerz_jit_fault_recover_regs(fvm, hpc, uc->uc_mcontext->__ss.__x, g_cur_cpu);
                ocerz_jit_fault_recover_xmm(fvm, hpc, uc->uc_mcontext->__ns.__v, g_cur_cpu);
                ocerz_jit_fault_recover_flags(fvm, hpc, g_cur_cpu);
                ocerz_flags_materialize(g_cur_cpu);
            }
            ocerz_jit_invalidate_range(fvm, page, OCERZ_HOST_PAGE_SIZE);
            if (getenv("OCERZ_CACHEPATCHLOG"))
                fprintf(stderr, "ocerz: CACHEPATCH[%d] rip=%#llx addr=%p injit=%d\n",
                        (int)getpid(), (unsigned long long)jrip, si->si_addr, in_jit);
            if (in_jit) {
                g_cur_cpu->rip = jrip;
                g_cur_cpu->sig_repeat = 0;
                ocerz_recov_note(7, jrip);
                depth = 0;
                siglongjmp(*g_sig_recover, 1);
            }
            return;              /* interpreter store: retrying it is enough */
        }
    }

    if (depth == 0 && g_cur_cpu && g_sig_recover &&
        ocerz_host_in_guest_space(si->si_addr)) {
        depth = 1;
        const ucontext_t *uc = (const ucontext_t *)ctx;
        const void *hpc = uc ? (const void *)(uintptr_t)uc->uc_mcontext->__ss.__pc : NULL;
        struct OcerzVM *fvm = g_cur_cpu->vm;
        int in_jit = hpc && fvm && ocerz_jit_pc_in_arena(fvm, hpc);
        if (in_jit) {

            ocerz_jit_fault_recover_regs(fvm, hpc,
                uc->uc_mcontext->__ss.__x, g_cur_cpu);
            ocerz_jit_fault_recover_xmm(fvm, hpc,
                uc->uc_mcontext->__ns.__v, g_cur_cpu);
            ocerz_jit_fault_recover_flags(fvm, hpc, g_cur_cpu);
        }

        ocerz_flags_materialize(g_cur_cpu);

        uint64_t fault_rip = g_cur_cpu->cur_rip;
        int rip_exact = 1;
        {
            if (in_jit) {

                uint64_t jrip;
                if (ocerz_jit_fault_rip(fvm, hpc, &jrip))
                    fault_rip = jrip;
                else
                    rip_exact = 0;
            }

        }
        uint64_t gs = g_cur_cpu->gs_base;
        uint64_t gaddr = ocerz_h2g(si->si_addr);
        int code = ocerz_addr_committed(gaddr) == 1 ? 2 : 1;

        uint64_t esr = ctx ? ((const ucontext_t *)ctx)->uc_mcontext->__es.__esr : 0;
        uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);
        int is_fetch = (ec == 0x20 || ec == 0x21);
        int is_write = !is_fetch && (esr & (1u << 6)) != 0;
        uint32_t err = 0x4u;
        if (code == 2) err |= 0x1u;
        if (is_write) err |= 0x2u;
        if (is_fetch) err |= 0x10u;

        if (gaddr != g_cur_cpu->sig_last_fault) {
            g_cur_cpu->sig_last_fault = gaddr;
            g_cur_cpu->sig_repeat = 0;
        }
        int looping = ++g_cur_cpu->sig_repeat > OCERZ_SIG_MAX_REPEAT;

        int no_wine_teb = g_cur_cpu->sig_altstack_sp == 0;
        if (!no_wine_teb) {
            uint64_t sb = g_cur_cpu->gpr[OCERZ_RSP] & ~0xffffull;
            if (ocerz_addr_readable(sb) &&
                ocerz_addr_committed(ocerz_ld(sb, 8)) != 1)
                no_wine_teb = 1;
        }
        if (looping && no_wine_teb) {
            if (g_sigtrace) {
                char wb[160];
                char *w = wb;
                w = str_into(w, "ocerz: gs0x320 WORKER-TERMINATE pid=");
                w = hex_into(w, (uint64_t)getpid());
                w = str_into(w, " gaddr=");
                w = hex_into(w, gaddr);
                w = str_into(w, " gs=");
                w = hex_into(w, gs);
                w = str_into(w, " icount=");
                w = hex_into(w, g_vm ? g_vm->insn_count : 0);
                w = str_into(w, "\n");
                write(2, wb, (size_t)(w - wb));
            }
            g_cur_cpu->terminated = 1;
            ocerz_recov_note(3, g_cur_cpu->rip);
            depth = 0;
            siglongjmp(*g_sig_recover, 1);
        }

        {
            static int rewind_off = -1;
            if (rewind_off < 0)
                rewind_off = getenv("OCERZ_NO_RIP_REWIND") ? 1 : 0;
            if (rip_exact && !rewind_off)
                g_cur_cpu->rip = fault_rip;
        }
        {
            static int faultlog = -1;
            if (faultlog < 0)
                faultlog = getenv("OCERZ_FAULTLOG") ? 1 : 0;
            if (faultlog) {
                uint64_t rb = 0, rs = 0;
                unsigned hp = ocerz_host_region_prot(gaddr, &rb, &rs);
                char fb[384];
                char *f = fb;
                f = str_into(f, "ocerz: FAULT-MAP addr=");
                f = hex_into(f, gaddr);
                f = str_into(f, " owned=");
                f = hex_into(f, (uint64_t)(int64_t)ocerz_addr_committed(gaddr));
                f = str_into(f, " slot_prot=");
                f = hex_into(f, (uint64_t)(int64_t)ocerz_addr_prot(gaddr));
                f = str_into(f, " host_prot=");
                f = hex_into(f, hp);
                f = str_into(f, " region=");
                f = hex_into(f, rb);
                f = str_into(f, "+");
                f = hex_into(f, rs);
                f = str_into(f, " rip=");
                f = hex_into(f, g_cur_cpu->rip);
                f = str_into(f, " hpc=");
                f = hex_into(f, (uint64_t)(uintptr_t)hpc);
                f = str_into(f, " hinsn=");
                f = hex_into(f, hpc ? *(const uint32_t *)hpc : 0);
                f = str_into(f, " esr=");
                f = hex_into(f, esr);
                f = str_into(f, "\n");
                write(2, fb, (size_t)(f - fb));
                if (in_jit && uc) {
                    OcerzJitFaultInfo ji;
                    if (ocerz_jit_fault_info(fvm, hpc, &ji)) {
                        char jb[640];
                        char *j = jb;
                        j = str_into(j, "ocerz: JITFAULT block=");
                        j = hex_into(j, ji.block_rip);
                        j = str_into(j, " insn=");
                        j = hex_into(j, ji.insn_rip);
                        j = str_into(j, " idx=");
                        j = hex_into(j, (uint64_t)(uint32_t)ji.insn_index);
                        j = str_into(j, " hoff=");
                        j = hex_into(j, ji.host_word);
                        j = str_into(j, " class=");
                        j = hex_into(j, ji.pin_class);
                        j = str_into(j, " pins=");
                        j = hex_into(j, ji.n_pinned);
                        for (int i = 0; i < ji.n_pinned; i++) {
                            j = str_into(j, " x");
                            j = hex_into(j, (uint64_t)(21 + i));
                            j = str_into(j, "/g");
                            j = hex_into(j, ji.host_holds[i]);
                            j = str_into(j, "=");
                            j = hex_into(j,
                                uc->uc_mcontext->__ss.__x[21 + i]);
                        }
                        j = str_into(j, "\n");
                        write(2, jb, (size_t)(j - jb));
                    }
                }
            }
        }
        if (in_jit && rip_exact && ocerz_commpage && ocerz_guest_base == 0 &&
            gaddr >= OCERZ_COMMPAGE_LO && gaddr < OCERZ_COMMPAGE_HI &&
            ocerz_jit_note_commpage_fault(fvm, hpc, fault_rip)) {
            /* plain-form access to the emulated commpage: the block is now
             * marked for guarded retranslation; resume at the instruction */
            g_cur_cpu->rip = fault_rip;
            g_cur_cpu->sig_repeat = 0;
            g_cur_cpu->interp_once = 1;      /* this instruction: interpreter (exact commpage semantics) */
            if (getenv("OCERZ_CPFAULTLOG")) {
                fprintf(stderr, "ocerz: CPFAULT rip=%#llx gaddr=%#llx\n", (unsigned long long)fault_rip, (unsigned long long)gaddr);
                ocerz_cpu_dump(g_cur_cpu, stderr);
            }
            ocerz_recov_note(4, fault_rip);
            depth = 0;
            siglongjmp(*g_sig_recover, 1);
        }
        uint64_t fault_rsp = g_cur_cpu->gpr[OCERZ_RSP];
        int delivered = looping ? 0
                       : ocerz_signal_deliver(g_cur_cpu, SIGSEGV, gaddr, code, err);
        if (g_winefaultlog && delivered && g_cur_cpu->sig_altstack_sp != 0) {
            int teb_committed = gs <= UINT64_MAX - 16 &&
                                ocerz_addr_readable(gs + 8) &&
                                ocerz_addr_readable(gs + 16);
            uint64_t stack_base = teb_committed ? ocerz_ld(gs + 8, 8) : 0;
            uint64_t stack_limit = teb_committed ? ocerz_ld(gs + 16, 8) : 0;
            char wb[512];
            char *w = wb;
            w = str_into(w, "ocerz: WINEFAULT host_sig=");
            w = hex_into(w, (uint64_t)sig);
            w = str_into(w, " addr=");
            w = hex_into(w, gaddr);
            w = str_into(w, " rip=");
            w = hex_into(w, fault_rip);
            w = str_into(w, " gs=");
            w = hex_into(w, gs);
            w = str_into(w, " rsp=");
            w = hex_into(w, fault_rsp);
            w = str_into(w, " TEB+8.StackBase=");
            w = hex_into(w, stack_base);
            w = str_into(w, " TEB+16.StackLimit=");
            w = hex_into(w, stack_limit);
            w = str_into(w, " teb_committed=");
            w = hex_into(w, (uint64_t)teb_committed);
            w = str_into(w, " low_base=");
            w = hex_into(w, ocerz_low_base);
            w = str_into(w, " h2g(rsp)=");
            w = hex_into(w, ocerz_h2g((const void *)(uintptr_t)fault_rsp));
            w = str_into(w, "\n");
            write(2, wb, (size_t)(w - wb));
        }
        if (g_sigtrace) {
            char tb[256];
            char *t = tb;
            t = str_into(t, delivered ? "ocerz: SIG[" : "ocerz: SIGNH[");
            t = hex_into(t, (uint64_t)getpid());
            t = str_into(t, "] deliver addr=");
            t = hex_into(t, gaddr);
            t = str_into(t, " rip=");
            t = hex_into(t, fault_rip);
            t = str_into(t, " gs=");
            t = hex_into(t, gs);
            t = str_into(t, gaddr == gs - 8 ? " [==gs-8]" : "");
            t = str_into(t, " comm(addr)=");
            t = hex_into(t, (uint64_t)(int64_t)ocerz_addr_committed(gaddr));
            t = str_into(t, " comm(gs)=");
            t = hex_into(t, (uint64_t)(int64_t)ocerz_addr_committed(gs));
            t = str_into(t, " ->tramp=");
            t = hex_into(t, delivered ? g_cur_cpu->rip : 0);
            t = str_into(t, " icount=");
            t = hex_into(t, g_vm ? g_vm->insn_count : 0);
            t = str_into(t, "\n");
            write(2, tb, (size_t)(t - tb));
            {   /* frame-pointer chain and the images holding addr/rip */
                char bb[640]; char *w = bb;
                uint64_t ib = 0; const char *in = ocerz_dyld_name_for_addr(gaddr, &ib);
                w = str_into(w, "ocerz:   addr-image=");
                w = str_into(w, in ? in : "<none>");
                w = str_into(w, " bt:");
                uint64_t fp = g_cur_cpu->gpr[OCERZ_RBP];
                for (int d = 0; d < 12 && fp > 0x1000 && ocerz_addr_readable(fp + 8) && w < bb + 560; d++) {
                    w = str_into(w, " ");
                    w = hex_into(w, ocerz_ld(fp + 8, 8));
                    uint64_t nf = ocerz_addr_readable(fp) ? ocerz_ld(fp, 8) : 0;
                    if (nf <= fp) break;
                    fp = nf;
                }
                w = str_into(w, "\n");
                write(2, bb, (size_t)(w - bb));
            }
            if (fault_rip != 0) {
                char xb[200];
                char *x = xb;
                x = str_into(x, "ocerz:   insn@");
                x = hex_into(x, fault_rip);
                x = str_into(x, " =");
                for (int i = -3; i < 12; i++) {
                    uint64_t b = ocerz_ld(fault_rip + (uint64_t)(int64_t)i, 1);
                    *x++ = ' ';
                    *x++ = "0123456789abcdef"[(b >> 4) & 0xf];
                    *x++ = "0123456789abcdef"[b & 0xf];
                }
                x = str_into(x, "\n");
                write(2, xb, (size_t)(x - xb));
            } else {
                char xb[512];
                char *x = xb;
                x = str_into(x, "ocerz:   rip0 hist:");
                for (int i = 2; i <= 24; i++) {
                    x = str_into(x, " ");
                    x = hex_into(x, g_riphist[(g_riphist_n - (unsigned)i) & 31]);
                }
                x = str_into(x, "\n");
                write(2, xb, (size_t)(x - xb));
            }
            {

                char gb[256];
                char *g = gb;
                static const char *nm[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                           "r8","r9","r10","r11","r12","r13","r14","r15"};
                g = str_into(g, "ocerz:   gpr");
                for (int i = 0; i < 16; i++) {
                    g = str_into(g, " ");
                    g = str_into(g, nm[i]);
                    g = str_into(g, "=");
                    g = hex_into(g, g_cur_cpu->gpr[i]);
                }
                g = str_into(g, "\n");
                write(2, gb, (size_t)(g - gb));
            }
            {

                static const int regs[] = {OCERZ_RSI, OCERZ_R12, OCERZ_R11};
                static const char *rn[] = {"rsi","r12","r11"};
                for (int k = 0; k < 3; k++) {
                    uint64_t b = g_cur_cpu->gpr[regs[k]];
                    char mb[256]; char *m = mb;
                    m = str_into(m, "ocerz:   mem["); m = str_into(m, rn[k]);
                    m = str_into(m, "="); m = hex_into(m, b); m = str_into(m, "]:");
                    for (int q = 0; q < 8; q++) {
                        uint64_t a = b + (uint64_t)q * 8;
                        m = str_into(m, " ");
                        if (ocerz_addr_readable(a))
                            m = hex_into(m, ocerz_ld(a, 8));
                        else
                            m = str_into(m, "<unc>");
                    }
                    m = str_into(m, "\n");
                    write(2, mb, (size_t)(m - mb));
                }
            }
            if (code == 1) {
                char cb[400];
                char *c = cb;
                c = str_into(c, "ocerz:   commitmap ");
                uint64_t pg = gaddr & ~0xfffull;
                uint64_t scanlo = pg - 0x10000;
                c = hex_into(c, scanlo);
                c = str_into(c, "..: ");
                for (int i = 0; i < 36; i++) {
                    uint64_t a = scanlo + (uint64_t)i * 0x1000;
                    int cm = ocerz_addr_committed(a);
                    *c++ = (a == pg) ? '[' : ' ';
                    *c++ = cm == 1 ? 'C' : (cm == 0 ? '.' : '?');
                    *c++ = (a == pg) ? ']' : ' ';
                }
                c = str_into(c, "\n");
                write(2, cb, (size_t)(c - cb));
            }
        }
        depth = 0;
        if (delivered) {
            ocerz_recov_note(5, fault_rip);
            siglongjmp(*g_sig_recover, 1);
        }
    }

    if (g_cur_cpu && g_sig_recover && depth == 0 &&
        !ocerz_host_in_guest_space(si->si_addr)) {
        int no_teb = g_cur_cpu->sig_altstack_sp == 0;
        if (!no_teb) {
            uint64_t sb = g_cur_cpu->gpr[OCERZ_RSP] & ~0xffffull;
            uint64_t teb = ocerz_addr_readable(sb)
                         ? ocerz_ld(sb, 8) : 0;
            if (ocerz_addr_committed(teb) != 1)
                no_teb = 1;
        }
        if (no_teb) {
            if (g_sigtrace) {
                static volatile unsigned wild_logs;
                unsigned n = __atomic_fetch_add(&wild_logs, 1, __ATOMIC_RELAXED);
                if (n < 32) {
                    const ucontext_t *uc = (const ucontext_t *)ctx;
                    char tb[512];
                    char *t = tb;
                    t = str_into(t, "ocerz: WILD-WORKER-TERMINATE pid=");
                    t = hex_into(t, (uint64_t)getpid());
                    t = str_into(t, " addr=");
                    t = hex_into(t, (uint64_t)(uintptr_t)si->si_addr);
                    t = str_into(t, " host_pc=");
                    t = hex_into(t, uc ? uc->uc_mcontext->__ss.__pc : 0);
                    t = str_into(t, " rip=");
                    t = hex_into(t, g_cur_cpu->rip);
                    t = str_into(t, " cur_rip=");
                    t = hex_into(t, g_cur_cpu->cur_rip);
                    t = str_into(t, " rsp=");
                    t = hex_into(t, g_cur_cpu->gpr[OCERZ_RSP]);
                    t = str_into(t, " gs=");
                    t = hex_into(t, g_cur_cpu->gs_base);
                    OcerzJitFaultInfo wji;
                    if (uc && g_cur_cpu->vm && ocerz_jit_pc_in_arena(g_cur_cpu->vm, (const void *)(uintptr_t)uc->uc_mcontext->__ss.__pc) &&
                        ocerz_jit_fault_info(g_cur_cpu->vm, (const void *)(uintptr_t)uc->uc_mcontext->__ss.__pc, &wji)) {
                        t = str_into(t, " jit-block=");
                        t = hex_into(t, wji.block_rip);
                        t = str_into(t, " insn=");
                        t = hex_into(t, wji.insn_rip);
                        t = str_into(t, " hoff=");
                        t = hex_into(t, wji.host_word);
                        t = str_into(t, " hinsn=");
                        t = hex_into(t, *(const uint32_t *)(uintptr_t)uc->uc_mcontext->__ss.__pc);
                        t = str_into(t, " esr=");
                        t = hex_into(t, uc->uc_mcontext->__es.__esr);
                    }
                    t = str_into(t, "\n");
                    write(2, tb, (size_t)(t - tb));
                }
            }
            g_cur_cpu->terminated = 1;
            ocerz_recov_note(6, g_cur_cpu->rip);
            siglongjmp(*g_sig_recover, 1);
        }
    }
    char buf[640];
    char *p = buf;

    if (depth > 0 && ocerz_host_in_guest_space(si->si_addr)) {
        uint64_t fg = ocerz_h2g(si->si_addr);
        if (ocerz_addr_committed(fg) == 0 && ocerz_commit_fault_page(fg))
            return;
    }
    if (depth++) {
        p = str_into(p, "ocerz: nested fault inside crash handler host_addr=");
        p = hex_into(p, (uint64_t)(uintptr_t)si->si_addr);
        if (ctx) {
            const ucontext_t *uc = (const ucontext_t *)ctx;
            p = str_into(p, " host_pc=");
            p = hex_into(p, uc->uc_mcontext->__ss.__pc);
        }
        p = str_into(p, " gaddr=");
        p = hex_into(p, ocerz_host_in_guest_space(si->si_addr) ? ocerz_h2g(si->si_addr) : 0);
        p = str_into(p, " comm=");
        p = hex_into(p, (uint64_t)(int64_t)(ocerz_host_in_guest_space(si->si_addr)
                                            ? ocerz_addr_committed(ocerz_h2g(si->si_addr)) : -2));
        p = str_into(p, " prot=");
        p = hex_into(p, (uint64_t)(int64_t)(ocerz_host_in_guest_space(si->si_addr)
                                            ? ocerz_addr_prot(ocerz_h2g(si->si_addr)) : -2));
        p = str_into(p, " slide=");
        p = hex_into(p, g_image_slide);
        p = str_into(p, "\n");
        write(2, buf, (size_t)(p - buf));
        _exit(139);
    }
    p = str_into(p, "ocerz: guest crash[");
    p = hex_into(p, (uint64_t)(uint32_t)getpid());
    p = str_into(p, "] cpu#");
    p = hex_into(p, g_cur_cpu ? (uint64_t)g_cur_cpu->cpu_number : 0xffff);
    p = str_into(p, " ");
    p = str_into(p, sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" :
                    sig == SIGTRAP ? "SIGTRAP" : sig == SIGSYS ? "SIGSYS" : "SIGSEGV");
    p = str_into(p, " host_addr=");
    p = hex_into(p, (uint64_t)(uintptr_t)si->si_addr);
    if (ctx) {
        const ucontext_t *uc = (const ucontext_t *)ctx;
        uint64_t hpc = uc->uc_mcontext->__ss.__pc;
        p = str_into(p, " host_pc=");
        p = hex_into(p, hpc);
        p = str_into(p, " host_insn=");
        p = hex_into(p, *(const uint32_t *)(uintptr_t)hpc);
        p = str_into(p, " host_lr=");
        p = hex_into(p, uc->uc_mcontext->__ss.__lr);
        p = str_into(p, " slide=");
        p = hex_into(p, g_image_slide);
        write(2, buf, (size_t)(p - buf)); p = buf;
        p = str_into(p, "\n  host-x:");
        for (int i = 0; i < 29; i++) {
            p = str_into(p, i % 8 == 0 ? "\n    " : " ");
            p = hex_into(p, uc->uc_mcontext->__ss.__x[i]);
        }
        p = str_into(p, " fp=");
        p = hex_into(p, uc->uc_mcontext->__ss.__fp);
        p = str_into(p, " lr=");
        p = hex_into(p, uc->uc_mcontext->__ss.__lr);
        p = str_into(p, " sp=");
        p = hex_into(p, uc->uc_mcontext->__ss.__sp);
        write(2, buf, (size_t)(p - buf)); p = buf;
        if (sig == SIGILL) {
            /* find who branched here: scan the arena for b/bl/b.cond/cbz/tbz
             * words whose target is the fault pc */
            uint64_t pc0 = uc->uc_mcontext->__ss.__pc;
            const uint32_t *cb, *ce;
            if (g_vm && ocerz_jit_code_range(g_vm, &cb, &ce)) {
                p = str_into(p, "  jit-owner=");
                p = hex_into(p, (uint64_t)(uint32_t)ocerz_jit_owner_pid(g_vm));
                p = str_into(p, " self=");
                p = hex_into(p, (uint64_t)(uint32_t)getpid());
                p = str_into(p, " arena=[");
                p = hex_into(p, (uint64_t)(uintptr_t)cb);
                p = str_into(p, ",");
                p = hex_into(p, (uint64_t)(uintptr_t)ce);
                p = str_into(p, ") base-word=");
                p = hex_into(p, *cb);
                p = str_into(p, "\n  branch-sites->pc:");
                int found = 0;
                for (const uint32_t *w = cb; w < ce && found < 8; w++) {
                    uint32_t v = *w;
                    int64_t off = 0; int is = 0;
                    if ((v & 0x7c000000u) == 0x14000000u) { off = (int64_t)((int32_t)(v << 6) >> 6) * 4; is = 1; }          /* b/bl */
                    else if ((v & 0xff000010u) == 0x54000000u || (v & 0x7e000000u) == 0x34000000u) { off = (int64_t)((int32_t)((v >> 5) << 13) >> 13) * 4; is = 1; }  /* b.cond / cbz */
                    if (is && (uint64_t)(uintptr_t)w + (uint64_t)off == pc0) {
                        p = str_into(p, " ");
                        p = hex_into(p, (uint64_t)(uintptr_t)w);
                        p = str_into(p, "/w=");
                        p = hex_into(p, v);
                        found++;
                    }
                }
                if (!found) p = str_into(p, " none");
                p = str_into(p, "\n");
                write(2, buf, (size_t)(p - buf)); p = buf;
                {
                    OcerzJitFaultInfo fi;
                    if (ocerz_jit_fault_info(g_vm, (const void *)(uintptr_t)pc0, &fi)) {
                        p = str_into(p, "  owner-block: rip=");
                        p = hex_into(p, fi.block_rip);
                        p = str_into(p, " word=");
                        p = hex_into(p, fi.host_word);
                        p = str_into(p, " insn=");
                        p = hex_into(p, (uint64_t)(int64_t)fi.insn_index);
                        p = str_into(p, " pin=");
                        p = hex_into(p, fi.pin_class);
                        p = str_into(p, "\n");
                    } else
                        p = str_into(p, "  owner-block: none\n");
                    write(2, buf, (size_t)(p - buf)); p = buf;
                }
                {
                    uint32_t w[64];
                    vm_size_t got = 0;
                    if (mach_vm_read_overwrite(mach_task_self(), pc0 - 0x80, sizeof w,
                                               (mach_vm_address_t)(uintptr_t)w, (mach_vm_size_t *)&got) == KERN_SUCCESS && got == sizeof w) {
                        p = str_into(p, "  mem@pc-80:");
                        for (int i = 0; i < 64; i++) {
                            p = str_into(p, i == 32 ? " |" : " ");
                            p = hex_into(p, w[i]);
                            if (i == 47) { p = str_into(p, "\n   "); write(2, buf, (size_t)(p - buf)); p = buf; }
                        }
                        p = str_into(p, "\n");
                        write(2, buf, (size_t)(p - buf)); p = buf;
                    }
                }
            }
        }
        p = str_into(p, "  host-stack:");
        {
            /* host sp is not guest memory: probe with mach instead */
            uint64_t a0 = uc->uc_mcontext->__ss.__sp;
            vm_size_t got = 0;
            uint64_t w[12];
            if (mach_vm_read_overwrite(mach_task_self(), a0, sizeof w,
                                       (mach_vm_address_t)(uintptr_t)w, (mach_vm_size_t *)&got) == KERN_SUCCESS)
                for (unsigned i = 0; i * 8 < (uint64_t)got && i < 12; i++) {
                    p = str_into(p, " ");
                    p = hex_into(p, w[i]);
                }
        }
        p = str_into(p, "\n");
        write(2, buf, (size_t)(p - buf)); p = buf;
        /* host frame-pointer chain (symbolize offline: atos -o ocerz -l <slide+0x100000000> addr...) */
        p = str_into(p, " host_bt=");
        uint64_t fp = uc->uc_mcontext->__ss.__fp;
        for (int i = 0; i < 8 && fp && (fp & 7) == 0 && ocerz_addr_readable(fp) && ocerz_addr_readable(fp + 8); i++) {
            uint64_t ret = *(const uint64_t *)(uintptr_t)(fp + 8);
            if (!ret) break;
            p = hex_into(p, ret);
            p = str_into(p, ",");
            fp = *(const uint64_t *)(uintptr_t)fp;
        }
    }
    OcerzCPU *c = g_cur_cpu ? g_cur_cpu : (g_vm ? &g_vm->cpu : NULL);
    if (c) {
        p = str_into(p, " guest_rip=");
        p = hex_into(p, c->rip);
        p = str_into(p, " guest_addr=");
        p = hex_into(p, ocerz_h2g(si->si_addr));
        p = str_into(p, " icount=");
        p = hex_into(p, g_vm ? g_vm->insn_count : 0);
        p = str_into(p, " cur_rip=");
        p = hex_into(p, c->cur_rip);
        p = str_into(p, " interp_once=");
        p = hex_into(p, (uint64_t)c->interp_once);
        p = str_into(p, " exec_state=");
        p = hex_into(p, (uint64_t)ocerz_jit_exec_state);
    }
    p = str_into(p, "\n");
    write(2, buf, (size_t)(p - buf));
    if (c) {
        static const char *const rn[] = { "rax", "rcx", "rdx", "rbx",
                                          "rsi", "rdi", "rbp", "r8" };
        static const int ri[] = { OCERZ_RAX, OCERZ_RCX, OCERZ_RDX, OCERZ_RBX,
                                  OCERZ_RSI, OCERZ_RDI, OCERZ_RBP, OCERZ_R8 };
        p = buf;
        p = str_into(p, "  regs:");
        for (int i = 0; i < 8; i++) {
            p = str_into(p, " ");
            p = str_into(p, rn[i]);
            p = str_into(p, "=");
            p = hex_into(p, c->gpr[ri[i]]);
        }
        p = str_into(p, "\n");
        write(2, buf, (size_t)(p - buf));
        {
            p = buf;
            p = str_into(p, "  gs_base=");
            p = hex_into(p, c->gs_base);
            p = str_into(p, " fs_base=");
            p = hex_into(p, c->fs_base);
            p = str_into(p, " insn@rip-24=");
            for (int i = -24; i < 16; i++) {
                uint64_t b = ocerz_ld(c->rip + (uint64_t)(int64_t)i, 1);
                *p++ = ' ';
                if (i == 0) *p++ = '[';
                *p++ = "0123456789abcdef"[(b >> 4) & 0xf];
                *p++ = "0123456789abcdef"[b & 0xf];
            }
            *p++ = '\n';
            write(2, buf, (size_t)(p - buf));
            p = buf;
            p = str_into(p, "  r12=");
            p = hex_into(p, c->gpr[OCERZ_R12]);
            p = str_into(p, " r13=");
            p = hex_into(p, c->gpr[OCERZ_R13]);
            p = str_into(p, " r14=");
            p = hex_into(p, c->gpr[OCERZ_R14]);
            p = str_into(p, " r15=");
            p = hex_into(p, c->gpr[OCERZ_R15]);
            p = str_into(p, " [r15+0x18]=");
            p = hex_into(p, ocerz_addr_readable(c->gpr[OCERZ_R15] + 0x18)
                              ? ocerz_ld(c->gpr[OCERZ_R15] + 0x18, 8) : 0);
            *p++ = '\n';
            write(2, buf, (size_t)(p - buf));
            p = buf;
            p = str_into(p, "  blockhist:");
            for (int i = 1; i <= 16; i++) {
                p = str_into(p, " ");
                p = hex_into(p, g_riphist[(g_riphist_n - (unsigned)i) & 31]);
            }
            *p++ = '\n';
            write(2, buf, (size_t)(p - buf));
        }
        {
            int comm = ocerz_addr_committed(ocerz_h2g(si->si_addr));
            const char *cs = comm == 1 ? "  fault-page: COMMITTED"
                           : comm == 0 ? "  fault-page: UNCOMMITTED"
                                       : "  fault-page: outside-arena";
            p = buf;
            p = str_into(p, cs);
            p = str_into(p, sig == SIGBUS
                ? (si->si_code == 1 ? " si_code=ADRALN"
                 : si->si_code == 2 ? " si_code=ADRERR"
                 : si->si_code == 3 ? " si_code=OBJERR" : " si_code=?")
                : "");
            uint64_t rbase = 0, rsize = 0;
            unsigned hp = ocerz_host_region_prot(ocerz_h2g(si->si_addr), &rbase, &rsize);
            p = str_into(p, " host_prot=");
            p = hex_into(p, hp);
            p = str_into(p, " region=[");
            p = hex_into(p, rbase);
            p = str_into(p, ",");
            p = hex_into(p, rbase + rsize);
            p = str_into(p, ")\n");
            write(2, buf, (size_t)(p - buf));
            uint64_t ripbase = 0, ripsize = 0;
            unsigned riphp = ocerz_host_region_prot(c->rip, &ripbase, &ripsize);
            p = buf;
            p = str_into(p, "  rip-region=[");
            p = hex_into(p, ripbase);
            p = str_into(p, ",");
            p = hex_into(p, ripbase + ripsize);
            p = str_into(p, ") prot=");
            p = hex_into(p, riphp);
            p = str_into(p, "\n");
            write(2, buf, (size_t)(p - buf));
        }
        uint64_t fp = c->gpr[OCERZ_RBP];
        p = buf;
        p = str_into(p, "  rbp-chain:");
        for (int d = 0; d < 9 && fp >= 0x300000000ull; d++) {
            if (!ocerz_addr_readable(fp) ||
                !ocerz_addr_readable(fp + 8))
                break;
            p = str_into(p, " ");
            p = hex_into(p, ocerz_ld(fp + 8, 8));
            uint64_t nf = ocerz_ld(fp, 8);
            if (nf <= fp)
                break;
            fp = nf;
        }
        p = str_into(p, "\n");
        write(2, buf, (size_t)(p - buf));
        const char *pk = getenv("OCERZ_PEEK");
        if (pk) {
            p = buf;
            p = str_into(p, "  peek:");
            while (*pk) {
                uint64_t a = strtoull(pk, (char **)&pk, 0);
                if (*pk == ',')
                    pk++;
                p = str_into(p, " [");
                p = hex_into(p, a);
                p = str_into(p, "]=");
                if (ocerz_addr_readable(a))
                    p = hex_into(p, ocerz_ld(a, 8));
                else
                    p = str_into(p, "uncommitted");
            }
            p = str_into(p, "\n");
            write(2, buf, (size_t)(p - buf));
        }
        const char *sd = getenv("OCERZ_STRDUMP");
        if (sd) {
            uint64_t a = strtoull(sd, NULL, 0);
            p = buf;
            p = str_into(p, "  strdump@");
            p = hex_into(p, a);
            p = str_into(p, "=");
            for (int i = 0; i < 200 && p < buf + 250; i++) {
                uint64_t b = ocerz_ld(a + (uint64_t)i, 1);
                *p++ = (b >= 32 && b < 127) ? (char)b : '.';
            }
            *p++ = '\n';
            write(2, buf, (size_t)(p - buf));
        }
        uint64_t sp = c->gpr[OCERZ_RSP];
        int shown = 0;
        p = buf;
        p = str_into(p, "  bt:");
        for (uint64_t a = sp; a < sp + 0x400 && shown < 14; a += 8) {
            if (!ocerz_addr_readable(a)) break;         /* the report must not fault itself */
            uint64_t v = ocerz_ld(a, 8);
            if (v >= 0x7ff802000000ull && v < 0x7ff818000000ull) {
                p = str_into(p, " ");
                p = hex_into(p, v);
                shown++;
            }
        }
        p = str_into(p, "\n");
        write(2, buf, (size_t)(p - buf));
        if (g_crash_stack) {
            static const char *const an[] = { "r9", "r10", "r11", "r12",
                                              "r13", "r14", "r15", "rsp" };
            static const int ai[] = { OCERZ_R9, OCERZ_R10, OCERZ_R11, OCERZ_R12,
                                      OCERZ_R13, OCERZ_R14, OCERZ_R15, OCERZ_RSP };
            p = buf;
            p = str_into(p, "  regs2:");
            for (int i = 0; i < 8; i++) {
                p = str_into(p, " ");
                p = str_into(p, an[i]);
                p = str_into(p, "=");
                p = hex_into(p, c->gpr[ai[i]]);
            }
            p = str_into(p, "\n");
            write(2, buf, (size_t)(p - buf));
            uint64_t rbp = c->gpr[OCERZ_RBP];
            for (int row = 0; row < 14; row++) {
                uint64_t base = rbp - 0x80 + (uint64_t)row * 0x10;
                p = buf;
                p = str_into(p, "  [rbp");
                p = str_into(p, base >= rbp ? "+" : "-");
                p = hex_into(p, base >= rbp ? base - rbp : rbp - base);
                p = str_into(p, "]:");
                for (int col = 0; col < 2; col++) {
                    p = str_into(p, " ");
                    p = hex_into(p, ocerz_ld(base + (uint64_t)col * 8, 8));
                }
                p = str_into(p, "\n");
                write(2, buf, (size_t)(p - buf));
            }
            {
                uint64_t a = c->gpr[OCERZ_R14];
                if (a >= 0x100000000ull) {
                    for (int row = 0; row < 8; row++) {
                        p = buf;
                        p = str_into(p, "  *r14+");
                        p = hex_into(p, (uint64_t)row * 0x20);
                        p = str_into(p, ":");
                        for (int col = 0; col < 4; col++) {
                            p = str_into(p, " ");
                            p = hex_into(p, ocerz_ld(a + (uint64_t)(row * 4 + col) * 8, 8));
                        }
                        p = str_into(p, "\n");
                        write(2, buf, (size_t)(p - buf));
                    }
                }
            }
        }
    }
    _exit(139);
}

int ocerz_vm_init(OcerzVM *vm)
{
    g_image_slide = (uint64_t)_dyld_get_image_vmaddr_slide(0);
    memset(vm, 0, sizeof *vm);
    vm->cpu.vm = vm;
    ocerz_cpu_reset(&vm->cpu);
    vm->jit_enabled = 1;
    return OCERZ_OK;
}

/* OCERZ_PORTDUMP=1 + SIGUSR2: dump every receive right with queued messages.
 * Diagnostic for lost-wakeup wedges: a port with a growing queue and no
 * receiver names the conversation whose delivery ocerz dropped. */
static void portdump_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si; (void)ctx;
    mach_port_name_array_t names = NULL;
    mach_port_type_array_t types = NULL;
    mach_msg_type_number_t ncnt = 0, tcnt = 0;
    if (mach_port_names(mach_task_self(), &names, &ncnt, &types, &tcnt) !=
        KERN_SUCCESS)
        return;
    fprintf(stderr, "ocerz: PORTDUMP[%d] rights=%u\n", (int)getpid(), ncnt);
    for (mach_msg_type_number_t i = 0; i < ncnt; i++) {
        if (!(types[i] & MACH_PORT_TYPE_RECEIVE))
            continue;
        mach_port_status_t st;
        mach_msg_type_number_t cnt = MACH_PORT_RECEIVE_STATUS_COUNT;
        if (mach_port_get_attributes(mach_task_self(), names[i],
                                     MACH_PORT_RECEIVE_STATUS,
                                     (mach_port_info_t)&st,
                                     &cnt) != KERN_SUCCESS)
            continue;
        if (!st.mps_msgcount)
            continue;
        mach_port_seqno_t seq = 0;
        mach_msg_size_t msize = 0;
        mach_msg_id_t mid = 0;
        char tinfo[68];
        mach_msg_type_number_t tsz = sizeof(tinfo);
        kern_return_t pk = mach_port_peek(mach_task_self(), names[i],
                                          MACH_RCV_TRAILER_TYPE(MACH_RCV_TRAILER_NULL),
                                          &seq, &msize, &mid,
                                          (mach_msg_trailer_info_t)tinfo, &tsz);
        fprintf(stderr,
                "ocerz: PORTDUMP[%d] port=%#x msgs=%u psets=%u srights=%u "
                "sorights=%u peek_kr=%d id=%#x size=%u\n",
                (int)getpid(), names[i], st.mps_msgcount,
                (unsigned)st.mps_pset, (unsigned)st.mps_srights,
                (unsigned)st.mps_sorights, pk, mid, msize);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)(uintptr_t)names,
                  ncnt * sizeof(*names));
    vm_deallocate(mach_task_self(), (vm_address_t)(uintptr_t)types,
                  tcnt * sizeof(*types));
}

static void ocerz_install_kick_handler(void);
void ocerz_vm_install_handlers(OcerzVM *vm)
{
    ocerz_install_kick_handler();
    extern int ocerz_cftrap_on;
    ocerz_cftrap_on = getenv("OCERZ_CFTRAP") != NULL;
    g_crash_stack = getenv("OCERZ_CRASH_STACK") != NULL;
    g_sigtrace = getenv("OCERZ_SIGTRACE") != NULL;
    g_winefaultlog = getenv("OCERZ_WINEFAULTLOG") != NULL;
    const char *w = getenv("OCERZ_WATCH");
    { const char *wl = getenv("OCERZ_WATCHLEN"); if (wl) ocerz_watch_len = strtoull(wl, NULL, 0); }
    if (w)
        ocerz_watch_addr = strtoull(w, NULL, 0);
    const char *wv = getenv("OCERZ_STVAL");
    if (wv)
        ocerz_watch_val = strtoull(wv, NULL, 0);
    ocerz_watch_shadow = getenv("OCERZ_SHADOWST") != NULL;
    const char *et = getenv("OCERZ_EXCTRAP");
    if (et)
        ocerz_exc_trap = strtoull(et, NULL, 0);
    const char *st = getenv("OCERZ_SELTRAP");
    if (st)
        ocerz_sel_trap = strtoull(st, NULL, 0);
    const char *gt = getenv("OCERZ_ARGTRAP");
    if (gt)
        ocerz_arg_trap = strtoull(gt, NULL, 0);
    const char *ct = getenv("OCERZ_CTXTRAP");
    if (ct)
        ocerz_ctx_trap = strtoull(ct, NULL, 0);
    const char *bl = getenv("OCERZ_BT_LO");
    const char *bh = getenv("OCERZ_BT_HI");
    if (bl && bh) {
        ocerz_bt_lo = strtoull(bl, NULL, 0);
        ocerz_bt_hi = strtoull(bh, NULL, 0);
    }
    static stack_t altss;
    if (!altss.ss_sp) {
        altss.ss_size = SIGSTKSZ < 0x10000 ? 0x10000 : (size_t)SIGSTKSZ;
        altss.ss_sp = mmap(NULL, altss.ss_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
        if (altss.ss_sp != MAP_FAILED)
            sigaltstack(&altss, NULL);
        else
            altss.ss_sp = NULL;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | (altss.ss_sp ? SA_ONSTACK : 0);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    /* SIGILL/SIGTRAP/SIGSYS have no default handler in ocerz, so a jump to a
     * garbage code pointer used to kill the process SILENTLY (macOS .ips was
     * the only trace; three of those on 2026-08-17: br to an mmap pool base /
     * arena code_cur that was never written).  Route them through the crash
     * reporter so the death is diagnosable in our own logs. */
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGSYS, &sa, NULL);

    struct sigaction as;
    memset(&as, 0, sizeof as);
    as.sa_sigaction = async_sig_handler;
    as.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGQUIT, &as, NULL);
    if (getenv("OCERZ_RIPDUMP")) {
        struct sigaction su;
        memset(&su, 0, sizeof su);
        su.sa_sigaction = ripdump_handler;
        su.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGUSR1, &su, NULL);
    } else {
        sigaction(SIGUSR1, &as, NULL);
    }
    if (getenv("OCERZ_PORTDUMP")) {
        struct sigaction sp;
        memset(&sp, 0, sizeof sp);
        sp.sa_sigaction = portdump_handler;
        sp.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGUSR2, &sp, NULL);
    }
    g_vm = vm;
    if (vm->jit_enabled && !vm->jit)
        vm->jit = ocerz_jit_create(vm);
}

uint64_t ocerz_vm_call(OcerzVM *vm, uint64_t func, const uint64_t *args, int nargs, uint64_t stack_top)
{
    static const int ar[6] = { OCERZ_RDI, OCERZ_RSI, OCERZ_RDX, OCERZ_RCX, OCERZ_R8, OCERZ_R9 };
    static uint64_t sentinel;
    if (!sentinel) {
        void *want = (void *)(uintptr_t)0x500000000ull;
        void *p = mmap(want, 0x1000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (p == want) {
            memset(p, 0xcc, 0x1000);
            sentinel = 0x500000000ull;
        } else {
            sentinel = OCERZ_CALL_SENTINEL;
        }
        OCERZ_LOG("vm: call sentinel page at %#llx\n", (unsigned long long)sentinel);
    }
    OcerzCPU *prev_cpu = g_cur_cpu;
    OcerzCPU local = prev_cpu ? *prev_cpu : vm->cpu;
    local.terminated = 0;
    for (int i = 0; i < nargs && i < 6; i++)
        local.gpr[ar[i]] = args[i];
    uint64_t sp = (stack_top & ~0xfull) - 8;
    ocerz_st(sp, 8, sentinel);
    local.gpr[OCERZ_RSP] = sp;
    local.rip = func;
    g_cur_cpu = &local;
    const char *icap_s = getenv("OCERZ_ICAP");
    unsigned long long icap = icap_s ? strtoull(icap_s, NULL, 0) : 0;
    static uint64_t riptrap[16];
    static int riptrap_n = -1;
    static unsigned char riptrap_hit[16];
    if (riptrap_n < 0) {
        riptrap_n = 0;
        const char *rs = getenv("OCERZ_RIPLOG");
        while (rs && *rs && riptrap_n < 16) {
            riptrap[riptrap_n++] = strtoull(rs, (char **)&rs, 0);
            while (*rs == ',' || *rs == ' ') rs++;
        }
    }
    const char *prof_s = getenv("OCERZ_PROFILE");
    unsigned long long prof = prof_s ? strtoull(prof_s, NULL, 0) : 0;
    unsigned long long prof_next = prof ? vm->insn_count + prof : 0;

    static uint64_t mtrace_lo, mtrace_hi;
    static int mtrace_init;
    if (!mtrace_init) {
        mtrace_init = 1;
        const char *ml = getenv("OCERZ_TRACE_MAIN_LO");
        const char *mh = getenv("OCERZ_TRACE_MAIN_HI");
        if (ml && mh) {
            mtrace_lo = strtoull(ml, NULL, 0);
            mtrace_hi = strtoull(mh, NULL, 0);
        }
    }

    const int any_diag = (ocerz_exc_trap || ocerz_sel_trap || ocerz_arg_trap || ocerz_ctx_trap ||
                          ocerz_bt_lo || prof || riptrap_n > 0 || icap || mtrace_lo);
    sigjmp_buf jb;
    sigjmp_buf *prev_recover = g_sig_recover;
    g_sig_recover = &jb;
    sigsetjmp(jb, 1);
    g_cur_cpu = &local;
    while (local.rip != sentinel && !vm->exited && !local.terminated) {
        g_riphist[g_riphist_n++ & 31] = local.rip;
        int r;
        int mtrace_hit = 0;
        if (__builtin_expect(any_diag, 0)) {
        if (ocerz_exc_trap && local.rip == ocerz_exc_trap)
            ocerz_exc_report(&local);
        if (ocerz_arg_trap && local.rip == ocerz_arg_trap)
            arg_trap_report(&local);
        if (ocerz_sel_trap && local.rip == ocerz_sel_trap)
            sel_trap_report(&local);
        if (ocerz_ctx_trap && local.rip == ocerz_ctx_trap)
            ctx_trap_report(&local);
        if (ocerz_bt_lo && local.rip >= ocerz_bt_lo && local.rip < ocerz_bt_hi)
            ocerz_bt_report(&local);
        if (prof && vm->insn_count >= prof_next) {
            prof_next = vm->insn_count + prof;
            fprintf(stderr, "ocerz: PROFILE icount=%llu rip=%#llx\n",
                    (unsigned long long)vm->insn_count, (unsigned long long)local.rip);
        }
        for (int t = 0; t < riptrap_n; t++) {
            if (local.rip == riptrap[t] && riptrap_hit[t] < 40) {
                riptrap_hit[t]++;
                fprintf(stderr, "ocerz: RIPLOG hit %#llx (#%d) icount=%llu rdi=%#llx rsi=%#llx rdx=%#llx rbx=%#llx r14=%#llx\n",
                        (unsigned long long)riptrap[t], riptrap_hit[t],
                        (unsigned long long)vm->insn_count,
                        (unsigned long long)local.gpr[OCERZ_RDI],
                        (unsigned long long)local.gpr[OCERZ_RSI],
                        (unsigned long long)local.gpr[OCERZ_RDX],
                        (unsigned long long)local.gpr[OCERZ_RBX],
                        (unsigned long long)local.gpr[OCERZ_R14]);
            }
        }
        if (icap && vm->insn_count > icap) {
            ocerz_cpu_dump(&local, stderr);
            uint64_t fp = local.gpr[OCERZ_RBP];
            fprintf(stderr, "ocerz: rbp-chain:");
            for (int d = 0; d < 200 && fp >= 0x300000000ull; d++) {
                uint64_t ret = ocerz_ld(fp + 8, 8);
                fprintf(stderr, " %#llx", (unsigned long long)ret);
                uint64_t nf = ocerz_ld(fp, 8);
                if (nf <= fp) break;
                fp = nf;
            }
            fprintf(stderr, "\n");
            fprintf(stderr, "ocerz: ICAP hit at %llu instructions, func=%#llx\n",
                    (unsigned long long)vm->insn_count, (unsigned long long)func);
            _exit(126);
        }
        if (mtrace_lo && local.rip >= mtrace_lo && local.rip < mtrace_hi) {
            fprintf(stderr, "MT %#llx rax=%#llx rdi=%#llx rsi=%#llx rsp=%#llx [rsp]=%#llx\n",
                    (unsigned long long)local.rip,
                    (unsigned long long)local.gpr[OCERZ_RAX],
                    (unsigned long long)local.gpr[OCERZ_RDI],
                    (unsigned long long)local.gpr[OCERZ_RSI],
                    (unsigned long long)local.gpr[OCERZ_RSP],
                    (unsigned long long)ocerz_ld(local.gpr[OCERZ_RSP], 8));
            mtrace_hit = 1;
        }
        }

        if (mtrace_hit || local.interp_once) {
            int was_once = local.interp_once;
            local.interp_once = 0;
            r = ocerz_interp_step(vm, &local);
            if (was_once && getenv("OCERZ_CPFAULTLOG"))
                fprintf(stderr, "ocerz: INTERP-ONCE done -> rip=%#llx rax=%#llx rcx=%#llx rdx=%#llx r=%d\n",
                        (unsigned long long)local.rip, (unsigned long long)local.gpr[OCERZ_RAX],
                        (unsigned long long)local.gpr[OCERZ_RCX], (unsigned long long)local.gpr[OCERZ_RDX], r);
        } else if (vm->jit_enabled && (vm->jit || (vm->jit = ocerz_jit_create(vm)))) {
            r = ocerz_jit_step(vm, &local);
            if (r == OCERZ_EUNSUP)
                r = ocerz_interp_step(vm, &local);
        } else {
            r = ocerz_interp_step(vm, &local);
        }
        if (r == OCERZ_STEP_EXIT)
            break;
        if (r == OCERZ_STEP_FATAL) {
            static int forcetol = -1;
            if (forcetol < 0) forcetol = getenv("OCERZ_INITTOL") ? 1 : 0;
            if (ocerz_init_tolerant || forcetol) {
                fprintf(stderr,
                        "ocerz: init-skip: initializer %#llx faulted at rip=%#llx (skipped, process continues)\n",
                        (unsigned long long)func, (unsigned long long)local.rip);
                break;
            }

            {
                fprintf(stderr, "ocerz: initabort-bt rip=%#llx", (unsigned long long)local.rip);
                uint64_t fp = local.gpr[OCERZ_RBP];
                for (int d = 0; d < 24 && fp >= 0x10000; d++) {
                    uint64_t ra = ocerz_addr_readable(fp + 8)
                                ? ocerz_ld(fp + 8, 8) : 0;
                    fprintf(stderr, " %#llx", (unsigned long long)ra);
                    uint64_t nf = ocerz_addr_readable(fp)
                                ? ocerz_ld(fp, 8) : 0;
                    if (nf <= fp) break;
                    fp = nf;
                }
                fprintf(stderr, "\n");
            }
            ocerz_cpu_dump(&local, stderr);
            OCERZ_FATAL("initializer call to %#llx aborted after %llu instructions\n",
                        (unsigned long long)func, (unsigned long long)vm->insn_count);
            _exit(125);
        }
    }
    g_sig_recover = prev_recover;
    if (prev_cpu && vm->jit_ordered_required)
        __atomic_store_n(&prev_cpu->ras_top, 0, __ATOMIC_RELEASE);
    g_cur_cpu = prev_cpu;
    return local.gpr[OCERZ_RAX];
}

/* Host-signal kick: a no-op handler whose return is a context-synchronization
 * event on the target core, so a thread spinning in JIT code observes the
 * stop-site patches made by ocerz_jit_request_stop. */
static void ocerz_kick_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
}
static void ocerz_install_kick_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ocerz_kick_handler;
    /* deliberately NOT SA_RESTART: the unstick monitor uses this signal to
     * EINTR guest threads out of lost-wakeup parks (the manual `sample`
     * "shake" that always revived wedged wine sessions, automated).  Every
     * guest-visible blocking call handles EINTR (wine loops on it). */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGEMT, &sa, NULL);
}

/* ---- unstick monitor -------------------------------------------------
 * UPDATE #39 family: a guest thread parks in a blocking host wait whose
 * wakeup was lost (waiter/waker alias, kevent edge, ...); an EINTR shake
 * always revives the session.  Automate the shake: kick any cpu thread
 * that has been inside one blocking host call for >800ms.  Legitimate
 * long waits just retry (EINTR is part of every wait's contract). */
static void *ocerz_unstick_thread(void *arg)
{
    (void)arg;
    static int lg = -1;
    if (lg < 0) lg = getenv("OCERZ_UNSTICKLOG") ? 1 : 0;
    static int wauto = -1;
    static uint64_t wbase;
    if (wauto < 0) {
        const char *w = getenv("OCERZ_WATCH");
        const char *b = getenv("OCERZ_MACDRVDUMP");
        wauto = (w && strcmp(w, "auto") == 0 && b) ? 1 : 0;
        wbase = b ? strtoull(b, NULL, 0) : 0;
    }
    for (;;) {
        struct timespec ts = { 0, 250 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (wauto == 1) {
            /* auto-resolve the winemac requestSource signal word:
             * base+0x560f0 -> controller; controller+8 -> source; +0x58. */
            uint64_t slot = wbase + 0x560f0;
            if (ocerz_addr_readable(slot)) {
                uint64_t ctrl = ocerz_ld(slot, 8);
                if (ctrl && ocerz_addr_readable(ctrl + 0x10)) {
                    uint64_t src = ocerz_ld(ctrl + 0x10, 8);   /* requests array */
                    if (src && ocerz_addr_readable(src + 0x30)) {
                        ocerz_watch_addr = src + 0x10;         /* storage/cap-head/count-mut */
                        ocerz_watch_len = 0x20;
                        fprintf(stderr, "ocerz: WATCH-AUTO[%d] resolved %#llx\n",
                                (int)getpid(), (unsigned long long)ocerz_watch_addr);
                        wauto = 2;
                    }
                }
            }
        }
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        pthread_mutex_lock(&g_cpus_lock);
        for (int i = 0; i < g_cpus_n; i++) {
            uint64_t t0 = g_cpus[i]->block_since_ns;
            if (t0 && now - t0 > 800ull * 1000 * 1000) {
                g_cpus[i]->block_since_ns = now;   /* re-arm: kick again in 800ms if still stuck */
                if (lg)
                    fprintf(stderr, "ocerz: UNSTICK[%d] kicking cpu#%u (blocked %llums) what=%d rip=%#llx\n",
                            (int)getpid(), g_cpus[i]->cpu_number,
                            (unsigned long long)((now - t0) / 1000000),
                            g_cpus[i]->block_what,
                            (unsigned long long)g_cpus[i]->rip);
                pthread_kill(g_cpu_threads[i], SIGEMT);
            }
            uint64_t bs = g_cpus[i]->block_started_ns;
            static uint64_t warned[OCERZ_MAX_CPUS];
            /* `now` is sampled before the scan, so a cpu that enters a
             * syscall mid-scan has bs > now: unsigned wrap made every such
             * thread look blocked for 2^64ns. */
            if (bs && now > bs && now - bs > 5000000000ull && warned[i] != bs) {
                warned[i] = bs;
                fprintf(stderr, "ocerz: BLOCKED[%d] cpu#%u trap=%d for %llus rip=%#llx a0=%#llx a1=%#llx a2=%#llx",
                        (int)getpid(), g_cpus[i]->cpu_number, g_cpus[i]->block_what,
                        (unsigned long long)((now - bs) / 1000000000ull),
                        (unsigned long long)g_cpus[i]->rip,
                        (unsigned long long)g_cpus[i]->gpr[OCERZ_RDI],
                        (unsigned long long)g_cpus[i]->gpr[OCERZ_RSI],
                        (unsigned long long)g_cpus[i]->gpr[OCERZ_RDX]);
                /* Guest caller chain.  rbp is only a frame pointer by
                 * convention, so every link is untrusted: require 8-byte
                 * alignment and READABLE (committed alone is true for the
                 * PROT_NONE identity reservations, where a load is a SIGBUS
                 * this thread cannot attribute - it has no cpu). */
                if (getenv("OCERZ_BLOCKBT")) {
                    uint64_t sp = g_cpus[i]->gpr[OCERZ_RSP], fp = g_cpus[i]->gpr[5];
                    if (sp && !(sp & 7) && ocerz_addr_readable(sp))
                        fprintf(stderr, " bt=%#llx", (unsigned long long)ocerz_ld(sp, 8));
                    for (int fj = 0; fj < 8 && fp && !(fp & 7) &&
                                     ocerz_addr_readable(fp) && ocerz_addr_readable(fp + 8); fj++) {
                        fprintf(stderr, ",%#llx", (unsigned long long)ocerz_ld(fp + 8, 8));
                        fp = ocerz_ld(fp, 8);
                    }
                }
                fputc('\n', stderr);
            }
        }
        {   /* OCERZ_BTRACE freeze latch: a cpu that has entered NO guest block
             * for 20 polls (5 s) while others keep running is the stuck thread.
             * Storing mask 0 stops every ring so the last 64K block entries
             * survive, then dump them.  This is the only instrument that can
             * see control flow which never leaves the code arena. */
            static unsigned bt_quiet[OCERZ_MAX_CPUS];
            static int bt_latched;
            if (!bt_latched && g_cpus_n > 0 && g_cpus[0]->btrace) {
                if (__atomic_load_n(&g_btrace_req, __ATOMIC_ACQUIRE)) {
                    bt_latched = 1;
                    for (int i = 0; i < g_cpus_n; i++)
                        __atomic_store_n(&g_cpus[i]->btrace_mask, 0u, __ATOMIC_RELEASE);
                    const char *e = getenv("OCERZ_BTRACE_DEPTH");
                    uint32_t depth = e ? (uint32_t)strtoul(e, NULL, 0) : 400;
                    for (int i = 0; i < g_cpus_n; i++) {
                        OcerzCPU *c = g_cpus[i];
                        if (!c->btrace) continue;
                        uint32_t bn = c->btrace_n, m = (1u << 16) - 1;
                        fprintf(stderr, "ocerz: BTRACE[%d] cpu#%u tid=%#llx quiet=%u n=%u\n",
                                (int)getpid(), c->cpu_number,
                                (unsigned long long)(ocerz_addr_readable(c->gs_base + 0x18)
                                                     ? ocerz_ld(c->gs_base + 0x18, 8) : 0),
                                bt_quiet[i], bn);
                        for (uint32_t k = 1; k <= depth && k <= bn; k++)
                            fprintf(stderr, "  %u %#llx\n", k,
                                    (unsigned long long)c->btrace[(bn - k) & m]);
                    }
                    fflush(stderr);
                }
            }
        }
        pthread_mutex_unlock(&g_cpus_lock);
    }
    return NULL;
}
void ocerz_unstick_start(void)
{
    if (getenv("OCERZ_NO_UNSTICK"))
        return;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_unstick_started, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    pthread_t t;
    if (pthread_create(&t, NULL, ocerz_unstick_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        __atomic_store_n(&g_unstick_started, 0, __ATOMIC_RELEASE);
    }
}

void ocerz_vm_request_exit(OcerzVM *vm, int code)
{
    vm->exit_code = code;
    __atomic_store_n(&vm->exited, 1, __ATOMIC_SEQ_CST);

    ocerz_jit_request_stop(vm);

    pthread_t self = pthread_self();
    pthread_mutex_lock(&g_cpus_lock);
    for (int i = 0; i < g_cpus_n; i++)
        __atomic_store_n(&g_cpus[i]->interrupt, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < g_cpus_n; i++)
        if (!pthread_equal(g_cpu_threads[i], self))
            pthread_kill(g_cpu_threads[i], SIGEMT);
    pthread_mutex_unlock(&g_cpus_lock);
}

int ocerz_vm_run_cpu(OcerzVM *vm, OcerzCPU *cpu)
{
    const char *tlo = getenv("OCERZ_TRACE_LO");
    const char *thi = getenv("OCERZ_TRACE_HI");
    uint64_t trace_lo = tlo ? strtoull(tlo, NULL, 0) : 0;
    uint64_t trace_hi = thi ? strtoull(thi, NULL, 0) : 0;
    sigjmp_buf jb;
    OcerzCPU *prev_cpu = g_cur_cpu;
    sigjmp_buf *prev_recover = g_sig_recover;
    g_sig_recover = &jb;

    ocerz_cpu_register(cpu);
    if (sigsetjmp(jb, 1) != 0 && getenv("OCERZ_CPUREG_LOG")) {

        static _Atomic unsigned recov;
        fprintf(stderr, "ocerz: CPUREG recovery #%u (old code would leak a dangling entry here)\n",
                ++recov);
    }
    g_cur_cpu = cpu;

    while (!vm->exited && !cpu->terminated && !cpu->interrupt) {
        g_riphist[g_riphist_n++ & 31] = cpu->rip;
        int r;
        if (ocerz_exc_trap && cpu->rip == ocerz_exc_trap)
            ocerz_exc_report(cpu);
        if (ocerz_arg_trap && cpu->rip == ocerz_arg_trap)
            arg_trap_report(cpu);
        if (ocerz_sel_trap && cpu->rip == ocerz_sel_trap)
            sel_trap_report(cpu);
        if (ocerz_ctx_trap && cpu->rip == ocerz_ctx_trap)
            ctx_trap_report(cpu);
        if (ocerz_bt_lo && cpu->rip >= ocerz_bt_lo && cpu->rip < ocerz_bt_hi)
            ocerz_bt_report(cpu);
        if (trace_lo && cpu->rip >= trace_lo && cpu->rip < trace_hi) {
            fprintf(stderr, "WT %#llx rax=%#llx rdi=%#llx rsi=%#llx r8=%#llx r12=%#llx\n",
                    (unsigned long long)cpu->rip, (unsigned long long)cpu->gpr[OCERZ_RAX],
                    (unsigned long long)cpu->gpr[OCERZ_RDI], (unsigned long long)cpu->gpr[OCERZ_RSI],
                    (unsigned long long)cpu->gpr[OCERZ_R8], (unsigned long long)cpu->gpr[OCERZ_R12]);
            r = ocerz_interp_step(vm, cpu);
            if (r == OCERZ_STEP_EXIT)
                break;
            if (r == OCERZ_STEP_FATAL)
                goto fatal;
            continue;
        }
        if (cpu->interp_once) {
            cpu->interp_once = 0;
            r = ocerz_interp_step(vm, cpu);
        } else if (vm->jit_enabled && (vm->jit || (vm->jit = ocerz_jit_create(vm)))) {
            r = ocerz_jit_step(vm, cpu);
            if (r == OCERZ_EUNSUP)
                r = ocerz_interp_step(vm, cpu);
        } else {
            r = ocerz_interp_step(vm, cpu);
        }
        if (r == OCERZ_STEP_EXIT)
            break;
        if (r == OCERZ_STEP_FATAL) {
        fatal:
            ocerz_cpu_dump(cpu, stderr);
            uint64_t fp = cpu->gpr[OCERZ_RBP];
            fprintf(stderr, "ocerz: rbp-chain:");
            for (int d = 0; d < 40 && fp >= 0x300000000ull; d++) {
                fprintf(stderr, " %#llx", (unsigned long long)ocerz_ld(fp + 8, 8));
                uint64_t nf = ocerz_ld(fp, 8);
                if (nf <= fp) break;
                fp = nf;
            }
            fprintf(stderr, "\nocerz: %llu instructions executed\n",
                    (unsigned long long)vm->insn_count);
            /* A fatal in 32-bit guest code usually lands on a WoW64 thread,
             * and leaving the process half-alive wedges the guest's parent,
             * which waits for the child forever.  Die as a process so wine can
             * move on.  (This test used to mean "a 32-bit mode entry, which we
             * could not execute, just happened"; now that the interpreter does
             * execute 32-bit code, cpu->mode32 means the thread was IN 32-bit
             * code when it died, and the same reasoning applies.) */
            if (cpu->mode32)
                exit(125);
            g_sig_recover = prev_recover;
            g_cur_cpu = prev_cpu;
            ocerz_cpu_unregister(cpu);
            return 125;
        }
    }
    g_sig_recover = prev_recover;
    g_cur_cpu = prev_cpu;
    ocerz_cpu_unregister(cpu);
    return vm->exit_code;
}

static void *ocerz_test_async_stop(void *p)
{
    OcerzVM *vm = (OcerzVM *)p;
    const char *icn = getenv("OCERZ_TEST_ASYNC_STOP_ICOUNT");
    uint64_t thresh = icn ? strtoull(icn, NULL, 0) : 500000;
    while (!__atomic_load_n(&vm->exited, __ATOMIC_RELAXED) &&
           __atomic_load_n(&vm->insn_count, __ATOMIC_RELAXED) < thresh)
        sched_yield();
    ocerz_vm_request_exit(vm, 0);
    return NULL;
}

static void *ocerz_test_async_stop_ms(void *p)
{
    OcerzVM *vm = (OcerzVM *)p;
    const char *msn = getenv("OCERZ_TEST_ASYNC_STOP_MS");
    long ms = msn ? strtol(msn, NULL, 0) : 200;
    if (ms < 0)
        ms = 0;
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    struct timespec rem;
    while (nanosleep(&req, &rem) != 0 && errno == EINTR)
        req = rem;
    ocerz_vm_request_exit(vm, 0);
    return NULL;
}

int ocerz_vm_run(OcerzVM *vm)
{
    ocerz_vm_install_handlers(vm);

    if (getenv("OCERZ_TEST_ASYNC_STOP_ICOUNT")) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &at, ocerz_test_async_stop, vm);
        pthread_attr_destroy(&at);
    } else if (getenv("OCERZ_TEST_ASYNC_STOP_MS")) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &at, ocerz_test_async_stop_ms, vm);
        pthread_attr_destroy(&at);
    }
    int rc = ocerz_vm_run_cpu(vm, &vm->cpu);
    OCERZ_LOG("guest exited with code %d after %llu instructions\n",
              vm->exit_code, (unsigned long long)vm->insn_count);
    if (vm->jit)
        OCERZ_LOG("jit translated %llu blocks\n",
                  (unsigned long long)ocerz_jit_blocks(vm->jit));
    return rc;
}
