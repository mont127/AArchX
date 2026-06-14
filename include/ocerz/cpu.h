/*
 * include/ocerz/cpu.h
 *
 * The emulated x86_64 CPU state and its conventions.
 *
 * gpr[] is indexed in x86 machine-encoding order: 0=RAX 1=RCX 2=RDX 3=RBX
 * 4=RSP 5=RBP 6=RSI 7=RDI 8..15=R8..R15. OCERZ_REG_NONE (0xff) marks an
 * absent base/index register in decoded memory operands.
 *
 * rflags is kept EAGERLY: every flag-writing instruction updates the live
 * bits immediately through the helpers in flags.h, so rflags can always be
 * read or pushed without lazy materialization. Bit layout follows the
 * architecture: CF bit0, fixed-1 bit1, PF bit2, AF bit4, ZF bit6, SF bit7,
 * TF bit8, IF bit9, DF bit10, OF bit11. User code on macOS always runs with
 * IF set; ocerz_cpu_reset() initializes rflags to 0x202.
 *
 * fs_base/gs_base back the FS/GS segment overrides. macOS x86_64 thread-local
 * storage lives at %gs:0; the guest sets gs_base through the machine-dependent
 * syscall thread_fast_set_cthread_self, which the syscall layer intercepts
 * and stores here.
 *
 * xmm[] holds the 16 SSE registers as Ocerz128 pairs. mxcsr starts at the
 * architectural reset value 0x1f80 (all exceptions masked).
 *
 * The x87 unit is modeled with 64-bit doubles instead of 80-bit extended
 * precision: fpr[] are the eight physical registers, ftop is the top-of-stack
 * index (0..7, decrements on push), fcw/fsw/ftw are the control, status and
 * tag words. fsw bits 11..13 are synthesized from ftop when software reads
 * the status word. This loses 80-bit precision; macOS x86_64 code uses SSE
 * for floating point and touches x87 mostly through fnstcw/fldcw in
 * setjmp/longjmp, so the deviation is acceptable for now and documented.
 *
 * The vm back-pointer lets instruction handlers reach process-wide state
 * (exit flags, options, counters) without globals, which keeps the design
 * ready for one-CPU-per-thread later.
 */
#ifndef OCERZ_CPU_H
#define OCERZ_CPU_H

#include "ocerz/types.h"

enum {
    OCERZ_RAX = 0,
    OCERZ_RCX = 1,
    OCERZ_RDX = 2,
    OCERZ_RBX = 3,
    OCERZ_RSP = 4,
    OCERZ_RBP = 5,
    OCERZ_RSI = 6,
    OCERZ_RDI = 7,
    OCERZ_R8 = 8,
    OCERZ_R9 = 9,
    OCERZ_R10 = 10,
    OCERZ_R11 = 11,
    OCERZ_R12 = 12,
    OCERZ_R13 = 13,
    OCERZ_R14 = 14,
    OCERZ_R15 = 15,
};

#define OCERZ_REG_NONE 0xff

#define OCERZ_CF ((uint64_t)1 << 0)
#define OCERZ_FLAG_FIXED1 ((uint64_t)1 << 1)
#define OCERZ_PF ((uint64_t)1 << 2)
#define OCERZ_AF ((uint64_t)1 << 4)
#define OCERZ_ZF ((uint64_t)1 << 6)
#define OCERZ_SF ((uint64_t)1 << 7)
#define OCERZ_TF ((uint64_t)1 << 8)
#define OCERZ_IF ((uint64_t)1 << 9)
#define OCERZ_DF ((uint64_t)1 << 10)
#define OCERZ_OF ((uint64_t)1 << 11)

enum {
    OCERZ_SEG_NONE = 0,
    OCERZ_SEG_FS = 1,
    OCERZ_SEG_GS = 2,
};

typedef struct OcerzCPU {
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t rflags;
    uint64_t fs_base;
    uint64_t gs_base;
    Ocerz128 xmm[16];
    uint32_t mxcsr;
    uint16_t fcw;
    uint16_t fsw;
    uint8_t ftw;
    uint8_t ftop;
    double fpr[8];
    struct OcerzVM *vm;
    int terminated;
    int cpu_number;
    uint64_t wq_workloop_id;
    /* Per-thread signal state for guest signal delivery (see syscall.c): the
     * registered sigaltstack and the current blocked mask, plus a flag that is
     * set while the thread is running on its altstack so a nested signal does
     * not re-switch to it. */
    uint64_t sig_altstack_sp;
    uint64_t sig_altstack_size;
    uint64_t sig_mask;
    int sig_on_stack;
    uint64_t sig_last_fault;
    int sig_repeat;
    /* The last TEB-band gs base this thread installed (what WoW64 PE code runs
     * with). A WoW64 PE class-0 syscall is always issued with gs=TEB; if a class-0
     * SIGSYS is taken while gs is the unix cthread base (Wine's own signal handler
     * swapped it via machdep), ocerz restores gs to this so __wine_syscall_
     * dispatcher resolves the TEB the macOS-14 kernel would have, instead of
     * reading cthread-relative junk (the 0x3fff66748 / [0x8ffff8] crash). */
    uint64_t wine_teb_base;
} OcerzCPU;

/* A gs base is "TEB-band" (a Wine TEB, what PE code runs with, ~0x7ffd8000) when
 * it is neither the arena-resident unix cthread base (> 0x380000000) nor a tiny
 * garbage value. */
static inline int ocerz_gs_is_teb_band(uint64_t gs)
{
    return gs >= 0x10000ull && gs < 0x380000000ull;
}

void ocerz_cpu_reset(OcerzCPU *cpu);
void ocerz_cpu_dump(const OcerzCPU *cpu, FILE *out);

#endif
