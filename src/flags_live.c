/*
 * src/flags_live.c
 *
 * The def/use table behind include/ocerz/flags_live.h. Read that header first:
 * it states the safety contract (def ⊆ written, use ⊇ read) that every entry
 * here is written to satisfy, and explains why the default is def=0/use=ALL.
 *
 * Each entry below is justified against src/flags.c, which is the bit-exact
 * semantic reference for the whole emulator.
 */
#include "ocerz/flags_live.h"

/* Does any operand actually ACCESS memory? This is the fault barrier's trigger.
 *
 * LEA is the deliberate exception and it matters: its "memory" operand is an
 * address COMPUTATION that is never dereferenced, so LEA cannot fault and reads
 * no flag. Treating it as a barrier would penalize one of the most common
 * instructions in real code for no reason. src/jit.c:emit_lea confirms it emits
 * no access.
 *
 * PREFETCH/CLFLUSH name memory but architecturally cannot fault (and the JIT
 * emits nothing for them at all).
 */
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

void ocerz_flags_defuse(const X86Insn *insn, uint64_t *def, uint64_t *use)
{
    /* The conservative default, and what every unlisted op keeps: kill nothing,
     * read everything. This is exactly today's eager behaviour, so an op missing
     * from the whitelist costs speed and never correctness. */
    uint64_t d = 0;
    uint64_t u = OCERZ_FL_ALL;

    switch (insn->op) {

    /* ---- No flags read, no flags written. ---------------------------------
     * Pure data movement and address arithmetic. */
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
    /* NOT is the classic trap in this table: it is an ALU op that writes NO
     * flags at all (unlike NEG, which writes all six). */
    case OCERZ_OP_NOT:
        d = 0;
        u = 0;
        break;

    /* ---- Write all six, read none. ----------------------------------------
     * flags.c: ocerz_flags_add / _sub / _logic all end in put_arith(), which
     * rewrites the full arithmetic set. */
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

    /* ---- MUL/IMUL: write all six, read none. ------------------------------
     * flags.c:ocerz_flags_mul/_imul both end in put_arith(), which rewrites the
     * full OCERZ_ARITH_FLAGS set (AF included — ocerz defines it 0 here rather
     * than leaving the architecturally-undefined bit stale). No form of either
     * op reads a flag: the one-operand, two-operand and three-operand shapes
     * all compute purely from their register/immediate inputs. This entry costs
     * nothing on the memory-operand forms, which the fault barrier below still
     * forces to use=ALL. */
    case OCERZ_OP_MUL:
    case OCERZ_OP_IMUL:
        d = OCERZ_FL_ALL;
        u = 0;
        break;

    /* ---- Read CF, write all six. ------------------------------------------
     * The carry-in forms. use=CF is the entry that makes an ADC/SBB consumer
     * hold its producer's CF live across the block. */
    case OCERZ_OP_ADC:
    case OCERZ_OP_SBB:
        d = OCERZ_FL_ALL;
        u = OCERZ_CF;
        break;

    /* ---- INC/DEC: write all but CF, read none. ----------------------------
     * flags.c:ocerz_flags_inc/_dec fold (cpu->rflags & OCERZ_CF) back into the
     * value they store, i.e. CF is PRESERVED, not computed. Modelling that as
     * def = ALL&~CF (rather than def=ALL, use=CF) is both correct and strictly
     * more precise: CF stays live across an INC only when something downstream
     * actually reads it, instead of unconditionally.
     *
     * This mirrors the emitter, which commits with clear_mask = ALL&~CF and so
     * leaves the physical CF bit untouched. Do NOT "simplify" this to def=ALL:
     * that would let an INC kill a CF its successor still reads. */
    case OCERZ_OP_INC:
    case OCERZ_OP_DEC:
        d = OCERZ_FL_ALL & ~(uint64_t)OCERZ_CF;
        u = 0;
        break;

    /* ---- Constant-count shifts. -------------------------------------------
     * A count of 0 leaves the destination AND every flag untouched — the
     * interpreter early-outs and the JIT emits nothing — so it defs nothing.
     * Any other immediate count writes all six (flags.c:ocerz_flags_shl/_shr/
     * _sar all put_arith). A CL-count shift has no compile-time count, so it
     * cannot be proven nonzero and keeps the conservative default. */
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

    /* ---- Conditional/unconditional branches: write no flag. ---------------
     * These appear only as a block terminator. Their `use` is what XLIVE's
     * cross-seam relaxation reads: a predecessor A that links to B may drop a
     * flag that B (starting with one of these) never reads. Until STEP 2
     * relaxes the terminator live_out=ALL seed at jit.c, these entries are
     * INERT (live_out=ALL and def=0 leave the backward pass unchanged), so this
     * table is safe to land first.
     *
     * JCC reads exactly its condition's flags. The mapping is emit_jcc's own
     * `cc >> 1` table (src/jit.c): the low bit only inverts the sense, it reads
     * the SAME flags. def stays 0 — a branch writes no arithmetic flag — so a
     * superset use is the only correctness obligation, and this is the exact set
     * (an exact use is still a valid superset). */
    case OCERZ_OP_JCC:
        d = 0;
        switch (insn->cc >> 1) {
        case 0: u = OCERZ_OF; break;                 /* O  */
        case 1: u = OCERZ_CF; break;                 /* B  */
        case 2: u = OCERZ_ZF; break;                 /* E  */
        case 3: u = OCERZ_CF | OCERZ_ZF; break;      /* BE */
        case 4: u = OCERZ_SF; break;                 /* S  */
        case 5: u = OCERZ_PF; break;                 /* P  */
        case 6: u = OCERZ_SF | OCERZ_OF; break;      /* L  */
        default: u = OCERZ_ZF | OCERZ_SF | OCERZ_OF; break; /* LE */
        }
        break;

    /* JMP (direct or indirect), JRCXZ and LOOP read NO flag: JMP is
     * unconditional, JRCXZ/LOOP branch on rcx (a register, not rflags). def=0.
     * An INDIRECT JMP through memory is still forced use=ALL by the fault
     * barrier below, so this is only the no-flag claim for the register/imm
     * forms; the memory-fault path stays conservative. */
    case OCERZ_OP_JMP:
    case OCERZ_OP_JRCXZ:
    case OCERZ_OP_LOOP:
        d = 0;
        u = 0;
        break;

    /* LOOPE/LOOPNE branch on (rcx != 0) AND ZF, so they read exactly ZF. */
    case OCERZ_OP_LOOPE:
    case OCERZ_OP_LOOPNE:
        d = 0;
        u = OCERZ_ZF;
        break;

    default:
        /* def=0, use=ALL. See the header: this is the safe direction.
         * CALL/RET/IRET/SYSCALL/INT/INT3/UD2 deliberately land here — they keep
         * use=ALL so XLIVE never relaxes across a fault/dynamic barrier. */
        break;
    }

    /* THE FAULT BARRIER. Anything that dereferences memory can take a guest
     * signal, and the handler may PUSHF and read flags this pass would
     * otherwise have called dead. Force every flag live across it. def is left
     * alone: the write still happens, it just cannot license killing a
     * predecessor whose value a handler might observe first.
     *
     * Applied last so it cannot be missed by an early break above. */
    if (insn_touches_memory(insn))
        u = OCERZ_FL_ALL;

    *def = d;
    *use = u;
}
