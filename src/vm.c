/*
 * src/vm.c
 *
 * The master run loop and crash containment.
 *
 * ocerz_vm_run() installs SIGSEGV/SIGBUS handlers, then executes the guest:
 * when the JIT tier is enabled and present it gets first claim on each
 * dispatch (returning OCERZ_EUNSUP hands control back for one interpreted
 * instruction, which is also how unimplemented instructions inside a hot
 * region make progress); otherwise the interpreter single-steps. The loop
 * ends when the guest calls exit (STEP_EXIT, exit code preserved) or when
 * emulation cannot continue (STEP_FATAL, exit code 125 after a CPU dump).
 *
 * The signal handler is async-signal-safe by construction (write(2) of a
 * preformatted buffer built with a tiny hex formatter, then _exit(139)).
 * It reports the guest rip at the time of the fault and the faulting host
 * address, the two numbers that make wild-pointer bugs in guest code (or
 * Ocerz itself) immediately diagnosable. g_vm makes the current VM visible
 * to the handler; Ocerz is single-guest-per-process so a global is
 * accurate.
 *
 * Two env-gated diagnostics live here because they need the VM state.
 * OCERZ_ICAP=<n> aborts an initializer-phase ocerz_vm_call once the global
 * instruction count exceeds n, dumping the CPU — bisecting n against a spin
 * locates the looping rip without a 100M-line trace. OCERZ_WATCH=<addr>
 * (checked in the ocerz_st/ocerz_st128 inlines, mem.h) reports every guest
 * store covering that address with value, rip, and icount via
 * ocerz_watch_hit — a software store-watchpoint that pinpoints which
 * instruction last set a corrupted global. OCERZ_RIPLOG=<addr[,addr...]>
 * logs the first few times an ocerz_vm_call loop iteration begins at one of
 * the listed guest rips (with icount and rdi/rsi) — an execution-tripwire for
 * confirming whether a specific IMP or init routine actually runs, complementary
 * to OCERZ_WATCH's store view (it sees block-entry rips, so JIT-chained
 * interiors can be missed). OCERZ_PROFILE=<n> prints the current rip every n
 * (interpreted) instructions — a coarse statistical profiler that tells a
 * confined busy-loop (rip clustered in one region) apart from slow forward
 * progress (rip sweeping many functions/dylibs); note only interpreted steps
 * advance the count, so JIT-resident regions are under-sampled. OCERZ_TRACE_LO/
 * OCERZ_TRACE_HI=<addr> bound a guest-rip window inside which ocerz_vm_run_cpu
 * single-steps (forcing the interpreter) and prints rip+key regs per
 * instruction — a SCOPED, per-thread trace. Because under OCERZ_INITPHASE the
 * main guest runs via ocerz_vm_call while spawned/worker threads run via
 * ocerz_vm_run_cpu, this window traces ONLY the worker threads, untangling the
 * libsystem_pthread code that the main thread also executes.
 *
 * The ocerz_vm_call return sentinel is a real mapped page (int3-filled,
 * readable, at 0x500000000 — outside both the guest arena and the shared
 * cache), not a bare magic constant. Guest code is entitled to READ the
 * bytes at its own return address: objc's autorelease-return optimization
 * (objc_autoreleaseReturnValue) does `mov rax,[rsp]; cmp dword [rax],
 * 0xe8c78948` to probe for the reclaim marker, so an unmapped fake return
 * address SIGSEGVs the first time a +load or initializer returns an
 * autoreleased object. With a mapped page the probe reads 0xcc bytes,
 * finds no marker, and correctly falls back to the plain autorelease path;
 * if anything ever actually jumps there the int3 traps loudly instead of
 * executing garbage. The crash handler guards against re-entry (its own
 * guest-stack backtrace scan can fault on a corrupt sp, which previously
 * recursed the signal forever) and writes the fault summary before
 * attempting that scan. For that guard to actually fire the handler is
 * installed with SA_NODEFER so a fault taken WHILE the handler runs (the
 * bt-scan's ocerz_ld off the just-overflowed, now-unmapped guest stack page)
 * is delivered as a nested signal instead of being masked and retried forever
 * by the kernel — without SA_NODEFER the re-fault spins the thread and the
 * process hangs instead of dying. It is also installed SA_ONSTACK on a
 * dedicated sigaltstack so it can still run when the guest/host stack itself is
 * the unmapped region. The handler always terminates the process (_exit(139),
 * or _exit(139) via the depth guard on the nested fault).
 */
#include "ocerz/vm.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"

#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>

#define OCERZ_CALL_SENTINEL 0x00000000deadca11ull

static OcerzVM *g_vm;
static __thread OcerzCPU *g_cur_cpu;
/* When a guest CPU fault is converted to a guest signal, the host SIGSEGV/SIGBUS
 * handler redirects g_cur_cpu to the guest handler and unwinds the interrupted
 * interpreter/JIT back to the nearest run-loop step via this per-thread jump
 * buffer (set at the top of ocerz_vm_run_cpu / ocerz_vm_call, saved/restored
 * across nesting). The loop then resumes from the redirected cpu->rip. */
static __thread sigjmp_buf *g_sig_recover;
/* Max consecutive deliveries of the SAME guest fault address before the handler
 * is declared stuck (the page is genuinely bad, not commit-on-demand). */
#define OCERZ_SIG_MAX_REPEAT 16
/* Ring of recent block-entry rips per thread, for reconstructing how a thread
 * reached a fault (e.g. a branch to rip=0). Updated at each run-loop step. */
static __thread uint64_t g_riphist[32];
static __thread unsigned g_riphist_n;
static int g_crash_stack;
/* OCERZ_SIGTRACE: when set, crash_handler writes one async-signal-safe line per
 * converted guest signal (fault address, the handler/trampoline it redirects to,
 * the faulting guest rip and icount). Read once at handler-install time so the
 * signal handler never calls getenv. */
static int g_sigtrace;

uint64_t ocerz_watch_addr;
uint64_t ocerz_watch_val;
uint64_t ocerz_exc_trap;
/* OCERZ_CTXTRAP=<rip>: when a run-loop step begins at this guest rip, dump the
 * Wine syscall_frame pointed to by rcx (rax@0, rcx@0x10, rdx@0x18, rdi@0x28,
 * rip@0x70, rsp@0x88) — used to inspect the context that NtContinue/dispatcher-
 * return is about to restore (e.g. tracking down a frame with rip=0). */
static uint64_t ocerz_ctx_trap;

static void ctx_trap_report(const OcerzCPU *c)
{
    static int hits;
    if (hits >= 12)
        return;
    hits++;
    uint64_t f = c->gpr[OCERZ_RCX];
    fprintf(stderr,
            "ocerz: CTXTRAP pid=%d rip=%#llx frame=%#llx f.rip=%#llx f.rsp=%#llx "
            "f.rax=%#llx f.rcx=%#llx f.rdx=%#llx f.rdi=%#llx gs=%#llx icount=%#llx\n",
            getpid(), (unsigned long long)c->rip, (unsigned long long)f,
            (unsigned long long)ocerz_ld(f + 0x70, 8),
            (unsigned long long)ocerz_ld(f + 0x88, 8),
            (unsigned long long)ocerz_ld(f + 0x00, 8),
            (unsigned long long)ocerz_ld(f + 0x10, 8),
            (unsigned long long)ocerz_ld(f + 0x18, 8),
            (unsigned long long)ocerz_ld(f + 0x28, 8),
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

void ocerz_watch_hit(uint64_t gaddr, int size, uint64_t lo, uint64_t hi)
{
    OcerzCPU *c = g_cur_cpu ? g_cur_cpu : (g_vm ? &g_vm->cpu : NULL);
    fprintf(stderr, "ocerz: WATCH st [%#llx] size=%d val=%#llx:%#llx rip=%#llx icount=%llu"
            " rdi=%#llx rsi=%#llx rax=%#llx rbx=%#llx r14=%#llx\n",
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

static void crash_handler(int sig, siginfo_t *si, void *ctx)
{
    static volatile int depth;
    /* A fault inside the JIT block translator's guest-code read: unwind back
     * into translate() (which ends the block and lets jit_step release the
     * translation lock normally) rather than delivering a guest signal here,
     * which would longjmp out through the held lock and deadlock. This must
     * precede the delivery path. See ocerz_jit_decode_recover in jit.h. */
    if (ocerz_jit_decode_recover)
        siglongjmp(*ocerz_jit_decode_recover, 1);
    /* A guest CPU fault: if the running guest thread has a handler registered,
     * convert the fault to a Darwin guest-signal delivery and unwind back to the
     * run loop, which resumes at the handler. A translated guest memory-access
     * fault is normalized to guest SIGSEGV regardless of whether the arm64 host
     * raised SIGSEGV or SIGBUS: on real x86_64 Darwin a bad access is SIGSEGV,
     * and the host SIGBUS here is an artifact of how the shadow window is mapped.
     * The si_code distinguishes a not-present page (SEGV_MAPERR) from a
     * permission fault on a committed page (SEGV_ACCERR). depth guards the frame
     * build itself so a fault while writing the frame falls through to the real
     * crash dump rather than looping. */
    if (depth == 0 && g_cur_cpu && g_sig_recover &&
        ocerz_host_in_guest_space(si->si_addr)) {
        depth = 1;
        uint64_t fault_rip = g_cur_cpu->rip;
        uint64_t gs = g_cur_cpu->gs_base;
        uint64_t gaddr = ocerz_h2g(si->si_addr);
        int code = ocerz_addr_committed(gaddr) == 1 ? 2 : 1;
        /* Build the x86 page-fault error code Wine reads from mcontext.__es to
         * classify the fault (read/write/execute, present/not). Recover the true
         * access type from the host arm64 ESR: instruction-abort exception class
         * => instruction fetch; data-abort WnR bit => write. Without this Wine
         * sees every fault as a not-present READ and mis-handles execute faults
         * (a jump to 0) and write/guard-page faults (stack growth). */
        uint64_t esr = ctx ? ((const ucontext_t *)ctx)->uc_mcontext->__es.__esr : 0;
        uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);
        int is_fetch = (ec == 0x20 || ec == 0x21);
        int is_write = !is_fetch && (esr & (1u << 6)) != 0;
        uint32_t err = 0x4u;
        if (code == 2) err |= 0x1u;
        if (is_write) err |= 0x2u;
        if (is_fetch) err |= 0x10u;
        /* A guest handler that keeps faulting on the SAME address makes no
         * progress (the page is genuinely bad, not a commit-on-demand miss);
         * after a bound, stop delivering and let the real crash dump show the
         * stuck address instead of recursing the handler to a stack overflow. A
         * fault at a DIFFERENT address resets the counter, so legitimately
         * nested or sequential faults (incl. Wine's NtContinue returns, which do
         * not sigreturn) are unaffected. */
        if (gaddr != g_cur_cpu->sig_last_fault) {
            g_cur_cpu->sig_last_fault = gaddr;
            g_cur_cpu->sig_repeat = 0;
        }
        int looping = ++g_cur_cpu->sig_repeat > OCERZ_SIG_MAX_REPEAT;
        int delivered = looping ? 0
                       : ocerz_signal_deliver(g_cur_cpu, SIGSEGV, gaddr, code, err);
        if (g_sigtrace) {
            char tb[256];
            char *t = tb;
            t = str_into(t, delivered ? "ocerz: SIG deliver addr=" : "ocerz: SIG nohandler addr=");
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
        }
        depth = 0;
        if (delivered)
            siglongjmp(*g_sig_recover, 1);
    }
    char buf[256];
    char *p = buf;
    if (depth++) {
        p = str_into(p, "ocerz: nested fault inside crash handler\n");
        write(2, buf, (size_t)(p - buf));
        _exit(139);
    }
    p = str_into(p, "ocerz: guest crash: ");
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
            int comm = ocerz_addr_committed(ocerz_h2g(si->si_addr));
            const char *cs = comm == 1 ? "  fault-page: COMMITTED"
                           : comm == 0 ? "  fault-page: UNCOMMITTED"
                                       : "  fault-page: outside-arena";
            p = buf;
            p = str_into(p, cs);
            p = str_into(p, sig == SIGBUS
                ? (si->si_code == 1 ? " si_code=ADRALN\n"
                 : si->si_code == 2 ? " si_code=ADRERR\n"
                 : si->si_code == 3 ? " si_code=OBJERR\n" : " si_code=?\n")
                : "\n");
            write(2, buf, (size_t)(p - buf));
        }
        uint64_t fp = c->gpr[OCERZ_RBP];
        p = buf;
        p = str_into(p, "  rbp-chain:");
        for (int d = 0; d < 9 && fp >= 0x300000000ull; d++) {
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
                if (ocerz_addr_committed(a) != 0)
                    p = hex_into(p, ocerz_ld(a, 8));
                else
                    p = str_into(p, "uncommitted");
            }
            p = str_into(p, "\n");
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
    memset(vm, 0, sizeof *vm);
    vm->cpu.vm = vm;
    ocerz_cpu_reset(&vm->cpu);
    vm->jit_enabled = 1;
    return OCERZ_OK;
}

void ocerz_vm_install_handlers(OcerzVM *vm)
{
    extern int ocerz_cftrap_on;
    ocerz_cftrap_on = getenv("OCERZ_CFTRAP") != NULL;
    g_crash_stack = getenv("OCERZ_CRASH_STACK") != NULL;
    g_sigtrace = getenv("OCERZ_SIGTRACE") != NULL;
    const char *w = getenv("OCERZ_WATCH");
    if (w)
        ocerz_watch_addr = strtoull(w, NULL, 0);
    const char *wv = getenv("OCERZ_STVAL");
    if (wv)
        ocerz_watch_val = strtoull(wv, NULL, 0);
    const char *et = getenv("OCERZ_EXCTRAP");
    if (et)
        ocerz_exc_trap = strtoull(et, NULL, 0);
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
    g_vm = vm;
    if (vm->jit_enabled && !vm->jit)
        vm->jit = ocerz_jit_create(vm);
}

/* ocerz_vm_call runs the guest function on a CALL-LOCAL OcerzCPU seeded from
 * the calling thread's current guest context (g_cur_cpu when set -- a worker or
 * a nested call -- else the main cpu template). It must NOT step vm->cpu
 * directly: guest callbacks are invoked from WORKER threads too (the dyldapi
 * objc callouts fire tens of thousands of times under load), and a worker
 * resetting/stepping the main thread's live register file while main runs or
 * sleeps in a blocked syscall derails both threads -- and since every call
 * shares one sentinel rip, main's loop would also return early the moment a
 * worker's nested call finished. The local cpu inherits the caller's gs_base
 * (thread identity/TSD/errno) and runs on the caller-provided guest stack. */
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
    sigjmp_buf jb;
    sigjmp_buf *prev_recover = g_sig_recover;
    g_sig_recover = &jb;
    sigsetjmp(jb, 1);
    g_cur_cpu = &local;
    while (local.rip != sentinel && !vm->exited) {
        g_riphist[g_riphist_n++ & 31] = local.rip;
        int r;
        if (ocerz_exc_trap && local.rip == ocerz_exc_trap)
            ocerz_exc_report(&local);
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
        if (mtrace_lo && local.rip >= mtrace_lo && local.rip < mtrace_hi) {
            fprintf(stderr, "MT %#llx rax=%#llx rdi=%#llx rsi=%#llx rsp=%#llx [rsp]=%#llx\n",
                    (unsigned long long)local.rip,
                    (unsigned long long)local.gpr[OCERZ_RAX],
                    (unsigned long long)local.gpr[OCERZ_RDI],
                    (unsigned long long)local.gpr[OCERZ_RSI],
                    (unsigned long long)local.gpr[OCERZ_RSP],
                    (unsigned long long)ocerz_ld(local.gpr[OCERZ_RSP], 8));
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
            ocerz_cpu_dump(&local, stderr);
            OCERZ_FATAL("initializer call to %#llx aborted after %llu instructions\n",
                        (unsigned long long)func, (unsigned long long)vm->insn_count);
            _exit(125);
        }
    }
    g_sig_recover = prev_recover;
    g_cur_cpu = prev_cpu;
    return local.gpr[OCERZ_RAX];
}

void ocerz_vm_request_exit(OcerzVM *vm, int code)
{
    vm->exited = 1;
    vm->exit_code = code;
}

int ocerz_vm_run_cpu(OcerzVM *vm, OcerzCPU *cpu)
{
    const char *tlo = getenv("OCERZ_TRACE_LO");
    const char *thi = getenv("OCERZ_TRACE_HI");
    uint64_t trace_lo = tlo ? strtoull(tlo, NULL, 0) : 0;
    uint64_t trace_hi = thi ? strtoull(thi, NULL, 0) : 0;
    sigjmp_buf jb;
    sigjmp_buf *prev_recover = g_sig_recover;
    g_sig_recover = &jb;
    sigsetjmp(jb, 1);
    g_cur_cpu = cpu;
    while (!vm->exited && !cpu->terminated) {
        g_riphist[g_riphist_n++ & 31] = cpu->rip;
        int r;
        if (ocerz_exc_trap && cpu->rip == ocerz_exc_trap)
            ocerz_exc_report(cpu);
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
            g_sig_recover = prev_recover;
            return 125;
        }
    }
    g_sig_recover = prev_recover;
    return vm->exit_code;
}

int ocerz_vm_run(OcerzVM *vm)
{
    ocerz_vm_install_handlers(vm);
    int rc = ocerz_vm_run_cpu(vm, &vm->cpu);
    OCERZ_LOG("guest exited with code %d after %llu instructions\n",
              vm->exit_code, (unsigned long long)vm->insn_count);
    if (vm->jit)
        OCERZ_LOG("jit translated %llu blocks\n",
                  (unsigned long long)ocerz_jit_blocks(vm->jit));
    return rc;
}
