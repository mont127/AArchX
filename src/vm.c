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
#include "ocerz/dyldapi.h"

#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>

#define OCERZ_CALL_SENTINEL 0x00000000deadca11ull

static OcerzVM *g_vm;
static __thread OcerzCPU *g_cur_cpu;

/* Pending ASYNCHRONOUS guest signals for THIS thread (a bitmask of signo, 1..31).
 * The macOS kernel delivers __pthread_kill(thread_port, signo) to a named thread
 * asynchronously, interrupting its current syscall. ocerz reproduces that: the host
 * async_sig_handler (installed for the signals Wine sends cross-thread, e.g. the
 * wineserver's SIGUSR1 system-APC/suspend kick) sets the bit and returns -- EINTRing
 * the target out of its blocking forwarded host syscall -- and the syscall-return
 * boundary in ocerz_handle_syscall builds the guest signal frame via
 * ocerz_signal_deliver. See sys_pthread_kill. */
static __thread volatile uint32_t g_pending_async_mask;

uint32_t ocerz_take_pending_async_sig(void)
{
    return __atomic_exchange_n(&g_pending_async_mask, 0u, __ATOMIC_SEQ_CST);
}
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

unsigned ocerz_vm_riphist(uint64_t *out, unsigned max)
{
    unsigned n = g_riphist_n < 32 ? g_riphist_n : 32;
    if (n > max) n = max;
    for (unsigned i = 0; i < n; i++)
        out[i] = g_riphist[(g_riphist_n - 1 - i) & 31];
    return n;
}
/* OCERZ_SIGTRACE: when set, crash_handler writes one async-signal-safe line per
 * converted guest signal (fault address, the handler/trampoline it redirects to,
 * the faulting guest rip and icount). Read once at handler-install time so the
 * signal handler never calls getenv. */
static int g_sigtrace;

uint64_t ocerz_watch_addr;
uint64_t ocerz_watch_val;
uint64_t ocerz_exc_trap;
/* When >0, a guest CPU fault inside an ocerz_vm_call (a library initializer)
 * is logged and SKIPPED -- the call returns -- instead of aborting the process.
 * Set around shared-cache image initializers (run_image_inits), which must run
 * on dlopen so a post-boot framework (Foundation) is fully initialized, but a
 * few of which (SkyLight's, which asserts a libdispatch queue context ocerz
 * cannot provide -> ud2) cannot run under emulation and are best-effort skipped
 * exactly as the old all-initializers-skipped behavior did, minus the ones we
 * actually need. */
int ocerz_init_tolerant;
/* OCERZ_SELTRAP=<rip>: when a run-loop step begins at this guest rip (point it at
 * objc's __forwarding_prep_0___ / a doesNotRecognizeSelector site), dump the
 * objc_msgSend receiver (rdi) + its isa, the selector pointer (rsi) and its name
 * string, and the thread gs_base -- to tell a non-canonical selector pointer from
 * a corrupt receiver isa behind an "unrecognized selector" abort. */
static uint64_t ocerz_sel_trap;
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
    uint64_t s = c->gpr[OCERZ_RSI];   /* context_from_server: rsi = server context_t */
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

/* Host handler for an async guest signal directed at this thread (the wineserver's
 * cross-thread SIGUSR1/SIGQUIT kick, forwarded by sys_pthread_kill -> the host
 * kernel -> here). Async-signal-safe: just record the signo; returning EINTRs the
 * interrupted blocking host syscall, and the guest handler runs at the next
 * syscall-return boundary (ocerz_handle_syscall). */
static void async_sig_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)si; (void)ctx;
    if (sig > 0 && sig < 32)
        __atomic_or_fetch(&g_pending_async_mask, 1u << sig, __ATOMIC_SEQ_CST);
}

/* OCERZ_RIPDUMP: SIGUSR1 dumps the handling thread's current guest rip and its
 * recent rip history, async-signal-safe, so a spinning guest loop (CFRunLoop /
 * SkyLight / dispatch) can be located by `kill -USR1` without stopping it. */
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
    /* Dump instruction bytes at each distinct recent guest rip so a spinning
     * loop can be disassembled offline. */
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
        if (ocerz_addr_committed(r) != 1) {
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
        /* The rip a real x86 kernel reports is the FAULTING instruction, but
         * cpu->rip has already advanced to the next one (see OcerzCPU::cur_rip).
         * cur_rip carries the faulting address for slow-path instructions; for
         * an INLINED jit instruction nothing wrote it, so recover the exact rip
         * from the block's side table via the host pc. Falls back to cur_rip
         * when the pc is not in a compiled block. */
        uint64_t fault_rip = g_cur_cpu->cur_rip;
        int rip_exact = 1;
        {
            const void *hpc = ctx ? (const void *)(uintptr_t)
                ((const ucontext_t *)ctx)->uc_mcontext->__ss.__pc : NULL;
            struct OcerzVM *fvm = g_cur_cpu->vm;
            if (hpc && fvm && ocerz_jit_pc_in_arena(fvm, hpc)) {
                /* Faulted in emitted code: only the side table knows the rip,
                 * because an inlined instruction never wrote cur_rip (which
                 * therefore still names some earlier slow instruction). If the
                 * table cannot answer, we do NOT know the faulting rip and must
                 * not pretend we do. */
                uint64_t jrip;
                if (ocerz_jit_fault_rip(fvm, hpc, &jrip))
                    fault_rip = jrip;
                else
                    rip_exact = 0;
            }
            /* Faulted outside the arena => we are inside ocerz's own C, i.e. the
             * interpreter slow path, which always sets cur_rip before executing.
             * cur_rip is authoritative there. */
        }
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
        /* Bug B bounded recovery (gs:0x320): a HOSTWQ/libdispatch WORKER has no Wine TEB and no
         * Wine sigaltstack (sig_altstack_sp==0), so when it faults, Wine's segv_handler can't
         * run -- get_current_teb() reads no TEB and the deref faults on [r14]=0 (near-null
         * ~0x320) OR [r14]=garbage; either way the handler makes no progress and re-faults. On
         * real macOS such a worker never faults; under ocerz it does, and the loop would kill
         * the whole process. Instead TERMINATE just this worker (abandon the dispatch callback;
         * the host thread parks) so the process survives. Gated on a no-altstack thread that is
         * LOOPING -- a real Wine thread has an altstack, and a legit fault doesn't loop on the
         * same address (it gets fixed). A foreign worker that ACQUIRED an altstack but still has
         * no TEB at its 64KB stack base (the deterministic ~167M dispatcher crash) is caught by
         * the second clause: the dispatcher's TEB lookup *(rsp&~0xffff) reads an INVALID
         * (uncommitted/poison) pointer, whereas a real Wine thread always has a VALID committed
         * TEB pointer there -- so this targets the altstack-foreign-worker variant WITHOUT
         * terminating a real Wine thread (avoids the hang risk of a blanket LOOPING-alone gate).
         * The fully-proper fix is still #16/#17 marshaling (don't run Wine PE code on workers). */
        int no_wine_teb = g_cur_cpu->sig_altstack_sp == 0;
        if (!no_wine_teb) {
            uint64_t sb = g_cur_cpu->gpr[OCERZ_RSP] & ~0xffffull;
            if (ocerz_addr_committed(sb) == 1 && ocerz_addr_committed(ocerz_ld(sb, 8)) != 1)
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
        /* REWIND rip TO THE FAULTING INSTRUCTION BEFORE DELIVERING.
         *
         * A faulting instruction is ABORTED: it does not complete, so the
         * architectural rip a real x86 kernel reports in the signal frame is the
         * faulting instruction itself, not the one after it. Both ocerz tiers
         * advance rip to the next instruction before executing (see
         * OcerzCPU::cur_rip), so without this the frame named the WRONG
         * instruction and, worse, sigreturn resumed PAST the faulting access --
         * silently SKIPPING it. That breaks the standard fix-and-retry idiom
         * every commit-on-demand guest relies on (Wine grows a thread stack from
         * the guard-page fault and returns, expecting the store to be retried);
         * the store would simply be dropped and the write lost.
         *
         * Only rewind when the faulting rip is exactly known. If a fault landed
         * in emitted code that has no side table, cur_rip names some earlier
         * instruction, and rewinding to THAT would re-execute good instructions
         * -- strictly worse than the old behaviour. In that case leave rip alone.
         *
         * The crash-dump path below is unaffected: it only reads fault_rip. */
        {
            static int rewind_off = -1;
            if (rewind_off < 0)
                rewind_off = getenv("OCERZ_NO_RIP_REWIND") ? 1 : 0;
            if (rip_exact && !rewind_off)
                g_cur_cpu->rip = fault_rip;
        }
        int delivered = looping ? 0
                       : ocerz_signal_deliver(g_cur_cpu, SIGSEGV, gaddr, code, err);
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
                /* GPR snapshot: for the heap-walk fault the next-chunk pointer in
                 * rax/r8 and the size in rcx/rdx tell us why the walk left the
                 * committed range. */
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
                /* Dump committed guest qwords at rsi and r12 (the heap base / first
                 * chunk for the RtlCreateHeap fault) to read the heap's size fields. */
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
                        if (ocerz_addr_committed(a) == 1) m = hex_into(m, ocerz_ld(a, 8));
                        else m = str_into(m, "<unc>");
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
    /* Out-of-guest-space recovery for the #16/#17 foreign-worker DISPATCHER crash (kills boot
     * processes -> winedbg, before they reach their own window). A thread with no Wine TEB runs
     * the WoW64 dispatcher; its gs-restore reads a POISON TEB pointer from its 64KB stack base
     * and then that poison propagates and faults at UNPREDICTABLE addresses outside guest space
     * (observed [rax+0x320], [rax+0x2f0], and [rax + scaled-index] at 3 different dispatcher
     * insns) -- so matching the FAULT address is hopeless. Instead match the reliable THREAD
     * STATE: a real Wine thread always stores a VALID (committed) TEB pointer at its stack base
     * *(rsp&~0xffff); a foreign worker has poison there. So on any out-of-guest-space fault, if
     * this thread's stack base holds no valid TEB pointer, it is a foreign worker running the
     * dispatcher -- terminate just it (process survives) instead of dying. Real threads (valid
     * TEB at the stack base) are never touched. The fully-proper fix is #16/#17 marshaling. */
    if (g_cur_cpu && g_sig_recover && depth == 0 &&
        !ocerz_host_in_guest_space(si->si_addr)) {
        int no_teb = g_cur_cpu->sig_altstack_sp == 0;
        if (!no_teb) {
            uint64_t sb = g_cur_cpu->gpr[OCERZ_RSP] & ~0xffffull;
            uint64_t teb = ocerz_addr_committed(sb) == 1 ? ocerz_ld(sb, 8) : 0;
            if (ocerz_addr_committed(teb) != 1)
                no_teb = 1;
        }
        if (no_teb) {
            if (g_sigtrace) {
                char tb[96];
                char *t = tb;
                t = str_into(t, "ocerz: gs0x320 WILD-WORKER-TERMINATE pid=");
                t = hex_into(t, (uint64_t)getpid());
                t = str_into(t, " addr=");
                t = hex_into(t, (uint64_t)(uintptr_t)si->si_addr);
                t = str_into(t, "\n");
                write(2, tb, (size_t)(t - tb));
            }
            g_cur_cpu->terminated = 1;
            siglongjmp(*g_sig_recover, 1);
        }
    }
    char buf[256];
    char *p = buf;
    /* Commit-on-demand during the frame build (a nested fault, depth>0): the recovery block
     * above is writing the signal frame onto a reserved-but-uncommitted page (a no-altstack
     * thread builds the frame on its rsp stack). map_lock is unsafe in this nested signal
     * context, so commit the page lock-free and RETURN to re-run the faulting write, instead
     * of dying with _exit(139). Only for a reserved arena page; a genuinely bad address falls
     * through to the fatal path below. */
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
        p = str_into(p, "\n");
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
            p = hex_into(p, ocerz_addr_committed(c->gpr[OCERZ_R15] + 0x18) == 1
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
    const char *st = getenv("OCERZ_SELTRAP");
    if (st)
        ocerz_sel_trap = strtoull(st, NULL, 0);
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
    /* Install the async-signal handler for the signals the wineserver sends
     * cross-thread (SIGUSR1 = the system-APC/suspend kick; SIGQUIT = thread stop).
     * NO SA_RESTART so the target's blocked forwarded syscall returns EINTR and the
     * guest handler is delivered at the syscall boundary. OCERZ_RIPDUMP repurposes
     * SIGUSR1 for the debug rip dump, so only take SIGUSR1 for async delivery when
     * RIPDUMP is off. */
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

    /* OCERZ_TRACE_MAIN_LO/HI, hoisted out of the step loop (it used to re-check
     * a lazy-init static on every guest instruction). */
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

    /* Every diagnostic below is off unless its env var was set, but the loop
     * still paid ~10 loads+compares per guest instruction to discover that.
     * Collapse them to ONE predicate tested once per step: each arm re-tests
     * its own condition inside, so behaviour is identical when any is armed and
     * the cost is a single not-taken branch when none is (the overwhelmingly
     * common case, including every benchmark and every real run).
     * NOTE: this is computed once per ocerz_vm_call and every input is either an
     * env-var-derived value fixed before the loop or a startup-set global, so it
     * cannot go stale underneath the loop. Anything added here must keep that
     * property or it must be tested outside the gate. */
    const int any_diag = (ocerz_exc_trap || ocerz_sel_trap || ocerz_ctx_trap ||
                          ocerz_bt_lo || prof || riptrap_n > 0 || icap || mtrace_lo);
    sigjmp_buf jb;
    sigjmp_buf *prev_recover = g_sig_recover;
    g_sig_recover = &jb;
    sigsetjmp(jb, 1);
    g_cur_cpu = &local;
    while (local.rip != sentinel && !vm->exited) {
        g_riphist[g_riphist_n++ & 31] = local.rip;
        int r;
        int mtrace_hit = 0;
        if (__builtin_expect(any_diag, 0)) {
        if (ocerz_exc_trap && local.rip == ocerz_exc_trap)
            ocerz_exc_report(&local);
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
        } /* any_diag */

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
            /* Walk the guest frame-pointer chain so the FATAL shows the full call chain into the
             * abort (e.g. which framework init -> which libxpc call hit _xpc_api_misuse). */
            {
                fprintf(stderr, "ocerz: initabort-bt rip=%#llx", (unsigned long long)local.rip);
                uint64_t fp = local.gpr[OCERZ_RBP];
                for (int d = 0; d < 24 && fp >= 0x10000; d++) {
                    uint64_t ra = ocerz_addr_committed(fp + 8) == 1 ? ocerz_ld(fp + 8, 8) : 0;
                    fprintf(stderr, " %#llx", (unsigned long long)ra);
                    uint64_t nf = ocerz_addr_committed(fp) == 1 ? ocerz_ld(fp, 8) : 0;
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
