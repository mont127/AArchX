/*
 * The emulated x86_64 CPU state and its conventions.
 *
 * The field ORDER here is load-bearing, because the JIT reaches this struct
 * with immediate-offset instructions.  xmm[] is 16-aligned so the JIT can use
 * scaled ldr/str q, and the return-address stack sits immediately after it so
 * ras[] stays within the +-504-byte immediate reach of stp/ldp on the call and
 * return paths.  The segment selectors were appended at the very END of the
 * struct for the same reason: nothing that already existed, and in particular
 * neither xmm[] nor the ras[] window, may move by a single byte.
 *
 * Those selectors exist because the 32-bit guest names a segment register as a
 * value rather than as an address override (PUSH/POP sreg, LES/LDS), so the
 * selector has to live somewhere; cs_sel remains the authority for CS and these
 * mirror it, since the JIT and the far-branch paths already read it.  The AVX
 * upper halves of ymm0-15 are likewise interpreter-only state: legacy SSE ops
 * leave them alone, VEX.128 ops zero them, VEX.256 ops write them, and the JIT
 * declines every VEX instruction.
 *
 * Pending signals use the guest sigset_t bit convention (bit sig-1), the same
 * one as sig_mask, so sigpending() can copy the word out directly.
 *
 * A large part of the rest is instrumentation that has to live per-cpu: the
 * OCERZ_BTRACE ring of guest block entries is written by JIT'd code itself,
 * because the exit-point sampler is blind to control flow that stays inside the
 * code arena once blocks are chained; the block_* fields are what the unstick
 * monitor reads to decide whether a thread has been parked too long in a wait
 * whose contract allows a spurious EINTR; and the suspend fields carry the
 * safe-point handshake that lets a guest thread_suspend report done only when
 * the target holds no emulator lock.
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

    uint64_t cur_rip;
    uint64_t rflags;

    uint64_t cc_src;
    uint64_t cc_dst;
    uint32_t cc_op;
    uint64_t fs_base;
    uint64_t gs_base;

    uint8_t mode32;
    uint16_t cs_sel;
    Ocerz128 xmm[16] __attribute__((aligned(16)));
    uint32_t ras_top;
    struct { uint64_t guest_rip; void *host_entry; } ras[256];
    Ocerz128 fp_ckpt[16] __attribute__((aligned(16)));
    uint64_t jit_scratch[2];
    uint64_t jit_fp;
    uint64_t *btrace;
    uint32_t btrace_n;
    uint32_t btrace_mask;
    volatile uint64_t block_since_ns;
    volatile uint64_t block_started_ns;
    volatile int block_what;
    volatile int block_nokick;
    uint32_t sendring_id[8], sendring_port[8], sendring_sz[8];
    int sendring_n;
    volatile uint32_t last_rcv_name;
    uint32_t mxcsr;
    uint16_t fcw;
    uint16_t fsw;
    uint8_t ftw;
    uint8_t ftop;
    double fpr[8];
    struct OcerzVM *vm;
    int terminated;
    int interp_once;
    int cpu_number;
    uint64_t wq_workloop_id;

    uint64_t sig_altstack_sp;
    uint64_t sig_altstack_size;
    uint64_t sig_mask;
    uint64_t sig_pending;
    uint32_t sig_host_rcvd[32];
    uint32_t sig_delivered[32];
    uint32_t in_sighandler;
    void    *host_pthread;
    uint32_t host_kport;
    uint32_t host_mask_last;
    uint32_t host_mask_changes;
    int sig_on_stack;
    uint64_t sig_last_fault;
    int sig_repeat;

    uint64_t wine_teb_base;

    volatile int interrupt;
    void *side_blk;
    int side_idx;

    uint16_t seg_sel[6];
    uint64_t dbg_ind_src;
    uint64_t host_tid;
    int32_t cur_sys_class;
    int32_t cur_sys_num;
    Ocerz128 ymmh[16] __attribute__((aligned(16)));
    volatile int suspend_count;
    volatile int susp_parked;
    int susp_host;
    int susp_have_gpr;
    uint64_t susp_gpr[16];
} OcerzCPU;

#define OCERZ_RAS_SIZE 256

static inline int ocerz_gs_is_teb_band(uint64_t gs)
{
    return gs >= 0x10000ull && gs < 0x380000000ull;
}

void ocerz_cpu_reset(OcerzCPU *cpu);
void ocerz_cpu_dump(const OcerzCPU *cpu, FILE *out);
void ocerz_apply_mxcsr_round(uint32_t mxcsr);

#endif
