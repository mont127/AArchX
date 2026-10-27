/*
 * CPU reset state, plus the register dump used by the fatal paths and -trace.
 *
 * The guest MXCSR rounding-control bits are propagated to the host arm64 FPCR,
 * which both the JIT's arm64 FP instructions and the interpreter's C-computed
 * SSE ops honour.  x86 and arm64 disagree on the directed encodings - MXCSR 01
 * rounds toward -inf while FPCR 01 rounds toward +inf - so the mapping goes
 * through a table rather than copying the bits across.  Without it,
 * divsd/sqrtsd and friends always rounded to nearest whatever mode a program
 * selected with ldmxcsr or fesetround.
 *
 * The same call sets FPCR.AH where the processor reports FEAT_AFP, as the M5
 * this was written on does.  With that bit set the arm64 floating-point
 * instructions handle NaNs the way SSE does: an invalid operation produces the
 * negative default NaN x86 calls the real indefinite, and when both operands
 * are NaNs the first one is returned, quieted, whichever of them was
 * signalling, where arm64 otherwise lets a signalling NaN win and answers with
 * a positive default.  src/jit.c
 * then emits add, subtract, multiply, divide and square root bare, with none of
 * the checks and replays that keep those results exact on a processor without
 * the bit.  ocerz_afp_enable decides once, from main, so a unit harness that
 * runs translated code on a thread nothing prepared never gets translations
 * that rely on it, and ocerz_afp answers what it decided.  Native mode leaves
 * the bit alone: the host's own code runs on guest threads there, a crossing
 * would have to clear the bit and set it again, and two FPCR writes cost about
 * fourteen nanoseconds, as much as the rest of the crossing.  OCERZ_NO_AFP=1
 * keeps the software path everywhere.
 *
 * Hardware TSO is the same kind of switch for memory order.  Translated code
 * that more than one thread runs keeps x86's total store order itself, with
 * acquire loads and release stores (src/jit.c, "memory ordering"), at about
 * 17% of 7-Zip's compression under Wine.  Apple's processors keep that order
 * in hardware, and a translator turns it on one thread at a time with
 * thread_set_x86_64_compat(1) (mach/mach_traps.h).  The kernel grants it only
 * to a process holding com.apple.developer.cross-architecture-support or its
 * -unmanaged twin and otherwise answers KERN_FAILURE and changes nothing.  Both are restricted entitlements:
 * a binary signed ad hoc that claims either is killed at launch, so it takes
 * an ocerz signed by a developer team as the main executable of an application
 * bundle whose embedded provisioning profile grants the entitlement.  Nothing
 * in this tree signs that way, and without it everything below answers "off".
 * ocerz_hwtso_enable decides once, from main and on the main thread, as
 * ocerz_afp_enable does: OCERZ_NO_HW_TSO=1 never asks, a kernel that says it
 * has no cross-architecture support (os_cross_arch_is_supported) is not asked,
 * and otherwise the main thread asks for itself and ocerz_hwtso_on records the
 * answer.  Both calls are looked up by name, because an import a macOS lacks
 * loads as NULL and sends the launch to ocerz-umbrella (src/syslink.c), which
 * could not start on that macOS either.  Every other thread asks for itself,
 * once, through ocerz_hwtso_thread before it runs guest code (the run loop,
 * ocerz_thread_attach, and the dispatcher for whatever else gets there); one
 * the kernel refuses while the process relies on the order makes it give the
 * order up for good (ocerz_jit_hwtso_lost) before that thread runs anything.
 * A forked child asks again for its one thread.  -v logs every answer.
 * OCERZ_PRETEND_HW_TSO=1 grants every thread without asking the kernel.  It
 * is UNSAFE and only for measuring what the order is worth on a Mac without
 * the entitlement: translated code then counts on an order the processor does
 * not keep, and a threaded program can compute wrong results.
 * OCERZ_PRETEND_HW_TSO_REFUSE=<n> with it refuses the n-th thread to ask, the
 * main thread being the first, which is how the tests reach the path of a
 * thread refused while others run.
 *
 * Reset installs Darwin's flat 64-bit user selectors, which is what `mov %ss, r`
 * reads out of reset.
 */
#include "ocerz/cpu.h"
#include "ocerz/decode.h"
#include "ocerz/mem.h"

#include "ocerz/mode.h"

#include <arm_acle.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#define CPU_FPCR_AH 0x2ull

static int g_afp;

void ocerz_afp_enable(void)
{
    int have = 0;
    size_t len = sizeof have;
    g_afp = !getenv("OCERZ_NO_AFP") && ocerz_mode != OCERZ_MODE_NATIVE &&
            sysctlbyname("hw.optional.arm.FEAT_AFP", &have, &len, NULL, 0) == 0 && have == 1;
    OCERZ_LOG("cpu: NaN results come from %s\n", g_afp ? "the processor (FPCR.AH)" : "translated checks");
}

int ocerz_afp(void)
{
    return g_afp;
}

int ocerz_hwtso_on;
static int g_hwtso_decided, g_hwtso_pretend, g_hwtso_refuse_at, g_hwtso_asks;
static kern_return_t (*g_hwtso_set)(uint32_t enable);
static __thread int t_hwtso;   /* this thread asked: 1 granted, -1 refused */

static kern_return_t hwtso_ask(void)
{
    int n = __atomic_add_fetch(&g_hwtso_asks, 1, __ATOMIC_RELAXED);
    if (g_hwtso_pretend)
        return n == g_hwtso_refuse_at ? KERN_FAILURE : KERN_SUCCESS;
    return g_hwtso_set ? g_hwtso_set(1) : KERN_NOT_SUPPORTED;
}

void ocerz_hwtso_enable(void)
{
    if (g_hwtso_decided)
        return;
    g_hwtso_decided = 1;
    const char *why = NULL;
    const char *p = getenv("OCERZ_PRETEND_HW_TSO");
    if (getenv("OCERZ_NO_HW_TSO")) {
        why = "OCERZ_NO_HW_TSO";
    } else if (p && *p && strcmp(p, "0") != 0) {
        const char *r = getenv("OCERZ_PRETEND_HW_TSO_REFUSE");
        g_hwtso_pretend = 1;
        g_hwtso_refuse_at = r ? atoi(r) : 0;
    } else {
        bool (*supported)(uint32_t) = (bool (*)(uint32_t))dlsym(RTLD_DEFAULT, "os_cross_arch_is_supported");
        g_hwtso_set = (kern_return_t (*)(uint32_t))dlsym(RTLD_DEFAULT, "thread_set_x86_64_compat");
        if (!g_hwtso_set)
            why = "this macOS has no thread_set_x86_64_compat";
        else if (supported && !supported(1 /* OS_CROSS_ARCH_X86_64 */))
            why = "the kernel has no cross-architecture support";
    }
    if (why) {
        OCERZ_LOG("hardware TSO: off (%s)\n", why);
        return;
    }
    kern_return_t kr = hwtso_ask();
    t_hwtso = kr == KERN_SUCCESS ? 1 : -1;
    __atomic_store_n(&ocerz_hwtso_on, kr == KERN_SUCCESS, __ATOMIC_RELEASE);
    if (kr != KERN_SUCCESS)
        OCERZ_LOG("hardware TSO: off (kr=%d)\n", (int)kr);
    else
        OCERZ_LOG("hardware TSO: on%s\n", g_hwtso_pretend ? " (pretended, OCERZ_PRETEND_HW_TSO: unsafe)" : "");
}

int ocerz_hwtso_thread(void)
{
    if (!__atomic_load_n(&ocerz_hwtso_on, __ATOMIC_ACQUIRE))
        return 1;
    if (!t_hwtso) {
        kern_return_t kr = hwtso_ask();
        t_hwtso = kr == KERN_SUCCESS ? 1 : -1;
        if (kr != KERN_SUCCESS) {
            uint64_t tid = 0;
            pthread_threadid_np(NULL, &tid);
            OCERZ_LOG("hardware TSO: refused to thread %#llx (kr=%d)\n", (unsigned long long)tid, (int)kr);
        }
    }
    return t_hwtso > 0;
}

void ocerz_hwtso_off(void)
{
    __atomic_store_n(&ocerz_hwtso_on, 0, __ATOMIC_RELEASE);
}

/* the forking thread is the child's only one: it asks again, in case the kernel did not carry the mode across */
void ocerz_hwtso_postfork_child(void)
{
    t_hwtso = 0;
    (void)ocerz_hwtso_thread();
}

/* Host code run with FPCR.AH set computes differently: AppKit's TextKit upgrade of an archived NSTextView faulted in
 * UIFoundation under m32 (Wesnoth's main nib), and loads fine with the bit clear.  m32 calls host frameworks from
 * guest threads all the time, as native mode does, so its calls clear the bit and its callbacks set it again. */
uint64_t ocerz_afp_host(void)
{
    uint64_t fpcr = g_afp ? __arm_rsr64("fpcr") : 0;
    if (fpcr & CPU_FPCR_AH)
        __arm_wsr64("fpcr", fpcr & ~CPU_FPCR_AH);
    return fpcr;
}

uint64_t ocerz_afp_guest(void)
{
    uint64_t fpcr = g_afp ? __arm_rsr64("fpcr") : 0;
    if (g_afp && !(fpcr & CPU_FPCR_AH))
        __arm_wsr64("fpcr", fpcr | CPU_FPCR_AH);
    return fpcr;
}

void ocerz_afp_restore(uint64_t fpcr)
{
    if (g_afp && __arm_rsr64("fpcr") != fpcr)
        __arm_wsr64("fpcr", fpcr);
}

/* FPCR.RMode for MXCSR.RC: nearest, down, up, toward zero.  FPCR is written only when it changes, which a callback
 * entering guest code in the mode it already has (nearly every one) never does; fesetround wrote it every time. */
void ocerz_apply_mxcsr_round(uint32_t mxcsr)
{
    static const uint64_t rmode[4] = { 0ull, 0x800000ull, 0x400000ull, 0xc00000ull };
    uint64_t fpcr = __arm_rsr64("fpcr");
    uint64_t want = (fpcr & ~0xc00000ull) | rmode[(mxcsr >> 13) & 3];
    if (g_afp)
        want |= CPU_FPCR_AH;
    if (want != fpcr)
        __arm_wsr64("fpcr", want);
}

void ocerz_cpu_reset(OcerzCPU *cpu)
{
    struct OcerzVM *vm = cpu->vm;
    memset(cpu, 0, sizeof *cpu);
    cpu->vm = vm;
    cpu->rflags = OCERZ_FLAG_FIXED1 | OCERZ_IF;
    cpu->fcw = 0x037f;
    cpu->mxcsr = 0x1f80;
    cpu->cs_sel = 0x2b;
    cpu->seg_sel[OCERZ_SREG_CS] = 0x2b;
    cpu->seg_sel[OCERZ_SREG_SS] = 0x23;
}

void ocerz_cpu_dump(const OcerzCPU *cpu, FILE *out)
{
    static const char *const names[16] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    };
    for (int i = 0; i < 16; i += 2)
        fprintf(out, "%-4s=%016llx  %-4s=%016llx\n",
                names[i], (unsigned long long)cpu->gpr[i],
                names[i + 1], (unsigned long long)cpu->gpr[i + 1]);

    for (int i = 0; i < 16; i++) {
        uint64_t v = cpu->gpr[i];
        char s[192];
        int n = 0;
        if (v == 0 || !ocerz_addr_readable(v))
            continue;
        for (; n < (int)sizeof s - 1; n++) {
            uint64_t a = v + (uint64_t)n;
            unsigned char c;
            if ((a & 0xfffu) == 0 && !ocerz_addr_readable(a))
                break;
            c = (unsigned char)ocerz_ld(a, 1);
            if (c == 0)
                break;
            if (c < 0x20 || c > 0x7e) {
                n = -1;
                break;
            }
            s[n] = (char)c;
        }
        if (n >= 8) {
            s[n] = 0;
            fprintf(out, "%-4s->\"%s\"\n", names[i], s);
        }
    }

    {
        const char *mp = getenv("OCERZ_MSGPTR");
        if (mp) {
            uint64_t slot = strtoull(mp, NULL, 0);
            uint64_t p = (slot && ocerz_addr_committed(slot) == 1) ? ocerz_ld(slot, 8) : 0;
            fprintf(out, "msgptr@%#llx -> %#llx", (unsigned long long)slot,
                    (unsigned long long)p);
            if (p && ocerz_addr_committed(p) == 1) {
                fputs(" \"", out);
                for (int k = 0; k < 200; k++) {
                    uint64_t a = p + (uint64_t)k;
                    unsigned char c;
                    if ((a & 0xfffu) == 0 && ocerz_addr_committed(a) != 1)
                        break;
                    c = (unsigned char)ocerz_ld(a, 1);
                    if (c == 0)
                        break;
                    fputc((c >= 0x20 && c < 0x7f) ? (int)c : '.', out);
                }
                fputc('"', out);
            }
            fputc('\n', out);
        }
    }
    fprintf(out, "rip =%016llx  rflags=%08llx [%c%c%c%c%c%c%c]\n",
            (unsigned long long)cpu->rip,
            (unsigned long long)cpu->rflags,
            (cpu->rflags & OCERZ_OF) ? 'O' : '-',
            (cpu->rflags & OCERZ_SF) ? 'S' : '-',
            (cpu->rflags & OCERZ_ZF) ? 'Z' : '-',
            (cpu->rflags & OCERZ_AF) ? 'A' : '-',
            (cpu->rflags & OCERZ_PF) ? 'P' : '-',
            (cpu->rflags & OCERZ_CF) ? 'C' : '-',
            (cpu->rflags & OCERZ_DF) ? 'D' : '-');
    fprintf(out, "fs_base=%016llx gs_base=%016llx\n",
            (unsigned long long)cpu->fs_base,
            (unsigned long long)cpu->gs_base);
    for (int i = 0; i < 16; i += 2)
        fprintf(out, "xmm%-2d=%016llx:%016llx  xmm%-2d=%016llx:%016llx\n",
                i, (unsigned long long)cpu->xmm[i].hi, (unsigned long long)cpu->xmm[i].lo,
                i + 1, (unsigned long long)cpu->xmm[i + 1].hi, (unsigned long long)cpu->xmm[i + 1].lo);
}
