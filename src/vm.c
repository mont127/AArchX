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
 * attempting that scan.
 */
#include "ocerz/vm.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/mem.h"

#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>

#define OCERZ_CALL_SENTINEL 0x00000000deadca11ull

static OcerzVM *g_vm;
static __thread OcerzCPU *g_cur_cpu;

uint64_t ocerz_watch_addr;

void ocerz_watch_hit(uint64_t gaddr, int size, uint64_t lo, uint64_t hi)
{
    OcerzCPU *c = g_cur_cpu ? g_cur_cpu : (g_vm ? &g_vm->cpu : NULL);
    fprintf(stderr, "ocerz: WATCH st [%#llx] size=%d val=%#llx:%#llx rip=%#llx icount=%llu\n",
            (unsigned long long)gaddr, size,
            (unsigned long long)hi, (unsigned long long)lo,
            c ? (unsigned long long)c->rip : 0,
            g_vm ? (unsigned long long)g_vm->insn_count : 0);
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
    (void)ctx;
    static volatile int depth;
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
    OcerzCPU *c = g_cur_cpu ? g_cur_cpu : (g_vm ? &g_vm->cpu : NULL);
    if (c) {
        p = str_into(p, " guest_rip=");
        p = hex_into(p, c->rip);
        p = str_into(p, " guest_addr=");
        p = hex_into(p, (uint64_t)(uintptr_t)si->si_addr - ocerz_guest_base);
        p = str_into(p, " icount=");
        p = hex_into(p, g_vm ? g_vm->insn_count : 0);
    }
    p = str_into(p, "\n");
    write(2, buf, (size_t)(p - buf));
    if (c) {
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
    const char *w = getenv("OCERZ_WATCH");
    if (w)
        ocerz_watch_addr = strtoull(w, NULL, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
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
    for (int i = 0; i < nargs && i < 6; i++)
        vm->cpu.gpr[ar[i]] = args[i];
    uint64_t sp = (stack_top & ~0xfull) - 8;
    ocerz_st(sp, 8, sentinel);
    vm->cpu.gpr[OCERZ_RSP] = sp;
    vm->cpu.rip = func;
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
    while (vm->cpu.rip != sentinel && !vm->exited) {
        int r;
        if (prof && vm->insn_count >= prof_next) {
            prof_next = vm->insn_count + prof;
            fprintf(stderr, "ocerz: PROFILE icount=%llu rip=%#llx\n",
                    (unsigned long long)vm->insn_count, (unsigned long long)vm->cpu.rip);
        }
        for (int t = 0; t < riptrap_n; t++) {
            if (vm->cpu.rip == riptrap[t] && riptrap_hit[t] < 40) {
                riptrap_hit[t]++;
                fprintf(stderr, "ocerz: RIPLOG hit %#llx (#%d) icount=%llu rdi=%#llx rsi=%#llx rdx=%#llx rbx=%#llx r14=%#llx\n",
                        (unsigned long long)riptrap[t], riptrap_hit[t],
                        (unsigned long long)vm->insn_count,
                        (unsigned long long)vm->cpu.gpr[OCERZ_RDI],
                        (unsigned long long)vm->cpu.gpr[OCERZ_RSI],
                        (unsigned long long)vm->cpu.gpr[OCERZ_RDX],
                        (unsigned long long)vm->cpu.gpr[OCERZ_RBX],
                        (unsigned long long)vm->cpu.gpr[OCERZ_R14]);
            }
        }
        if (icap && vm->insn_count > icap) {
            ocerz_cpu_dump(&vm->cpu, stderr);
            uint64_t fp = vm->cpu.gpr[OCERZ_RBP];
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
        if (vm->jit_enabled && vm->jit) {
            r = ocerz_jit_step(vm, &vm->cpu);
            if (r == OCERZ_EUNSUP)
                r = ocerz_interp_step(vm, &vm->cpu);
        } else {
            r = ocerz_interp_step(vm, &vm->cpu);
        }
        if (r == OCERZ_STEP_EXIT)
            break;
        if (r == OCERZ_STEP_FATAL) {
            ocerz_cpu_dump(&vm->cpu, stderr);
            OCERZ_FATAL("initializer call to %#llx aborted after %llu instructions\n",
                        (unsigned long long)func, (unsigned long long)vm->insn_count);
            _exit(125);
        }
    }
    return vm->cpu.gpr[OCERZ_RAX];
}

void ocerz_vm_request_exit(OcerzVM *vm, int code)
{
    vm->exited = 1;
    vm->exit_code = code;
}

int ocerz_vm_run_cpu(OcerzVM *vm, OcerzCPU *cpu)
{
    g_cur_cpu = cpu;
    const char *tlo = getenv("OCERZ_TRACE_LO");
    const char *thi = getenv("OCERZ_TRACE_HI");
    uint64_t trace_lo = tlo ? strtoull(tlo, NULL, 0) : 0;
    uint64_t trace_hi = thi ? strtoull(thi, NULL, 0) : 0;
    while (!vm->exited && !cpu->terminated) {
        int r;
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
            return 125;
        }
    }
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
