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
    uint64_t jit_fp;
    /* OCERZ_BTRACE: per-cpu ring of guest block entries, written by JIT'd code
     * itself.  g_riphist only samples JIT *exit* points, so with block chaining
     * it is blind to control flow that stays inside the code arena; this ring
     * is the only way to see which guest block actually ran. */
    uint64_t *btrace;
    uint32_t btrace_n;              /* monotonic write counter */
    uint32_t btrace_mask;           /* ring mask; 0 latches (stops recording) */
    volatile uint64_t block_since_ns;  /* nonzero while parked in a blocking host syscall (unstick monitor) */
    volatile uint64_t block_started_ns; /* like block_since_ns but never re-armed: true episode start */
    volatile int block_what;            /* syscall/trap number of the blocking call */
    uint32_t sendring_id[8], sendring_port[8], sendring_sz[8];
    int sendring_n;                     /* MACHSLOW: last mach sends, for wedge diagnostics */
    volatile uint32_t last_rcv_name;   /* port/set of the current mach receive (diagnostics) */                /* host sp at the active JIT function's frame base (class 3):
                                       every exit resets sp to it, dropping the host-stack RAS entries */
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
    /* Signals raised while sig_mask blocked them, held until it drops.  Same
     * bit convention as sig_mask (bit sig-1), which is also the guest's
     * sigset_t layout, so sigpending() can copy it out directly. */
    uint64_t sig_pending;
    int sig_on_stack;
    uint64_t sig_last_fault;
    int sig_repeat;

    uint64_t wine_teb_base;

    volatile int interrupt;
    /* a probed superblock side exit that just tripped to C: the block and
     * its side index, for the trace-inversion decision (see g_flip in jit.c) */
    void *side_blk;
    int side_idx;

    /* Segment selectors, indexed by OcerzSreg (ES,CS,SS,DS,FS,GS).  Only the
     * 32-bit guest touches these: PUSH/POP sreg and LES/LDS name a segment
     * register as a value rather than as an address override, so the selector
     * has to live somewhere.  cs_sel above stays the authority for CS -- these
     * mirror it -- because the JIT and the far-branch paths already read it.
     * Appended at the very end of the struct so that no existing field, and in
     * particular neither the 16-byte-aligned xmm[] nor the ras[] window the
     * JIT reaches with stp/ldp immediates, moves by a single byte. */
    uint16_t seg_sel[6];
    uint64_t dbg_ind_src;   /* OCERZ_WILDLOG: guest rip of the last indirect jmp/call dispatched */
} OcerzCPU;

#define OCERZ_RAS_SIZE 256

static inline int ocerz_gs_is_teb_band(uint64_t gs)
{
    return gs >= 0x10000ull && gs < 0x380000000ull;
}

void ocerz_cpu_reset(OcerzCPU *cpu);
void ocerz_cpu_dump(const OcerzCPU *cpu, FILE *out);

#endif
