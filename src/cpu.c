/*
 * src/cpu.c
 *
 * CPU lifecycle helpers: architectural reset state and the diagnostic
 * register dump used by fatal-error paths and the -trace mode.
 *
 * Reset state mirrors what XNU hands a fresh x86_64 user thread: all GPRs
 * zero, rflags 0x202 (fixed bit 1 plus IF — user code always sees
 * interrupts enabled), x87 control word 0x037f (all exceptions masked,
 * 64-bit precision, round-to-nearest), MXCSR 0x1f80 (all SSE exceptions
 * masked, round-to-nearest), empty x87 stack (ftw 0, ftop 0), zeroed XMM
 * registers and segment bases.
 *
 * The dump prints every GPR, rip, rflags with decoded mnemonic letters,
 * segment bases, and the low halves of the XMM registers — enough to
 * diagnose any divergence without drowning the terminal.
 */
#include "ocerz/cpu.h"

void ocerz_cpu_reset(OcerzCPU *cpu)
{
    struct OcerzVM *vm = cpu->vm;
    memset(cpu, 0, sizeof *cpu);
    cpu->vm = vm;
    cpu->rflags = OCERZ_FLAG_FIXED1 | OCERZ_IF;
    cpu->fcw = 0x037f;
    cpu->mxcsr = 0x1f80;
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
