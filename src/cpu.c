/*
 * CPU reset state, plus the register dump used by the fatal paths and -trace.
 *
 * The guest MXCSR rounding-control bits are propagated to the host arm64 FPCR,
 * which both the JIT's arm64 FP instructions and the interpreter's C-computed
 * SSE ops honour.  x86 and arm64 disagree on the directed encodings - MXCSR 01
 * rounds toward -inf while FPCR 01 rounds toward +inf - so the mapping goes
 * through the fe* constants rather than copying the bits across.  Without it,
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
 * Reset installs Darwin's flat 64-bit user selectors, which is what `mov %ss, r`
 * reads out of reset.
 */
#include "ocerz/cpu.h"
#include "ocerz/decode.h"
#include "ocerz/mem.h"

#include "ocerz/mode.h"

#include <arm_acle.h>
#include <fenv.h>
#include <stdlib.h>
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

void ocerz_apply_mxcsr_round(uint32_t mxcsr)
{
    static const int fe[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    fesetround(fe[(mxcsr >> 13) & 3]);
    if (g_afp) {
        uint64_t fpcr = __arm_rsr64("fpcr");
        if (!(fpcr & CPU_FPCR_AH))
            __arm_wsr64("fpcr", fpcr | CPU_FPCR_AH);
    }
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
