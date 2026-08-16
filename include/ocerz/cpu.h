/* The emulated x86_64 CPU state and its conventions. */
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

    uint64_t cur_rip;
    uint64_t rflags;

    uint64_t cc_src;
    uint64_t cc_dst;
    uint32_t cc_op;
    uint64_t fs_base;
    uint64_t gs_base;

    uint8_t mode32;
    uint16_t cs_sel;
    Ocerz128 xmm[16] __attribute__((aligned(16)));   /* 16-aligned: JIT uses scaled ldr/str q */
    /* return-address stack right after xmm: keeps ras[] within stp/ldp
     * immediate reach (<= 504 bytes) for the JIT's call/ret paths */
    uint32_t ras_top;
    struct { uint64_t guest_rip; void *host_entry; } ras[256];
    Ocerz128 fp_ckpt[16] __attribute__((aligned(16))); /* JIT FP-batch checkpoints (replay inputs) */
    uint64_t jit_scratch[2];        /* JIT temporaries that must survive a C callout (e.g. an old CF) */
    uint32_t mxcsr;
    uint16_t fcw;
    uint16_t fsw;
    uint8_t ftw;
    uint8_t ftop;
    double fpr[8];
    struct OcerzVM *vm;
    int terminated;
    int interp_once;                /* run the next instruction in the interpreter (fault recovery) */
    int cpu_number;
    uint64_t wq_workloop_id;

    uint64_t sig_altstack_sp;
    uint64_t sig_altstack_size;
    uint64_t sig_mask;
    int sig_on_stack;
    uint64_t sig_last_fault;
    int sig_repeat;

    uint64_t wine_teb_base;

    volatile int interrupt;
} OcerzCPU;

#define OCERZ_RAS_SIZE 256

static inline int ocerz_gs_is_teb_band(uint64_t gs)
{
    return gs >= 0x10000ull && gs < 0x380000000ull;
}

void ocerz_cpu_reset(OcerzCPU *cpu);
void ocerz_cpu_dump(const OcerzCPU *cpu, FILE *out);

#endif
