/* The master run loop and crash containment. */
#include "ocerz/vm.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/flags.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"
#include "ocerz/dyldapi.h"

#include <signal.h>
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

#define OCERZ_CALL_SENTINEL 0x00000000deadca11ull

static OcerzVM *g_vm;
static __thread OcerzCPU *g_cur_cpu;

#define OCERZ_MAX_CPUS 512
static uint64_t g_image_slide;
static OcerzCPU *g_cpus[OCERZ_MAX_CPUS];
static pthread_t g_cpu_threads[OCERZ_MAX_CPUS];
static int g_cpus_n;
static pthread_mutex_t g_cpus_lock = PTHREAD_MUTEX_INITIALIZER;
static OcerzCPU *g_fork_surviving_cpu;

static void ocerz_cpu_register(OcerzCPU *cpu)
{
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

void ocerz_vm_purge_jit_ras(OcerzVM *vm)
{
    if (!vm)
        return;
    __atomic_store_n(&vm->cpu.ras_top, 0, __ATOMIC_RELEASE);
    if (g_cur_cpu && g_cur_cpu->vm == vm)
        __atomic_store_n(&g_cur_cpu->ras_top, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&g_cpus_lock);
    for (int i = 0; i < g_cpus_n; i++)
        if (g_cpus[i] && g_cpus[i]->vm == vm)
            __atomic_store_n(&g_cpus[i]->ras_top, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_cpus_lock);
}

static __thread volatile uint32_t g_pending_async_mask;

uint32_t ocerz_take_pending_async_sig(void)
{
    return __atomic_exchange_n(&g_pending_async_mask, 0u, __ATOMIC_SEQ_CST);
}

static __thread sigjmp_buf *g_sig_recover;

#define OCERZ_SIG_MAX_REPEAT 16

static __thread uint64_t g_riphist[32];
static __thread unsigned g_riphist_n;
static int g_crash_stack;

void ocerz_vm_atfork_prepare(void)
{
    pthread_t self = pthread_self();

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
    g_riphist_n = 0;
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
    fprintf(stderr, "ocerz: ARGTRAP rip=%#llx", (unsigned long long)c->rip);
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
    default:
        return;  /* SEGV/BUS/USR1/USR2/QUIT/ILL/TRAP/FPE/ABRT: ocerz owns these */
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    if (kind == 1)
        sa.sa_handler = SIG_IGN;
    else if (kind == 2) {
        sa.sa_sigaction = async_sig_handler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
    } else
        sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
}

static void ripdump_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si; (void)ctx;
    if (!g_cur_cpu)
        return;
    char b[640];
    char *p = b;
    p = str_into(p, "ocerz: RIPDUMP rip=");
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

    if (ocerz_jit_decode_recover)
        siglongjmp(*ocerz_jit_decode_recover, 1);

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
        if (delivered)
            siglongjmp(*g_sig_recover, 1);
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
                    char tb[256];
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
                    t = str_into(t, "\n");
                    write(2, tb, (size_t)(t - tb));
                }
            }
            g_cur_cpu->terminated = 1;
            siglongjmp(*g_sig_recover, 1);
        }
    }
    char buf[256];
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
    p = str_into(p, sig == SIGBUS ? "SIGBUS" : "SIGSEGV");
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
    }
    OcerzCPU *c = g_cur_cpu ? g_cur_cpu : (g_vm ? &g_vm->cpu : NULL);
    if (c) {
        p = str_into(p, " guest_rip=");
        p = hex_into(p, c->rip);
        p = str_into(p, " guest_addr=");
        p = hex_into(p, ocerz_h2g(si->si_addr));
        p = str_into(p, " icount=");
        p = hex_into(p, g_vm ? g_vm->insn_count : 0);
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

void ocerz_vm_install_handlers(OcerzVM *vm)
{
    extern int ocerz_cftrap_on;
    ocerz_cftrap_on = getenv("OCERZ_CFTRAP") != NULL;
    g_crash_stack = getenv("OCERZ_CRASH_STACK") != NULL;
    g_sigtrace = getenv("OCERZ_SIGTRACE") != NULL;
    g_winefaultlog = getenv("OCERZ_WINEFAULTLOG") != NULL;
    const char *w = getenv("OCERZ_WATCH");
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

        if (mtrace_hit) {
            r = ocerz_interp_step(vm, &local);
        } else if (vm->jit_enabled && vm->jit) {
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

void ocerz_vm_request_exit(OcerzVM *vm, int code)
{
    vm->exit_code = code;
    __atomic_store_n(&vm->exited, 1, __ATOMIC_SEQ_CST);

    ocerz_jit_request_stop(vm);

    pthread_mutex_lock(&g_cpus_lock);
    for (int i = 0; i < g_cpus_n; i++)
        __atomic_store_n(&g_cpus[i]->interrupt, 1, __ATOMIC_SEQ_CST);
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
        if (vm->jit_enabled && vm->jit) {
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
            /* A 32-bit mode entry can strike on a secondary thread; leaving
             * the process half-alive wedges the guest's parent, which waits
             * for the child forever.  Die as a process so wine can move on. */
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
