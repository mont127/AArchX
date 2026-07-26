/* CPU reset state, plus the register dump used by fatal paths and -trace. */
#include "ocerz/cpu.h"
#include "ocerz/mem.h"

#include <stdlib.h>

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

    for (int i = 0; i < 16; i++) {
        uint64_t v = cpu->gpr[i];
        char s[192];
        int n = 0;
        if (v == 0 || ocerz_addr_committed(v) != 1)
            continue;
        for (; n < (int)sizeof s - 1; n++) {
            uint64_t a = v + (uint64_t)n;
            unsigned char c;
            if ((a & 0xfffu) == 0 && ocerz_addr_committed(a) != 1)
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
