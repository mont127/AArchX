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
 * instruction last set a corrupted global.
 */
#include "ocerz/vm.h"
#include "ocerz/interp.h"
#include "ocerz/jit.h"
#include "ocerz/mem.h"

#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

#define OCERZ_CALL_SENTINEL 0x00000000deadca11ull

static OcerzVM *g_vm;

uint64_t ocerz_watch_addr;

void ocerz_watch_hit(uint64_t gaddr, int size, uint64_t lo, uint64_t hi)
{
    fprintf(stderr, "ocerz: WATCH st [%#llx] size=%d val=%#llx:%#llx rip=%#llx icount=%llu\n",
            (unsigned long long)gaddr, size,
            (unsigned long long)hi, (unsigned long long)lo,
            g_vm ? (unsigned long long)g_vm->cpu.rip : 0,
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
    char buf[256];
    char *p = buf;
    p = str_into(p, "ocerz: guest crash: ");
    p = str_into(p, sig == SIGBUS ? "SIGBUS" : "SIGSEGV");
    p = str_into(p, " host_addr=");
    p = hex_into(p, (uint64_t)(uintptr_t)si->si_addr);
    if (g_vm) {
        p = str_into(p, " guest_rip=");
        p = hex_into(p, g_vm->cpu.rip);
        p = str_into(p, " guest_addr=");
        p = hex_into(p, (uint64_t)(uintptr_t)si->si_addr - ocerz_guest_base);
        p = str_into(p, " icount=");
        p = hex_into(p, g_vm->insn_count);
        uint64_t sp = g_vm->cpu.gpr[OCERZ_RSP];
        int shown = 0;
        p = str_into(p, "\n  bt:");
        for (uint64_t a = sp; a < sp + 0x400 && shown < 14; a += 8) {
            uint64_t v = ocerz_ld(a, 8);
            if (v >= 0x7ff802000000ull && v < 0x7ff818000000ull) {
                p = str_into(p, " ");
                p = hex_into(p, v);
                shown++;
            }
        }
    }
    p = str_into(p, "\n");
    write(2, buf, (size_t)(p - buf));
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
    for (int i = 0; i < nargs && i < 6; i++)
        vm->cpu.gpr[ar[i]] = args[i];
    uint64_t sp = (stack_top & ~0xfull) - 8;
    ocerz_st(sp, 8, OCERZ_CALL_SENTINEL);
    vm->cpu.gpr[OCERZ_RSP] = sp;
    vm->cpu.rip = func;
    const char *icap_s = getenv("OCERZ_ICAP");
    unsigned long long icap = icap_s ? strtoull(icap_s, NULL, 0) : 0;
    while (vm->cpu.rip != OCERZ_CALL_SENTINEL && !vm->exited) {
        int r;
        if (icap && vm->insn_count > icap) {
            ocerz_cpu_dump(&vm->cpu, stderr);
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

int ocerz_vm_run(OcerzVM *vm)
{
    ocerz_vm_install_handlers(vm);

    while (!vm->exited) {
        int r;
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
            fprintf(stderr, "ocerz: %llu instructions executed\n",
                    (unsigned long long)vm->insn_count);
            return 125;
        }
    }
    OCERZ_LOG("guest exited with code %d after %llu instructions\n",
              vm->exit_code, (unsigned long long)vm->insn_count);
    if (vm->jit)
        OCERZ_LOG("jit translated %llu blocks\n",
                  (unsigned long long)ocerz_jit_blocks(vm->jit));
    return vm->exit_code;
}
