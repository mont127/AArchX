/* The per-instruction flag def/use table behind flags_live.h. */
#include "ocerz/flags_live.h"
#include <stdlib.h>

static int insn_touches_memory(const X86Insn *insn)
{
    switch (insn->op) {
    case OCERZ_OP_LEA: case OCERZ_OP_PREFETCH: case OCERZ_OP_CLFLUSH:
    case OCERZ_OP_NOP:
        return 0;
    /* implicit stack / string memory */
    case OCERZ_OP_PUSH: case OCERZ_OP_POP: case OCERZ_OP_PUSHF: case OCERZ_OP_POPF:
    case OCERZ_OP_CALL: case OCERZ_OP_RET: case OCERZ_OP_LEAVE:
    case OCERZ_OP_MOVS: case OCERZ_OP_STOS: case OCERZ_OP_LODS:
    case OCERZ_OP_SCAS: case OCERZ_OP_CMPS:
        return 1;
    default:
        break;
    }
    for (int i = 0; i < insn->nops; i++)
        if (insn->ops[i].kind == OCERZ_OPK_MEM)
            return 1;
    return 0;
}

/* Strict fault barrier: force every flag live across any memory-touching
 * instruction so a guest signal handler would observe architecturally exact
 * (even dead) flags in its context.  Off by default: dead flags are dead in
 * the guest's own code, and a handler resuming/redirecting execution never
 * assumes flag state.  Live flags are always recoverable regardless. */
static int fault_barrier_enabled(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("OCERZ_FAULT_FLAG_BARRIER") ? 1 : 0;
    return en;
}

static void flags_defuse(const X86Insn *insn, uint64_t *def, uint64_t *use,
                         int fault_barrier)
{

    uint64_t d = 0;
    uint64_t u = OCERZ_FL_ALL;

    switch (insn->op) {

    case OCERZ_OP_MOV:
    case OCERZ_OP_MOVZX:
    case OCERZ_OP_MOVSX:
    case OCERZ_OP_MOVSXD:
    case OCERZ_OP_LEA:
    case OCERZ_OP_NOP:
    case OCERZ_OP_PAUSE:
    case OCERZ_OP_PREFETCH:
    case OCERZ_OP_CLFLUSH:
    case OCERZ_OP_MFENCE:
    case OCERZ_OP_LFENCE:
    case OCERZ_OP_SFENCE:
    case OCERZ_OP_BSWAP:
    case OCERZ_OP_NOT:
    case OCERZ_OP_PUSH: case OCERZ_OP_POP: case OCERZ_OP_CALL: case OCERZ_OP_RET:
    case OCERZ_OP_LEAVE: case OCERZ_OP_XCHG: case OCERZ_OP_CBW: case OCERZ_OP_CWD:
    case OCERZ_OP_MOVS: case OCERZ_OP_STOS: case OCERZ_OP_LODS:
    case OCERZ_OP_CLD: case OCERZ_OP_STD:
    case OCERZ_OP_CPUID: case OCERZ_OP_RDTSC: case OCERZ_OP_RDTSCP:
    case OCERZ_OP_MOVSEG: case OCERZ_OP_FXSAVE: case OCERZ_OP_FXRSTOR:
    case OCERZ_OP_LDMXCSR: case OCERZ_OP_STMXCSR: case OCERZ_OP_XGETBV:
        d = 0;
        u = 0;
        break;
    case OCERZ_OP_POPF:
        d = OCERZ_FL_ALL;
        u = 0;
        break;
    case OCERZ_OP_LAHF:
        d = 0;
        u = OCERZ_SF | OCERZ_ZF | OCERZ_AF | OCERZ_PF | OCERZ_CF;
        break;

    case OCERZ_OP_ADD:
    case OCERZ_OP_SUB:
    case OCERZ_OP_CMP:
    case OCERZ_OP_AND:
    case OCERZ_OP_OR:
    case OCERZ_OP_XOR:
    case OCERZ_OP_TEST:
    case OCERZ_OP_NEG:
        d = OCERZ_FL_ALL;
        u = 0;
        break;

    case OCERZ_OP_MUL:
    case OCERZ_OP_IMUL:
    case OCERZ_OP_DIV:      /* flags undefined after div/idiv: nothing may read them */
    case OCERZ_OP_IDIV:
        d = OCERZ_FL_ALL;
        u = 0;
        break;

    case OCERZ_OP_ADC:
    case OCERZ_OP_SBB:
        d = OCERZ_FL_ALL;
        u = OCERZ_CF;
        break;

    /* SSE compares write ZF/PF/CF and clear OF/SF/AF: they define everything. */
    case OCERZ_OP_UCOMISS:
    case OCERZ_OP_UCOMISD:
    case OCERZ_OP_COMISS:
    case OCERZ_OP_COMISD:
        d = OCERZ_FL_ALL;
        u = 0;
        break;

    case OCERZ_OP_SETCC:
    case OCERZ_OP_CMOVCC:
        d = 0;
        switch (insn->cc >> 1) {
        case 0: u = OCERZ_OF; break;
        case 1: u = OCERZ_CF; break;
        case 2: u = OCERZ_ZF; break;
        case 3: u = OCERZ_CF | OCERZ_ZF; break;
        case 4: u = OCERZ_SF; break;
        case 5: u = OCERZ_PF; break;
        case 6: u = OCERZ_SF | OCERZ_OF; break;
        default: u = OCERZ_ZF | OCERZ_SF | OCERZ_OF; break;
        }
        break;

    case OCERZ_OP_INC:
    case OCERZ_OP_DEC:
        d = OCERZ_FL_ALL & ~(uint64_t)OCERZ_CF;
        u = 0;
        break;

    case OCERZ_OP_SHL:
    case OCERZ_OP_SHR:
    case OCERZ_OP_SAR:
        if (insn->nops >= 2 && insn->ops[1].kind == OCERZ_OPK_IMM) {
            unsigned cnt = (unsigned)(insn->ops[1].imm &
                                      (insn->ops[0].size == 8 ? 63u : 31u));
            if (cnt == 0) {
                d = 0;
                u = 0;
            } else {
                d = OCERZ_FL_ALL;
                u = 0;
            }
        }
        break;

    case OCERZ_OP_JCC:
        d = 0;
        switch (insn->cc >> 1) {
        case 0: u = OCERZ_OF; break;
        case 1: u = OCERZ_CF; break;
        case 2: u = OCERZ_ZF; break;
        case 3: u = OCERZ_CF | OCERZ_ZF; break;
        case 4: u = OCERZ_SF; break;
        case 5: u = OCERZ_PF; break;
        case 6: u = OCERZ_SF | OCERZ_OF; break;
        default: u = OCERZ_ZF | OCERZ_SF | OCERZ_OF; break;
        }
        break;

    case OCERZ_OP_JMP:
    case OCERZ_OP_JRCXZ:
    case OCERZ_OP_LOOP:
        d = 0;
        u = 0;
        break;

    case OCERZ_OP_LOOPE:
    case OCERZ_OP_LOOPNE:
        d = 0;
        u = OCERZ_ZF;
        break;

    /* x87 / SSE: only the compare-into-RFLAGS forms and PTEST touch the
     * arithmetic flags; FCMOVcc reads CF/ZF/PF.  Everything else in those
     * ranges is flag-neutral (memory forms stay fault barriers below). */
    case OCERZ_OP_FCOMI: case OCERZ_OP_FCOMIP:
    case OCERZ_OP_FUCOMI: case OCERZ_OP_FUCOMIP:
    case OCERZ_OP_PTEST:
        d = OCERZ_FL_ALL;
        u = 0;
        break;
    case OCERZ_OP_FCMOVCC:
        d = 0;
        u = OCERZ_CF | OCERZ_ZF | OCERZ_PF;
        break;

    /* bit scans: bsf/bsr write ZF and leave the rest (interpreter semantics,
     * matches the goldens); tzcnt/lzcnt write CF and ZF; popcnt writes all */
    case OCERZ_OP_BSF: case OCERZ_OP_BSR:
        d = OCERZ_ZF; u = 0;
        break;
    case OCERZ_OP_TZCNT: case OCERZ_OP_LZCNT:
        d = OCERZ_CF | OCERZ_ZF; u = 0;
        break;
    case OCERZ_OP_POPCNT:
        d = OCERZ_FL_ALL; u = 0;
        break;
    /* xadd/cmpxchg set the arithmetic flags like add/cmp; xchg touches none */
    case OCERZ_OP_XADD: case OCERZ_OP_CMPXCHG:
        d = OCERZ_FL_ALL; u = 0;
        break;

    default:
        if (insn->op >= OCERZ_OP_X87_FIRST && insn->op < OCERZ_OP_COUNT) {
            d = 0;
            u = 0;
        }
        break;
    }

    if (fault_barrier && fault_barrier_enabled() && insn_touches_memory(insn))
        u = OCERZ_FL_ALL;

    *def = d;
    *use = u;
}

void ocerz_flags_defuse(const X86Insn *insn, uint64_t *def, uint64_t *use)
{
    flags_defuse(insn, def, use, 1);
}

void ocerz_flags_defuse_nofault(const X86Insn *insn, uint64_t *def, uint64_t *use)
{
    flags_defuse(insn, def, use, 0);
}
