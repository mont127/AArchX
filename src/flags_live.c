/* The per-instruction flag def/use table behind flags_live.h. */
#include "ocerz/flags_live.h"
#include <stdlib.h>

static int insn_touches_memory(const X86Insn *insn)
{
    if (insn->op == OCERZ_OP_LEA || insn->op == OCERZ_OP_PREFETCH ||
        insn->op == OCERZ_OP_CLFLUSH || insn->op == OCERZ_OP_NOP)
        return 0;
    for (int i = 0; i < insn->nops; i++)
        if (insn->ops[i].kind == OCERZ_OPK_MEM)
            return 1;
    return 0;
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
        d = 0;
        u = 0;
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

    default:

        break;
    }

    if (fault_barrier && insn_touches_memory(insn) &&
        !getenv("OCERZ_NO_FAULT_FLAGS"))
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
