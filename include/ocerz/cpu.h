/*
 * include/ocerz/cpu.h
 *
 * The emulated x86_64 CPU state and its conventions.
 *
 * gpr[] is indexed in x86 machine-encoding order: 0=RAX 1=RCX 2=RDX 3=RBX
 * 4=RSP 5=RBP 6=RSI 7=RDI 8..15=R8..R15. OCERZ_REG_NONE (0xff) marks an
 * absent base/index register in decoded memory operands.
 *
 * rflags is architecturally current at every point that can OBSERVE it. In the
 * INTERPRETER that is every instruction boundary: each flag-writing instruction
 * updates the live bits immediately through the helpers in flags.h. The JIT
 * tier skips flag writes it proves dead (see src/flags_live.c), so between two
 * inlined arithmetic ops a bit may briefly hold a stale value -- but never at
 * any point that can read it: a block boundary, a slow-path call, a Jcc, or a
 * faulting instruction all force every flag live. Anything reading rflags from
 * outside the JIT's emitted code (PUSHF, a signal mcontext, ocerz_cpu_dump)
 * therefore still sees architecturally correct values. Bit layout follows the
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
    /* The rip of the instruction CURRENTLY EXECUTING, which is NOT what `rip`
     * holds. Both tiers advance rip to the NEXT instruction before executing
     * (interp: `cpu->rip += insn.len`; jit: exec_one's `cpu->rip = insn->rip +
     * insn->len`), because that is the value x86 semantics need mid-instruction
     * -- CALL pushes it as the return address. So on a fault, `rip` names the
     * instruction AFTER the faulting one, while a real x86 kernel reports the
     * faulting instruction itself in the signal frame.
     *
     * cur_rip carries that faulting address. It is written on the slow path
     * only (one store, adjacent to the rip store, same cache line). INLINED JIT
     * instructions deliberately do NOT write it -- that would put a store on
     * exactly the path the JIT exists to keep empty; for those, the fault
     * handler recovers the exact rip from the block's host-pc -> guest-rip side
     * table instead (ocerz_jit_fault_rip). See crash_handler in src/vm.c. */
    uint64_t cur_rip;
    uint64_t rflags;
    /* Deferred-flag (lazy RFLAGS) state. When cc_op == OCERZ_CC_NONE the six
     * arithmetic flags are architecturally LIVE in rflags; otherwise they are
     * DEFERRED and reconstructed on demand by ocerz_flags_materialize() from
     * cc_src/cc_dst per the operation packed in cc_op (kind + operand size +
     * ADC/SBB carry-in; see flags.h). The JIT stores this record instead of
     * computing flags on the hot path and materializes at each consumer (Jcc,
     * every slow-path call, block exit) and the fault seam, so the interpreter
     * and everything outside emitted JIT code never observes a non-NONE cc_op:
     * rflags stays the single reference the whole emulator diffs against.
     * cpu_reset()/calloc leave cc_op == 0 == OCERZ_CC_NONE. */
    uint64_t cc_src;
    uint64_t cc_dst;
    uint32_t cc_op;
    uint64_t fs_base;
    uint64_t gs_base;
    /* WoW64 mode. 0 = 64-bit long mode (always, today); 1 would mean the guest has
     * far-jumped into a 32-bit code segment. ocerz can RECOGNISE that transition but the
     * decoder is still long-mode-only, so entering it is a hard, loud stop rather than a
     * silent mis-execution of 64-bit-decoded bytes. cs_sel records the code selector a far
     * transfer installed, so the diagnostic can name it. */
    uint8_t mode32;
    uint16_t cs_sel;
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
    /* Cross-thread cooperative-stop / interrupt poll word. Set to non-zero by
     * ocerz_vm_request_exit() (from ANY thread) for every live CPU registered in
     * the run loop; the run-loop header re-reads it every block today. It exists
     * as a DEDICATED per-CPU word (not the shared vm->exited, and distinct from
     * `terminated`, which stays a thread's own self-death signal) so that once
     * the JIT chains hot self-loops -- which stop returning to the run-loop
     * header -- a BACK-EDGE POLL can observe a process-wide/cross-thread stop
     * with a single `ldr w,[x20,#off]; cbnz` off the pinned CPU pointer, instead
     * of loading the unrelated `vm` and its `exited` field. Written/read with
     * atomics on the slow (setter) side; the future JIT poll reads it as a plain
     * load (a stale 0 costs at most one more loop iteration, which is fine for a
     * stop signal). Placed last so it sits in its own trailing cache line: the
     * cross-thread store that sets it never invalidates the hot gpr[] line. */
    volatile int interrupt;
    /* Return-address-stack return predictor for the JIT (STEP 1). A PURE HINT:
     * correctness never depends on it. When the JIT inlines a direct CALL it
     * records the return address together with the host entry of the block that
     * begins there (if already compiled); when it inlines the matching RET it
     * checks the top entry against the value it actually popped and, only on an
     * exact match with a non-NULL host entry, branches straight into that block
     * instead of returning to ocerz_jit_step's hash dispatch. ANY mismatch,
     * empty stack, NULL entry, or a guest that manipulated its own stack falls
     * through to today's dispatch, so a stale or wrong prediction can only cost a
     * dispatch, never correctness. Measured miss rate on recursive code is
     * ~0.00003%. Placed after `interrupt`, off the hot gpr[] cache line; unused
     * (ras_top stays 0, nothing branches on it) unless the JIT emits the RAS
     * code, so it is inert when OCERZ_NO_RAS=1.
     *
     * DEPTH 256, not 32: the stack must be deeper than the guest's live call
     * nesting or it SATURATES and the predictor dies. A push past the top is
     * dropped, so once the stack fills the entries at the top are stale deep
     * frames that no shallow return ever matches -- the pop never fires, the top
     * stays pinned, and every return falls through to dispatch. Measured directly:
     * with a 32-entry stack fib(34) (recursion depth 33) saturated within the
     * first 32 calls, dropped every subsequent push, and RAS produced NO speedup;
     * a 256-entry stack has zero drops on the same workload, ~100% of returns
     * predict a valid host entry, and the call two-point metric improves ~5%. 256
     * covers ordinary call nesting cheaply (4 KB, off the hot line, touched only
     * at its live prefix); pathologically deeper guests simply revert to dispatch
     * for the frames past 256, which is correct, just unaccelerated. */
    uint32_t ras_top;
    struct { uint64_t guest_rip; void *host_entry; } ras[256];
} OcerzCPU;

#define OCERZ_RAS_SIZE 256

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
