/*
 * src/decode.c
 *
 * The complete x86_64 instruction decoder. It is PURE: it reads only the byte
 * buffer handed to it and never touches CPU or guest-memory state. The single
 * entry point ocerz_decode() turns the bytes at code[0] (architecturally
 * located at guest address `rip`) into one X86Insn per the contract in
 * include/ocerz/decode.h. ocerz_op_name() and ocerz_format_insn() are the
 * human-readable side, used only for tracing.
 *
 * Design overview
 * ---------------
 * A small DecState carries the live cursor (a byte pointer plus a remaining
 * count), the resolved prefix state, and the accumulating X86Insn. Every byte
 * read goes through fetch helpers that return OCERZ_ETRUNC the moment the
 * window is exhausted, so no path ever reads past `avail`. After a successful
 * decode the total length is recomputed from the cursor and checked against
 * the 15-byte architectural ceiling (OCERZ_ETOOLONG).
 *
 * Prefix loop. Legacy prefixes (66 67 F0 F2 F3 and the six segment bytes) may
 * appear in any order and any count. REX (40..4F) is captured only when it is
 * the LAST byte before the opcode, including immediately before a 0F escape; a
 * legacy prefix appearing after a REX byte discards that REX exactly as the
 * hardware does. The last-seen of F2/F3 wins for mandatory-prefix selection
 * and F2/F3 outrank 66 when both are present; 66 is only the operand-size
 * override when no F2/F3 is in effect for an SSE opcode.
 *
 * ModRM / SIB / displacement follow the standard 64-bit tables. mod=00 rm=101
 * is RIP-relative and is pre-resolved into an absolute address per the header
 * (disp = rip + insn_len + disp32, riprel=1). rm=100 always pulls a SIB byte;
 * index=100 without REX.X means no index; base=101 with mod=00 means a bare
 * disp32 with no base. REX.B extends rm/base and the embedded opcode register,
 * REX.X the index, REX.R the reg field; .scale stores the raw 2-bit SIB scale
 * (already the LOG2 the contract wants).
 *
 * Operand size defaults to 4, becomes 2 under 66, becomes 8 under REX.W (which
 * beats 66). The stack/branch family (PUSH/POP/CALL/RET/JMP/Jcc/LEAVE) defaults
 * to 8 in long mode, droppable to 2 by a 66 prefix. Address size is 8 unless a
 * 67 prefix makes it 4. Immediates are stored already sign-extended to 64 bits
 * except MOV r64,imm64 (B8+r with REX.W) and the moffs forms, which carry the
 * raw 8 bytes.
 *
 * 8-bit register encodings 4..7 with NO REX are the high-byte regs AH/CH/DH/BH
 * (reg = enc-4, high8=1); with any REX present they are SPL/BPL/SIL/DIL.
 *
 * Anything not on the explicit coverage list is rejected with OCERZ_EUNDEF.
 * Deliberately rejected families are listed in the final report: CLI/STI,
 * IN/OUT/INS/OUTS, far CALL/JMP/RET, ARPL, BOUND, AAA/AAS/AAD/AAM/DAA/DAS,
 * segment push/pop, LES/LDS, LGDT/LIDT/LMSW and the rest of 0F 00/01, all MMX
 * register forms (no-66 SSE2 integer twins), the entire VEX/EVEX space, RDRAND/
 * RDSEED, MOVNTPS/streaming-store variants not listed, and SSE3/SSSE3/SSE4
 * opcodes outside the enumerated set.
 *
 * Tables. The bulk of the work is opcode dispatch. To keep it auditable it is
 * written as explicit switch statements per map (one byte, 0F, 0F38, 0F3A, and
 * the eight x87 escapes) rather than data tables, because the per-opcode
 * operand shapes vary too much to compress cleanly and the switch form makes
 * the coverage list directly checkable against the code.
 */
#include "ocerz/decode.h"
#include "ocerz/cpu.h"
#include "ocerz/flags.h"

typedef struct DecState {
    const uint8_t *base;
    const uint8_t *p;
    const uint8_t *end;
    size_t avail;
    uint64_t rip;
    int has_66;
    int has_67;
    int has_f0;
    int rep;
    int mand;
    int seg;
    int rex;
    int rex_present;
    int rex_w;
    int rex_r;
    int rex_x;
    int rex_b;
    X86Insn *out;
} DecState;

enum {
    MAND_NONE = 0,
    MAND_66 = 1,
    MAND_F3 = 2,
    MAND_F2 = 3,
};

static int fetch8(DecState *s, uint8_t *v)
{
    if (s->p >= s->end)
        return OCERZ_ETRUNC;
    *v = *s->p++;
    return OCERZ_OK;
}

static int fetch16(DecState *s, uint16_t *v)
{
    if (s->end - s->p < 2)
        return OCERZ_ETRUNC;
    *v = (uint16_t)(s->p[0] | (s->p[1] << 8));
    s->p += 2;
    return OCERZ_OK;
}

static int fetch32(DecState *s, uint32_t *v)
{
    if (s->end - s->p < 4)
        return OCERZ_ETRUNC;
    *v = (uint32_t)s->p[0] | ((uint32_t)s->p[1] << 8) |
         ((uint32_t)s->p[2] << 16) | ((uint32_t)s->p[3] << 24);
    s->p += 4;
    return OCERZ_OK;
}

static int fetch64(DecState *s, uint64_t *v)
{
    if (s->end - s->p < 8)
        return OCERZ_ETRUNC;
    uint64_t r = 0;
    for (int i = 0; i < 8; i++)
        r |= (uint64_t)s->p[i] << (8 * i);
    s->p += 8;
    *v = r;
    return OCERZ_OK;
}

static void set_reg(X86Operand *op, int reg, int size)
{
    op->kind = OCERZ_OPK_REG;
    op->reg = (uint8_t)reg;
    op->size = (uint8_t)size;
    op->high8 = 0;
    op->base = OCERZ_REG_NONE;
    op->index = OCERZ_REG_NONE;
    op->scale = 0;
    op->riprel = 0;
    op->disp = 0;
    op->imm = 0;
}

static void set_reg8(DecState *s, X86Operand *op, int enc)
{
    op->kind = OCERZ_OPK_REG;
    op->size = 1;
    op->base = OCERZ_REG_NONE;
    op->index = OCERZ_REG_NONE;
    op->scale = 0;
    op->riprel = 0;
    op->disp = 0;
    op->imm = 0;
    if (!s->rex_present && enc >= 4 && enc <= 7) {
        op->reg = (uint8_t)(enc - 4);
        op->high8 = 1;
    } else {
        op->reg = (uint8_t)enc;
        op->high8 = 0;
    }
}

static void set_xmm(X86Operand *op, int reg, int size)
{
    op->kind = OCERZ_OPK_XMM;
    op->reg = (uint8_t)reg;
    op->size = (uint8_t)size;
    op->high8 = 0;
    op->base = OCERZ_REG_NONE;
    op->index = OCERZ_REG_NONE;
    op->scale = 0;
    op->riprel = 0;
    op->disp = 0;
    op->imm = 0;
}

static void set_st(X86Operand *op, int i)
{
    op->kind = OCERZ_OPK_ST;
    op->reg = (uint8_t)i;
    op->size = 10;
    op->high8 = 0;
    op->base = OCERZ_REG_NONE;
    op->index = OCERZ_REG_NONE;
    op->scale = 0;
    op->riprel = 0;
    op->disp = 0;
    op->imm = 0;
}

static void set_imm(X86Operand *op, uint64_t imm, int size)
{
    op->kind = OCERZ_OPK_IMM;
    op->reg = 0;
    op->size = (uint8_t)size;
    op->high8 = 0;
    op->base = OCERZ_REG_NONE;
    op->index = OCERZ_REG_NONE;
    op->scale = 0;
    op->riprel = 0;
    op->disp = 0;
    op->imm = imm;
}

static int read_imm8s(DecState *s, X86Operand *op, int size)
{
    uint8_t b;
    int e = fetch8(s, &b);
    if (e)
        return e;
    set_imm(op, (uint64_t)ocerz_sext(b, 1), size);
    return OCERZ_OK;
}

static int read_imm16(DecState *s, X86Operand *op)
{
    uint16_t w;
    int e = fetch16(s, &w);
    if (e)
        return e;
    set_imm(op, w, 2);
    return OCERZ_OK;
}

static int read_imm32s(DecState *s, X86Operand *op, int size)
{
    uint32_t d;
    int e = fetch32(s, &d);
    if (e)
        return e;
    set_imm(op, (uint64_t)ocerz_sext(d, 4), size);
    return OCERZ_OK;
}

static int read_imm_sized(DecState *s, X86Operand *op, int opsize)
{
    if (opsize == 2)
        return read_imm16(s, op);
    return read_imm32s(s, op, opsize);
}

static int read_imm64(DecState *s, X86Operand *op)
{
    uint64_t q;
    int e = fetch64(s, &q);
    if (e)
        return e;
    set_imm(op, q, 8);
    return OCERZ_OK;
}

static int cur_len(DecState *s)
{
    return (int)(s->p - s->base);
}

typedef struct ModRM {
    int mod;
    int reg;
    int rm;
    X86Operand mem;
} ModRM;

static int decode_modrm(DecState *s, ModRM *m, int mem_size)
{
    uint8_t b;
    int e = fetch8(s, &b);
    if (e)
        return e;
    m->mod = (b >> 6) & 3;
    m->reg = ((b >> 3) & 7) | (s->rex_r ? 8 : 0);
    int rm = b & 7;

    if (m->mod == 3) {
        m->rm = rm | (s->rex_b ? 8 : 0);
        m->mem.kind = OCERZ_OPK_NONE;
        return OCERZ_OK;
    }

    X86Operand *mo = &m->mem;
    mo->kind = OCERZ_OPK_MEM;
    mo->reg = 0;
    mo->size = (uint8_t)mem_size;
    mo->high8 = 0;
    mo->base = OCERZ_REG_NONE;
    mo->index = OCERZ_REG_NONE;
    mo->scale = 0;
    mo->riprel = 0;
    mo->disp = 0;
    mo->imm = 0;
    m->rm = -1;

    int base = -1;
    int has_disp8 = 0;
    int has_disp32 = 0;
    int riprel = 0;

    if (rm == 4) {
        uint8_t sib;
        e = fetch8(s, &sib);
        if (e)
            return e;
        int scale = (sib >> 6) & 3;
        int index = ((sib >> 3) & 7) | (s->rex_x ? 8 : 0);
        int sbase = (sib & 7) | (s->rex_b ? 8 : 0);
        mo->scale = (uint8_t)scale;
        if (((sib >> 3) & 7) == 4 && !s->rex_x)
            mo->index = OCERZ_REG_NONE;
        else
            mo->index = (uint8_t)index;
        if ((sib & 7) == 5 && m->mod == 0) {
            mo->base = OCERZ_REG_NONE;
            has_disp32 = 1;
        } else {
            base = sbase;
        }
    } else if (rm == 5 && m->mod == 0) {
        riprel = 1;
        has_disp32 = 1;
    } else {
        base = rm | (s->rex_b ? 8 : 0);
    }

    if (m->mod == 1)
        has_disp8 = 1;
    else if (m->mod == 2)
        has_disp32 = 1;

    if (base >= 0)
        mo->base = (uint8_t)base;

    int64_t disp = 0;
    if (has_disp8) {
        uint8_t d8;
        e = fetch8(s, &d8);
        if (e)
            return e;
        disp = ocerz_sext(d8, 1);
    } else if (has_disp32) {
        uint32_t d32;
        e = fetch32(s, &d32);
        if (e)
            return e;
        disp = ocerz_sext(d32, 4);
    }

    if (riprel) {
        mo->riprel = 1;
        mo->base = OCERZ_REG_NONE;
        mo->index = OCERZ_REG_NONE;
        mo->disp = disp;
    } else {
        mo->disp = disp;
    }
    return OCERZ_OK;
}

static void fixup_riprel(DecState *s)
{
    for (int i = 0; i < s->out->nops; i++) {
        X86Operand *op = &s->out->ops[i];
        if (op->kind == OCERZ_OPK_MEM && op->riprel)
            op->disp = (int64_t)(s->rip + (uint64_t)cur_len(s) + (uint64_t)op->disp);
    }
}

static void modrm_to_reg(const ModRM *m, X86Operand *op, int size)
{
    set_reg(op, m->rm, size);
}

static void modrm_to_xmm(const ModRM *m, X86Operand *op, int size)
{
    set_xmm(op, m->rm, size);
}

static int rm_is_reg(const ModRM *m)
{
    return m->mod == 3;
}

static void place_rm(DecState *s, const ModRM *m, X86Operand *op, int size, int gpr)
{
    if (rm_is_reg(m)) {
        if (gpr)
            modrm_to_reg(m, op, size);
        else
            modrm_to_xmm(m, op, size);
    } else {
        *op = m->mem;
        op->size = (uint8_t)size;
    }
}

static void place_rm8(DecState *s, const ModRM *m, X86Operand *op)
{
    if (rm_is_reg(m)) {
        set_reg8(s, op, m->rm);
    } else {
        *op = m->mem;
        op->size = 1;
    }
}

static int opsize_default(DecState *s)
{
    if (s->rex_w)
        return 8;
    if (s->has_66)
        return 2;
    return 4;
}

static int opsize_branch(DecState *s)
{
    if (s->has_66)
        return 2;
    return 8;
}

static void set_op(DecState *s, int op)
{
    s->out->op = (uint16_t)op;
}

static int alu_op_for(int idx)
{
    switch (idx) {
    case 0: return OCERZ_OP_ADD;
    case 1: return OCERZ_OP_OR;
    case 2: return OCERZ_OP_ADC;
    case 3: return OCERZ_OP_SBB;
    case 4: return OCERZ_OP_AND;
    case 5: return OCERZ_OP_SUB;
    case 6: return OCERZ_OP_XOR;
    default: return OCERZ_OP_CMP;
    }
}

static int shift_op_for(int idx)
{
    switch (idx) {
    case 0: return OCERZ_OP_ROL;
    case 1: return OCERZ_OP_ROR;
    case 2: return OCERZ_OP_RCL;
    case 3: return OCERZ_OP_RCR;
    case 4: return OCERZ_OP_SHL;
    case 5: return OCERZ_OP_SHR;
    case 6: return OCERZ_OP_SHL;
    default: return OCERZ_OP_SAR;
    }
}

static int alu_rm_r(DecState *s, int idx, int byte_form, int reg_is_dst)
{
    ModRM m;
    int size = byte_form ? 1 : opsize_default(s);
    int e = decode_modrm(s, &m, size);
    if (e)
        return e;
    set_op(s, alu_op_for(idx));
    s->out->opsize = (uint8_t)size;
    s->out->nops = 2;
    if (reg_is_dst) {
        if (byte_form)
            set_reg8(s, &s->out->ops[0], m.reg);
        else
            set_reg(&s->out->ops[0], m.reg, size);
        if (byte_form)
            place_rm8(s, &m, &s->out->ops[1]);
        else
            place_rm(s, &m, &s->out->ops[1], size, 1);
    } else {
        if (byte_form)
            place_rm8(s, &m, &s->out->ops[0]);
        else
            place_rm(s, &m, &s->out->ops[0], size, 1);
        if (byte_form)
            set_reg8(s, &s->out->ops[1], m.reg);
        else
            set_reg(&s->out->ops[1], m.reg, size);
    }
    return OCERZ_OK;
}

static int alu_acc_imm(DecState *s, int idx, int byte_form)
{
    int size = byte_form ? 1 : opsize_default(s);
    set_op(s, alu_op_for(idx));
    s->out->opsize = (uint8_t)size;
    s->out->nops = 2;
    if (byte_form)
        set_reg8(s, &s->out->ops[0], 0);
    else
        set_reg(&s->out->ops[0], 0, size);
    if (byte_form)
        return read_imm8s(s, &s->out->ops[1], 1);
    return read_imm_sized(s, &s->out->ops[1], size);
}

static int set_cc_from_low(DecState *s, int lo4)
{
    s->out->cc = (uint8_t)lo4;
    return OCERZ_OK;
}

static int branch_rel(DecState *s, int op, int rel_size, int lo4_for_cc)
{
    int e;
    X86Operand tmp;
    if (rel_size == 1)
        e = read_imm8s(s, &tmp, 8);
    else
        e = read_imm32s(s, &tmp, 8);
    if (e)
        return e;
    set_op(s, op);
    s->out->opsize = 8;
    if (lo4_for_cc >= 0)
        set_cc_from_low(s, lo4_for_cc);
    uint64_t target = s->rip + (uint64_t)cur_len(s) + tmp.imm;
    set_imm(&s->out->ops[0], target, 8);
    s->out->nops = 1;
    return OCERZ_OK;
}

static int decode_0f(DecState *s, uint8_t op2);
static int decode_0f38(DecState *s);
static int decode_0f3a(DecState *s);
static int decode_x87(DecState *s, uint8_t op);

static int decode_one_byte(DecState *s, uint8_t op);

int ocerz_decode(const uint8_t *code, size_t avail, uint64_t rip, X86Insn *out)
{
    DecState s;
    s.base = code;
    s.p = code;
    s.avail = avail;
    s.end = code + (avail > 16 ? 16 : avail);
    s.rip = rip;
    s.has_66 = 0;
    s.has_67 = 0;
    s.has_f0 = 0;
    s.rep = OCERZ_REP_NONE;
    s.mand = MAND_NONE;
    s.seg = OCERZ_SEG_NONE;
    s.rex = 0;
    s.rex_present = 0;
    s.rex_w = 0;
    s.rex_r = 0;
    s.rex_x = 0;
    s.rex_b = 0;
    s.out = out;

    memset(out, 0, sizeof(*out));
    out->rip = rip;
    for (int i = 0; i < 3; i++) {
        out->ops[i].base = OCERZ_REG_NONE;
        out->ops[i].index = OCERZ_REG_NONE;
    }

    int last_f23 = 0;
    for (;;) {
        if (s.p >= s.end)
            return avail >= 16 ? OCERZ_ETOOLONG : OCERZ_ETRUNC;
        uint8_t b = *s.p;
        if (b >= 0x40 && b <= 0x4f) {
            s.rex_present = 1;
            s.rex = b;
            s.rex_w = (b >> 3) & 1;
            s.rex_r = (b >> 2) & 1;
            s.rex_x = (b >> 1) & 1;
            s.rex_b = b & 1;
            s.p++;
            continue;
        }
        switch (b) {
        case 0x66:
            s.has_66 = 1;
            if (last_f23 == 0)
                s.mand = MAND_66;
            break;
        case 0x67:
            s.has_67 = 1;
            break;
        case 0xf0:
            s.has_f0 = 1;
            break;
        case 0xf2:
            s.rep = OCERZ_REP_REPNE;
            s.mand = MAND_F2;
            last_f23 = 1;
            break;
        case 0xf3:
            s.rep = OCERZ_REP_REP;
            s.mand = MAND_F3;
            last_f23 = 1;
            break;
        case 0x2e:
            s.seg = OCERZ_SEG_NONE;
            break;
        case 0x36:
            s.seg = OCERZ_SEG_NONE;
            break;
        case 0x3e:
            s.seg = OCERZ_SEG_NONE;
            break;
        case 0x26:
            s.seg = OCERZ_SEG_NONE;
            break;
        case 0x64:
            s.seg = OCERZ_SEG_FS;
            break;
        case 0x65:
            s.seg = OCERZ_SEG_GS;
            break;
        default:
            goto prefixes_done;
        }
        s.rex_present = 0;
        s.rex_w = s.rex_r = s.rex_x = s.rex_b = 0;
        s.p++;
    }
prefixes_done:
    out->addrsize = s.has_67 ? 4 : 8;
    out->seg = (uint8_t)s.seg;
    out->lock = (uint8_t)(s.has_f0 ? 1 : 0);
    out->rep = (uint8_t)OCERZ_REP_NONE;

    uint8_t op;
    int e = fetch8(&s, &op);
    int r;
    if (e) {
        r = e;
    } else if (op == 0x0f) {
        uint8_t op2;
        e = fetch8(&s, &op2);
        if (e)
            r = e;
        else if (op2 == 0x38)
            r = decode_0f38(&s);
        else if (op2 == 0x3a)
            r = decode_0f3a(&s);
        else
            r = decode_0f(&s, op2);
    } else if (op >= 0xd8 && op <= 0xdf) {
        r = decode_x87(&s, op);
    } else {
        r = decode_one_byte(&s, op);
    }

    if (r == OCERZ_ETRUNC && avail >= 16)
        return OCERZ_ETOOLONG;
    if (r != OCERZ_OK)
        return r;

    fixup_riprel(&s);

    int len = cur_len(&s);
    if (len > 15)
        return OCERZ_ETOOLONG;
    out->len = (uint8_t)len;
    return OCERZ_OK;
}

static int group1(DecState *s, uint8_t op)
{
    ModRM m;
    int byte_form = (op == 0x80);
    int size = byte_form ? 1 : opsize_default(s);
    int e = decode_modrm(s, &m, size);
    if (e)
        return e;
    set_op(s, alu_op_for(m.reg & 7));
    s->out->opsize = (uint8_t)size;
    s->out->nops = 2;
    if (byte_form)
        place_rm8(s, &m, &s->out->ops[0]);
    else
        place_rm(s, &m, &s->out->ops[0], size, 1);
    if (op == 0x80)
        return read_imm8s(s, &s->out->ops[1], 1);
    if (op == 0x83)
        return read_imm8s(s, &s->out->ops[1], size);
    return read_imm_sized(s, &s->out->ops[1], size);
}

static int group2(DecState *s, uint8_t op)
{
    ModRM m;
    int byte_form = (op == 0xc0 || op == 0xd0 || op == 0xd2);
    int size = byte_form ? 1 : opsize_default(s);
    int e = decode_modrm(s, &m, size);
    if (e)
        return e;
    set_op(s, shift_op_for(m.reg & 7));
    s->out->opsize = (uint8_t)size;
    s->out->nops = 2;
    if (byte_form)
        place_rm8(s, &m, &s->out->ops[0]);
    else
        place_rm(s, &m, &s->out->ops[0], size, 1);
    if (op == 0xc0 || op == 0xc1) {
        return read_imm8s(s, &s->out->ops[1], 1);
    } else if (op == 0xd0 || op == 0xd1) {
        set_imm(&s->out->ops[1], 1, 1);
        return OCERZ_OK;
    } else {
        set_reg(&s->out->ops[1], OCERZ_RCX, 1);
        return OCERZ_OK;
    }
}

static int group3(DecState *s, uint8_t op)
{
    ModRM m;
    int byte_form = (op == 0xf6);
    int size = byte_form ? 1 : opsize_default(s);
    int e = decode_modrm(s, &m, size);
    if (e)
        return e;
    int idx = m.reg & 7;
    s->out->opsize = (uint8_t)size;
    switch (idx) {
    case 0:
    case 1:
        set_op(s, OCERZ_OP_TEST);
        s->out->nops = 2;
        if (byte_form) {
            place_rm8(s, &m, &s->out->ops[0]);
            return read_imm8s(s, &s->out->ops[1], 1);
        }
        place_rm(s, &m, &s->out->ops[0], size, 1);
        return read_imm_sized(s, &s->out->ops[1], size);
    case 2:
        set_op(s, OCERZ_OP_NOT);
        break;
    case 3:
        set_op(s, OCERZ_OP_NEG);
        break;
    case 4:
        set_op(s, OCERZ_OP_MUL);
        break;
    case 5:
        set_op(s, OCERZ_OP_IMUL);
        break;
    case 6:
        set_op(s, OCERZ_OP_DIV);
        break;
    default:
        set_op(s, OCERZ_OP_IDIV);
        break;
    }
    s->out->nops = 1;
    if (byte_form)
        place_rm8(s, &m, &s->out->ops[0]);
    else
        place_rm(s, &m, &s->out->ops[0], size, 1);
    return OCERZ_OK;
}

static int group45(DecState *s, uint8_t op)
{
    ModRM m;
    if (op == 0xfe) {
        int e = decode_modrm(s, &m, 1);
        if (e)
            return e;
        int idx = m.reg & 7;
        if (idx == 0)
            set_op(s, OCERZ_OP_INC);
        else if (idx == 1)
            set_op(s, OCERZ_OP_DEC);
        else
            return OCERZ_EUNDEF;
        s->out->opsize = 1;
        s->out->nops = 1;
        place_rm8(s, &m, &s->out->ops[0]);
        return OCERZ_OK;
    }

    int size = opsize_default(s);
    int e = decode_modrm(s, &m, size);
    if (e)
        return e;
    int idx = m.reg & 7;
    switch (idx) {
    case 0:
        set_op(s, OCERZ_OP_INC);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        place_rm(s, &m, &s->out->ops[0], size, 1);
        return OCERZ_OK;
    case 1:
        set_op(s, OCERZ_OP_DEC);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        place_rm(s, &m, &s->out->ops[0], size, 1);
        return OCERZ_OK;
    case 2:
        set_op(s, OCERZ_OP_CALL);
        s->out->opsize = 8;
        s->out->nops = 1;
        place_rm(s, &m, &s->out->ops[0], 8, 1);
        return OCERZ_OK;
    case 4:
        set_op(s, OCERZ_OP_JMP);
        s->out->opsize = 8;
        s->out->nops = 1;
        place_rm(s, &m, &s->out->ops[0], 8, 1);
        return OCERZ_OK;
    case 6:
        set_op(s, OCERZ_OP_PUSH);
        s->out->opsize = (uint8_t)opsize_branch(s);
        s->out->nops = 1;
        place_rm(s, &m, &s->out->ops[0], opsize_branch(s), 1);
        return OCERZ_OK;
    case 3:
    case 5: {
        /* Far CALL (/3) and far JMP (/5), m16:32 or m16:64. The MEMORY form is the only
         * legal one -- mod==3 is undefined -- and it is what wow64cpu.dll uses to enter
         * 32-bit code (41 ff 2e = REX.B + FF /5 through [r14]).
         *
         * The operand is recorded as a plain MEM of the OFFSET width (4, or 8 under REX.W),
         * not as a 6/10-byte pointer: the far-ness is carried by the opcode, so no other
         * code path ever sees an operand with an unusual size it would mis-handle. The
         * selector lives immediately after the offset, at ea + opsize. */
        if (rm_is_reg(&m))
            return OCERZ_EUNDEF;
        set_op(s, idx == 3 ? OCERZ_OP_CALLF : OCERZ_OP_JMPF);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        place_rm(s, &m, &s->out->ops[0], size, 1);
        return OCERZ_OK;
    }
    default:
        return OCERZ_EUNDEF;
    }
}

static int decode_one_byte(DecState *s, uint8_t op)
{
    int e;

    if (op <= 0x3d) {
        int hi = op >> 3;
        int lo = op & 7;
        if (hi <= 7 && lo <= 5) {
            switch (lo) {
            case 0:
                return alu_rm_r(s, hi, 1, 0);
            case 1:
                return alu_rm_r(s, hi, 0, 0);
            case 2:
                return alu_rm_r(s, hi, 1, 1);
            case 3:
                return alu_rm_r(s, hi, 0, 1);
            case 4:
                return alu_acc_imm(s, hi, 1);
            default:
                return alu_acc_imm(s, hi, 0);
            }
        }
        return OCERZ_EUNDEF;
    }

    if (op >= 0x50 && op <= 0x57) {
        int reg = (op - 0x50) | (s->rex_b ? 8 : 0);
        int size = opsize_branch(s);
        set_op(s, OCERZ_OP_PUSH);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        set_reg(&s->out->ops[0], reg, size);
        return OCERZ_OK;
    }
    if (op >= 0x58 && op <= 0x5f) {
        int reg = (op - 0x58) | (s->rex_b ? 8 : 0);
        int size = opsize_branch(s);
        set_op(s, OCERZ_OP_POP);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        set_reg(&s->out->ops[0], reg, size);
        return OCERZ_OK;
    }

    switch (op) {
    case 0x63: {
        ModRM m;
        int dsize = s->rex_w ? 8 : 4;
        e = decode_modrm(s, &m, 4);
        if (e)
            return e;
        set_op(s, OCERZ_OP_MOVSXD);
        s->out->opsize = (uint8_t)dsize;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, dsize);
        place_rm(s, &m, &s->out->ops[1], 4, 1);
        return OCERZ_OK;
    }
    case 0x68: {
        int size = opsize_branch(s);
        set_op(s, OCERZ_OP_PUSH);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        return read_imm_sized(s, &s->out->ops[0], size);
    }
    case 0x6a: {
        int size = opsize_branch(s);
        set_op(s, OCERZ_OP_PUSH);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        return read_imm8s(s, &s->out->ops[0], size);
    }
    case 0x69:
    case 0x6b: {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_IMUL);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 3;
        set_reg(&s->out->ops[0], m.reg, size);
        place_rm(s, &m, &s->out->ops[1], size, 1);
        if (op == 0x6b)
            return read_imm8s(s, &s->out->ops[2], size);
        return read_imm_sized(s, &s->out->ops[2], size);
    }
    }

    if (op >= 0x70 && op <= 0x7f)
        return branch_rel(s, OCERZ_OP_JCC, 1, op & 0xf);

    switch (op) {
    case 0x80:
    case 0x81:
    case 0x83:
        return group1(s, op);
    case 0x84:
    case 0x85: {
        ModRM m;
        int byte_form = (op == 0x84);
        int size = byte_form ? 1 : opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_TEST);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        if (byte_form) {
            place_rm8(s, &m, &s->out->ops[0]);
            set_reg8(s, &s->out->ops[1], m.reg);
        } else {
            place_rm(s, &m, &s->out->ops[0], size, 1);
            set_reg(&s->out->ops[1], m.reg, size);
        }
        return OCERZ_OK;
    }
    case 0x86:
    case 0x87: {
        ModRM m;
        int byte_form = (op == 0x86);
        int size = byte_form ? 1 : opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_XCHG);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        if (byte_form) {
            place_rm8(s, &m, &s->out->ops[0]);
            set_reg8(s, &s->out->ops[1], m.reg);
        } else {
            place_rm(s, &m, &s->out->ops[0], size, 1);
            set_reg(&s->out->ops[1], m.reg, size);
        }
        return OCERZ_OK;
    }
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b: {
        ModRM m;
        int byte_form = (op == 0x88 || op == 0x8a);
        int reg_is_dst = (op == 0x8a || op == 0x8b);
        int size = byte_form ? 1 : opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_MOV);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        if (reg_is_dst) {
            if (byte_form) {
                set_reg8(s, &s->out->ops[0], m.reg);
                place_rm8(s, &m, &s->out->ops[1]);
            } else {
                set_reg(&s->out->ops[0], m.reg, size);
                place_rm(s, &m, &s->out->ops[1], size, 1);
            }
        } else {
            if (byte_form) {
                place_rm8(s, &m, &s->out->ops[0]);
                set_reg8(s, &s->out->ops[1], m.reg);
            } else {
                place_rm(s, &m, &s->out->ops[0], size, 1);
                set_reg(&s->out->ops[1], m.reg, size);
            }
        }
        return OCERZ_OK;
    }
    case 0x8c: {
        /* MOV r/m16, Sreg — store a segment SELECTOR. x86_64 has no real
         * segmentation; the selectors are fixed constants in macOS user mode
         * (CS=0x2b, SS=0x23, the rest 0), which Wine reads to fill a Windows
         * CONTEXT. Emit the constant as a 16-bit immediate MOV into r/m. */
        static const uint16_t seg_sel[8] = {
            0x00, 0x2b, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        ModRM m;
        e = decode_modrm(s, &m, 2);
        if (e)
            return e;
        set_op(s, OCERZ_OP_MOV);
        s->out->opsize = 2;
        s->out->nops = 2;
        place_rm(s, &m, &s->out->ops[0], 2, 1);
        set_imm(&s->out->ops[1], seg_sel[m.reg & 7], 2);
        return OCERZ_OK;
    }
    case 0x8e: {
        /* MOV Sreg, r/m16 — loading a selector is a no-op in 64-bit user mode
         * (FS/GS bases are set via the machdep TLS syscall, not the selector;
         * the rest are inert). Consume the ModRM and do nothing. */
        ModRM m;
        e = decode_modrm(s, &m, 2);
        if (e)
            return e;
        set_op(s, OCERZ_OP_NOP);
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0x8d: {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        if (rm_is_reg(&m))
            return OCERZ_EUNDEF;
        set_op(s, OCERZ_OP_LEA);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, size);
        s->out->ops[1] = m.mem;
        s->out->ops[1].size = (uint8_t)size;
        return OCERZ_OK;
    }
    case 0x8f: {
        ModRM m;
        int size = opsize_branch(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        if ((m.reg & 7) != 0)
            return OCERZ_EUNDEF;
        set_op(s, OCERZ_OP_POP);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        place_rm(s, &m, &s->out->ops[0], size, 1);
        return OCERZ_OK;
    }
    case 0x90:
        if (s->mand == MAND_F3) {
            set_op(s, OCERZ_OP_PAUSE);
            s->out->nops = 0;
            return OCERZ_OK;
        }
        if (s->rex_b) {
            int size = opsize_default(s);
            set_op(s, OCERZ_OP_XCHG);
            s->out->opsize = (uint8_t)size;
            s->out->nops = 2;
            set_reg(&s->out->ops[0], OCERZ_RAX, size);
            set_reg(&s->out->ops[1], 8, size);
            return OCERZ_OK;
        }
        set_op(s, OCERZ_OP_NOP);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97: {
        int size = opsize_default(s);
        int reg = (op - 0x90) | (s->rex_b ? 8 : 0);
        set_op(s, OCERZ_OP_XCHG);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], OCERZ_RAX, size);
        set_reg(&s->out->ops[1], reg, size);
        return OCERZ_OK;
    }
    case 0x98: {
        int size = opsize_default(s);
        set_op(s, OCERZ_OP_CBW);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0x99: {
        int size = opsize_default(s);
        set_op(s, OCERZ_OP_CWD);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0x9b:
        set_op(s, OCERZ_OP_FWAIT);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0x9c:
        set_op(s, OCERZ_OP_PUSHF);
        s->out->opsize = 8;
        s->out->nops = 0;
        return OCERZ_OK;
    case 0x9d:
        set_op(s, OCERZ_OP_POPF);
        s->out->opsize = 8;
        s->out->nops = 0;
        return OCERZ_OK;
    case 0x9e:
        set_op(s, OCERZ_OP_SAHF);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0x9f:
        set_op(s, OCERZ_OP_LAHF);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xa0:
    case 0xa1:
    case 0xa2:
    case 0xa3: {
        int byte_form = (op == 0xa0 || op == 0xa2);
        int size = byte_form ? 1 : opsize_default(s);
        int store = (op == 0xa2 || op == 0xa3);
        uint64_t off;
        e = fetch64(s, &off);
        if (e)
            return e;
        set_op(s, OCERZ_OP_MOV);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        X86Operand mem;
        mem.kind = OCERZ_OPK_MEM;
        mem.reg = 0;
        mem.size = (uint8_t)size;
        mem.high8 = 0;
        mem.base = OCERZ_REG_NONE;
        mem.index = OCERZ_REG_NONE;
        mem.scale = 0;
        mem.riprel = 0;
        mem.disp = (int64_t)off;
        mem.imm = 0;
        X86Operand acc;
        if (byte_form)
            set_reg8(s, &acc, 0);
        else
            set_reg(&acc, 0, size);
        if (store) {
            s->out->ops[0] = mem;
            s->out->ops[1] = acc;
        } else {
            s->out->ops[0] = acc;
            s->out->ops[1] = mem;
        }
        return OCERZ_OK;
    }
    case 0xa4:
    case 0xa5: {
        int size = (op == 0xa4) ? 1 : opsize_default(s);
        set_op(s, OCERZ_OP_MOVS);
        s->out->opsize = (uint8_t)size;
        s->out->rep = (uint8_t)s->rep;
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0xa6:
    case 0xa7: {
        int size = (op == 0xa6) ? 1 : opsize_default(s);
        set_op(s, OCERZ_OP_CMPS);
        s->out->opsize = (uint8_t)size;
        s->out->rep = (uint8_t)s->rep;
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0xa8:
    case 0xa9: {
        int byte_form = (op == 0xa8);
        int size = byte_form ? 1 : opsize_default(s);
        set_op(s, OCERZ_OP_TEST);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        if (byte_form) {
            set_reg8(s, &s->out->ops[0], 0);
            return read_imm8s(s, &s->out->ops[1], 1);
        }
        set_reg(&s->out->ops[0], 0, size);
        return read_imm_sized(s, &s->out->ops[1], size);
    }
    case 0xaa:
    case 0xab: {
        int size = (op == 0xaa) ? 1 : opsize_default(s);
        set_op(s, OCERZ_OP_STOS);
        s->out->opsize = (uint8_t)size;
        s->out->rep = (uint8_t)s->rep;
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0xac:
    case 0xad: {
        int size = (op == 0xac) ? 1 : opsize_default(s);
        set_op(s, OCERZ_OP_LODS);
        s->out->opsize = (uint8_t)size;
        s->out->rep = (uint8_t)s->rep;
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0xae:
    case 0xaf: {
        int size = (op == 0xae) ? 1 : opsize_default(s);
        set_op(s, OCERZ_OP_SCAS);
        s->out->opsize = (uint8_t)size;
        s->out->rep = (uint8_t)s->rep;
        s->out->nops = 0;
        return OCERZ_OK;
    }
    }

    if (op >= 0xb0 && op <= 0xb7) {
        int reg = (op - 0xb0) | (s->rex_b ? 8 : 0);
        set_op(s, OCERZ_OP_MOV);
        s->out->opsize = 1;
        s->out->nops = 2;
        if (s->rex_present || reg >= 8)
            set_reg(&s->out->ops[0], reg, 1);
        else
            set_reg8(s, &s->out->ops[0], op - 0xb0);
        return read_imm8s(s, &s->out->ops[1], 1);
    }
    if (op >= 0xb8 && op <= 0xbf) {
        int reg = (op - 0xb8) | (s->rex_b ? 8 : 0);
        int size = opsize_default(s);
        set_op(s, OCERZ_OP_MOV);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], reg, size);
        if (size == 8)
            return read_imm64(s, &s->out->ops[1]);
        if (size == 2)
            return read_imm16(s, &s->out->ops[1]);
        uint32_t d;
        e = fetch32(s, &d);
        if (e)
            return e;
        set_imm(&s->out->ops[1], d, 4);
        return OCERZ_OK;
    }

    switch (op) {
    case 0xc0:
    case 0xc1:
        return group2(s, op);
    case 0xc2: {
        set_op(s, OCERZ_OP_RET);
        s->out->opsize = 8;
        s->out->nops = 1;
        uint16_t w;
        e = fetch16(s, &w);
        if (e)
            return e;
        set_imm(&s->out->ops[0], w, 2);
        return OCERZ_OK;
    }
    case 0xc3:
        set_op(s, OCERZ_OP_RET);
        s->out->opsize = 8;
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xca:                      /* RETF imm16: far return, then pop imm16 bytes */
        set_op(s, OCERZ_OP_RETF);
        s->out->opsize = (uint8_t)opsize_default(s);
        s->out->nops = 1;
        return read_imm16(s, &s->out->ops[0]);
    case 0xcb:                      /* RETF */
        set_op(s, OCERZ_OP_RETF);
        s->out->opsize = (uint8_t)opsize_default(s);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xcf:
        /* IRETD/IRETQ: pop the interrupt frame (RIP, CS, RFLAGS, RSP, SS),
         * element size = operand size (8 with REX.W = IRETQ, else 4). Wine's
         * NtContinue / syscall-dispatcher build a fake iret frame and use this
         * to restore a full register context. */
        set_op(s, OCERZ_OP_IRET);
        s->out->opsize = (uint8_t)opsize_default(s);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xc6:
    case 0xc7: {
        ModRM m;
        int byte_form = (op == 0xc6);
        int size = byte_form ? 1 : opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        if ((m.reg & 7) != 0)
            return OCERZ_EUNDEF;
        set_op(s, OCERZ_OP_MOV);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        if (byte_form) {
            place_rm8(s, &m, &s->out->ops[0]);
            return read_imm8s(s, &s->out->ops[1], 1);
        }
        place_rm(s, &m, &s->out->ops[0], size, 1);
        return read_imm_sized(s, &s->out->ops[1], size);
    }
    case 0xc9:
        set_op(s, OCERZ_OP_LEAVE);
        s->out->opsize = 8;
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xcc:
        set_op(s, OCERZ_OP_INT3);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xcd: {
        set_op(s, OCERZ_OP_INT);
        s->out->nops = 1;
        uint8_t b;
        e = fetch8(s, &b);
        if (e)
            return e;
        set_imm(&s->out->ops[0], b, 1);
        return OCERZ_OK;
    }
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
        return group2(s, op);
    case 0xe0:
        return branch_rel(s, OCERZ_OP_LOOPNE, 1, -1);
    case 0xe1:
        return branch_rel(s, OCERZ_OP_LOOPE, 1, -1);
    case 0xe2:
        return branch_rel(s, OCERZ_OP_LOOP, 1, -1);
    case 0xe3:
        return branch_rel(s, OCERZ_OP_JRCXZ, 1, -1);
    case 0xe8:
        return branch_rel(s, OCERZ_OP_CALL, 4, -1);
    case 0xe9:
        return branch_rel(s, OCERZ_OP_JMP, 4, -1);
    case 0xeb:
        return branch_rel(s, OCERZ_OP_JMP, 1, -1);
    case 0xf4:
        set_op(s, OCERZ_OP_HLT);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xf5:
        set_op(s, OCERZ_OP_CMC);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xf6:
    case 0xf7:
        return group3(s, op);
    case 0xf8:
        set_op(s, OCERZ_OP_CLC);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xf9:
        set_op(s, OCERZ_OP_STC);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xfc:
        set_op(s, OCERZ_OP_CLD);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xfd:
        set_op(s, OCERZ_OP_STD);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xfe:
    case 0xff:
        return group45(s, op);
    }

    return OCERZ_EUNDEF;
}

static int sse_prefix(DecState *s)
{
    return s->mand;
}

static int decode_sse_rr(DecState *s, int op, int size, int reg_is_dst)
{
    ModRM m;
    int e = decode_modrm(s, &m, size);
    if (e)
        return e;
    set_op(s, op);
    s->out->opsize = (uint8_t)size;
    s->out->nops = 2;
    if (reg_is_dst) {
        set_xmm(&s->out->ops[0], m.reg, size);
        place_rm(s, &m, &s->out->ops[1], size, 0);
    } else {
        place_rm(s, &m, &s->out->ops[0], size, 0);
        set_xmm(&s->out->ops[1], m.reg, size);
    }
    return OCERZ_OK;
}

static int decode_sse_rri(DecState *s, int op, int size, int reg_is_dst)
{
    int e = decode_sse_rr(s, op, size, reg_is_dst);
    if (e)
        return e;
    s->out->nops = 3;
    return read_imm8s(s, &s->out->ops[2], 1);
}

static int select_packed(DecState *s, int ps, int pd, int ss, int sd)
{
    switch (sse_prefix(s)) {
    case MAND_NONE: return ps;
    case MAND_66: return pd;
    case MAND_F3: return ss;
    default: return sd;
    }
}

static int sse_scalar_size(DecState *s, int packed_def)
{
    switch (sse_prefix(s)) {
    case MAND_F3: return 4;
    case MAND_F2: return 8;
    default: return packed_def;
    }
}

static int decode_arith_sse(DecState *s, int ps, int pd, int ss, int sd)
{
    int op = select_packed(s, ps, pd, ss, sd);
    if (op < 0)
        return OCERZ_EUNDEF;
    int size = sse_scalar_size(s, 16);
    return decode_sse_rr(s, op, size, 1);
}

static int decode_pint(DecState *s, int op, int reg_is_dst)
{
    if (sse_prefix(s) != MAND_66)
        return OCERZ_EUNDEF;
    return decode_sse_rr(s, op, 16, reg_is_dst);
}

static int decode_pint_imm(DecState *s, int op)
{
    if (sse_prefix(s) != MAND_66)
        return OCERZ_EUNDEF;
    return decode_sse_rri(s, op, 16, 1);
}

static int decode_0f(DecState *s, uint8_t op2)
{
    int e;
    int mand = sse_prefix(s);

    switch (op2) {
    case 0x01: {
        uint8_t modrm;
        e = fetch8(s, &modrm);
        if (e)
            return e;
        if (modrm == 0xd0) {
            s->out->nops = 0;
            set_op(s, OCERZ_OP_XGETBV);
            return OCERZ_OK;
        }
        if (modrm == 0xf9) {
            s->out->nops = 0;
            set_op(s, OCERZ_OP_RDTSCP);
            return OCERZ_OK;
        }
        int grpreg = (modrm >> 3) & 7;
        if (modrm < 0xc0 && (grpreg == 0 || grpreg == 1)) {
            ModRM m;
            s->p -= 1;
            e = decode_modrm(s, &m, 10);
            if (e)
                return e;
            set_op(s, grpreg == 0 ? OCERZ_OP_SGDT : OCERZ_OP_SIDT);
            s->out->nops = 1;
            s->out->ops[0] = m.mem;
            s->out->ops[0].size = 10;
            return OCERZ_OK;
        }
        s->out->nops = 0;
        return OCERZ_EUNDEF;
    }
    case 0x05:
        set_op(s, OCERZ_OP_SYSCALL);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0x0b:
        set_op(s, OCERZ_OP_UD2);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xb9: {
        ModRM m;
        e = decode_modrm(s, &m, 4);
        if (e)
            return e;
        set_op(s, OCERZ_OP_UD2);
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0x0d: {
        ModRM m;
        e = decode_modrm(s, &m, 1);
        if (e)
            return e;
        set_op(s, OCERZ_OP_PREFETCH);
        s->out->nops = 1;
        if (rm_is_reg(&m)) {
            set_reg(&s->out->ops[0], m.rm, 1);
        } else {
            s->out->ops[0] = m.mem;
            s->out->ops[0].size = 1;
        }
        return OCERZ_OK;
    }
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x1c:
    case 0x1d:
    case 0x1e:
    case 0x1f: {
        ModRM m;
        e = decode_modrm(s, &m, 1);
        if (e)
            return e;
        set_op(s, OCERZ_OP_NOP);
        s->out->nops = 0;
        return OCERZ_OK;
    }
    case 0x10:
    case 0x11: {
        int reg_is_dst = (op2 == 0x10);
        int op;
        int size;
        switch (mand) {
        case MAND_F3:
            op = OCERZ_OP_MOVSS;
            size = 4;
            break;
        case MAND_F2:
            op = OCERZ_OP_MOVSDX;
            size = 8;
            break;
        default:
            op = OCERZ_OP_MOVUPS;
            size = 16;
            break;
        }
        return decode_sse_rr(s, op, size, reg_is_dst);
    }
    case 0x12: {
        ModRM m;
        int op;
        int size = (mand == MAND_F2) ? 8 : 16;
        switch (mand) {
        case MAND_66:
            op = OCERZ_OP_MOVLPS;
            break;
        case MAND_F2:
            op = OCERZ_OP_MOVDDUP;
            break;
        case MAND_F3:
            op = OCERZ_OP_MOVSLDUP;
            break;
        default:
            op = OCERZ_OP_MOVLPS;
            break;
        }
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        if (mand == MAND_NONE && rm_is_reg(&m))
            op = OCERZ_OP_MOVHLPS;
        set_op(s, op);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        set_xmm(&s->out->ops[0], m.reg, size);
        place_rm(s, &m, &s->out->ops[1], size, 0);
        return OCERZ_OK;
    }
    case 0x13: {
        if (mand != MAND_NONE && mand != MAND_66)
            return OCERZ_EUNDEF;
        return decode_sse_rr(s, OCERZ_OP_MOVLPS, 16, 0);
    }
    case 0x14:
        return decode_sse_rr(s, (mand == MAND_66) ? OCERZ_OP_UNPCKLPD : OCERZ_OP_UNPCKLPS, 16, 1);
    case 0x15:
        return decode_sse_rr(s, (mand == MAND_66) ? OCERZ_OP_UNPCKHPD : OCERZ_OP_UNPCKHPS, 16, 1);
    case 0x16: {
        ModRM m;
        int op;
        switch (mand) {
        case MAND_66:
            op = OCERZ_OP_MOVHPS;
            break;
        case MAND_F3:
            op = OCERZ_OP_MOVSHDUP;
            break;
        default:
            op = OCERZ_OP_MOVHPS;
            break;
        }
        e = decode_modrm(s, &m, 16);
        if (e)
            return e;
        if (mand == MAND_NONE && rm_is_reg(&m))
            op = OCERZ_OP_MOVLHPS;
        set_op(s, op);
        s->out->opsize = 16;
        s->out->nops = 2;
        set_xmm(&s->out->ops[0], m.reg, 16);
        place_rm(s, &m, &s->out->ops[1], 16, 0);
        return OCERZ_OK;
    }
    case 0x17:
        if (mand != MAND_NONE && mand != MAND_66)
            return OCERZ_EUNDEF;
        return decode_sse_rr(s, OCERZ_OP_MOVHPS, 16, 0);
    case 0x28:
        return decode_sse_rr(s, OCERZ_OP_MOVAPS, 16, 1);
    case 0x29:
        return decode_sse_rr(s, OCERZ_OP_MOVAPS, 16, 0);
    case 0x2a: {
        if (mand != MAND_F3 && mand != MAND_F2)
            return OCERZ_EUNDEF;
        ModRM m;
        int gsize = s->rex_w ? 8 : 4;
        e = decode_modrm(s, &m, gsize);
        if (e)
            return e;
        set_op(s, (mand == MAND_F3) ? OCERZ_OP_CVTSI2SS : OCERZ_OP_CVTSI2SD);
        s->out->opsize = (mand == MAND_F3) ? 4 : 8;
        s->out->nops = 2;
        set_xmm(&s->out->ops[0], m.reg, (mand == MAND_F3) ? 4 : 8);
        place_rm(s, &m, &s->out->ops[1], gsize, 1);
        return OCERZ_OK;
    }
    case 0x2c:
    case 0x2d: {
        if (mand != MAND_F3 && mand != MAND_F2)
            return OCERZ_EUNDEF;
        ModRM m;
        int gsize = s->rex_w ? 8 : 4;
        int ssize = (mand == MAND_F3) ? 4 : 8;
        e = decode_modrm(s, &m, ssize);
        if (e)
            return e;
        int op;
        if (op2 == 0x2c)
            op = (mand == MAND_F3) ? OCERZ_OP_CVTTSS2SI : OCERZ_OP_CVTTSD2SI;
        else
            op = (mand == MAND_F3) ? OCERZ_OP_CVTSS2SI : OCERZ_OP_CVTSD2SI;
        set_op(s, op);
        s->out->opsize = (uint8_t)gsize;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, gsize);
        place_rm(s, &m, &s->out->ops[1], ssize, 0);
        return OCERZ_OK;
    }
    case 0x2e:
        return decode_sse_rr(s, (mand == MAND_66) ? OCERZ_OP_UCOMISD : OCERZ_OP_UCOMISS,
                             (mand == MAND_66) ? 8 : 4, 1);
    case 0x2f:
        return decode_sse_rr(s, (mand == MAND_66) ? OCERZ_OP_COMISD : OCERZ_OP_COMISS,
                             (mand == MAND_66) ? 8 : 4, 1);
    case 0x31:
        set_op(s, OCERZ_OP_RDTSC);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xa2:
        set_op(s, OCERZ_OP_CPUID);
        s->out->nops = 0;
        return OCERZ_OK;
    }

    if (op2 >= 0x40 && op2 <= 0x4f) {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_CMOVCC);
        s->out->opsize = (uint8_t)size;
        s->out->cc = (uint8_t)(op2 & 0xf);
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, size);
        place_rm(s, &m, &s->out->ops[1], size, 1);
        return OCERZ_OK;
    }

    if (op2 >= 0x80 && op2 <= 0x8f)
        return branch_rel(s, OCERZ_OP_JCC, 4, op2 & 0xf);

    if (op2 >= 0x90 && op2 <= 0x9f) {
        ModRM m;
        e = decode_modrm(s, &m, 1);
        if (e)
            return e;
        set_op(s, OCERZ_OP_SETCC);
        s->out->opsize = 1;
        s->out->cc = (uint8_t)(op2 & 0xf);
        s->out->nops = 1;
        place_rm8(s, &m, &s->out->ops[0]);
        return OCERZ_OK;
    }

    switch (op2) {
    case 0x1e:
        if (mand == MAND_F3) {
            uint8_t b;
            if (s->p < s->end && *s->p == 0xfa) {
                e = fetch8(s, &b);
                if (e)
                    return e;
                set_op(s, OCERZ_OP_NOP);
                s->out->nops = 0;
                return OCERZ_OK;
            }
        }
        return OCERZ_EUNDEF;
    case 0x50: {
        ModRM m;
        e = decode_modrm(s, &m, 4);
        if (e)
            return e;
        if (!rm_is_reg(&m))
            return OCERZ_EUNDEF;
        set_op(s, (mand == MAND_66) ? OCERZ_OP_MOVMSKPD : OCERZ_OP_MOVMSKPS);
        s->out->opsize = 4;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, s->rex_w ? 8 : 4);
        set_xmm(&s->out->ops[1], m.rm, 16);
        return OCERZ_OK;
    }
    case 0x51:
        return decode_arith_sse(s, OCERZ_OP_SQRTPS, OCERZ_OP_SQRTPD, OCERZ_OP_SQRTSS, OCERZ_OP_SQRTSD);
    case 0x52:
        if (mand == MAND_F3)
            return decode_sse_rr(s, OCERZ_OP_RSQRTSS, 4, 1);
        if (mand == MAND_NONE)
            return decode_sse_rr(s, OCERZ_OP_RSQRTPS, 16, 1);
        return OCERZ_EUNDEF;
    case 0x53:
        if (mand == MAND_F3)
            return decode_sse_rr(s, OCERZ_OP_RCPSS, 4, 1);
        if (mand == MAND_NONE)
            return decode_sse_rr(s, OCERZ_OP_RCPPS, 16, 1);
        return OCERZ_EUNDEF;
    case 0x54:
        return decode_sse_rr(s, OCERZ_OP_ANDPS, 16, 1);
    case 0x55:
        return decode_sse_rr(s, OCERZ_OP_ANDNPS, 16, 1);
    case 0x56:
        return decode_sse_rr(s, OCERZ_OP_ORPS, 16, 1);
    case 0x57:
        return decode_sse_rr(s, OCERZ_OP_XORPS, 16, 1);
    case 0x58:
        return decode_arith_sse(s, OCERZ_OP_ADDPS, OCERZ_OP_ADDPD, OCERZ_OP_ADDSS, OCERZ_OP_ADDSD);
    case 0x59:
        return decode_arith_sse(s, OCERZ_OP_MULPS, OCERZ_OP_MULPD, OCERZ_OP_MULSS, OCERZ_OP_MULSD);
    case 0x5c:
        return decode_arith_sse(s, OCERZ_OP_SUBPS, OCERZ_OP_SUBPD, OCERZ_OP_SUBSS, OCERZ_OP_SUBSD);
    case 0x5d:
        return decode_arith_sse(s, OCERZ_OP_MINPS, OCERZ_OP_MINPD, OCERZ_OP_MINSS, OCERZ_OP_MINSD);
    case 0x5e:
        return decode_arith_sse(s, OCERZ_OP_DIVPS, OCERZ_OP_DIVPD, OCERZ_OP_DIVSS, OCERZ_OP_DIVSD);
    case 0x5f:
        return decode_arith_sse(s, OCERZ_OP_MAXPS, OCERZ_OP_MAXPD, OCERZ_OP_MAXSS, OCERZ_OP_MAXSD);
    case 0x7c:
        if (mand == MAND_66)
            return decode_sse_rr(s, OCERZ_OP_HADDPD, 16, 1);
        if (mand == MAND_F2)
            return decode_sse_rr(s, OCERZ_OP_HADDPS, 16, 1);
        return OCERZ_EUNDEF;
    case 0x7d:
        if (mand == MAND_66)
            return decode_sse_rr(s, OCERZ_OP_HSUBPD, 16, 1);
        if (mand == MAND_F2)
            return decode_sse_rr(s, OCERZ_OP_HSUBPS, 16, 1);
        return OCERZ_EUNDEF;
    case 0xd0: /* SSE3 ADDSUBPD (66) / ADDSUBPS (F2): subtract even lanes, add odd lanes */
        if (mand == MAND_66)
            return decode_sse_rr(s, OCERZ_OP_ADDSUBPD, 16, 1);
        if (mand == MAND_F2)
            return decode_sse_rr(s, OCERZ_OP_ADDSUBPS, 16, 1);
        return OCERZ_EUNDEF;
    case 0x5a: {
        int op;
        int isize, osize;
        switch (mand) {
        case MAND_66:
            op = OCERZ_OP_CVTPD2PS;
            isize = osize = 16;
            break;
        case MAND_F3:
            op = OCERZ_OP_CVTSS2SD;
            isize = 4;
            osize = 8;
            break;
        case MAND_F2:
            op = OCERZ_OP_CVTSD2SS;
            isize = 8;
            osize = 4;
            break;
        default:
            op = OCERZ_OP_CVTPS2PD;
            isize = osize = 16;
            break;
        }
        ModRM m;
        e = decode_modrm(s, &m, isize);
        if (e)
            return e;
        set_op(s, op);
        s->out->opsize = (uint8_t)osize;
        s->out->nops = 2;
        set_xmm(&s->out->ops[0], m.reg, osize);
        place_rm(s, &m, &s->out->ops[1], isize, 0);
        return OCERZ_OK;
    }
    case 0x5b: {
        int op;
        switch (mand) {
        case MAND_66:
            op = OCERZ_OP_CVTPS2DQ;
            break;
        case MAND_F3:
            op = OCERZ_OP_CVTTPS2DQ;
            break;
        case MAND_NONE:
            op = OCERZ_OP_CVTDQ2PS;
            break;
        default:
            return OCERZ_EUNDEF;
        }
        return decode_sse_rr(s, op, 16, 1);
    }
    }

    if (op2 >= 0x60 && op2 <= 0x6d) {
        int op;
        switch (op2) {
        case 0x60: op = OCERZ_OP_PUNPCKLBW; break;
        case 0x61: op = OCERZ_OP_PUNPCKLWD; break;
        case 0x62: op = OCERZ_OP_PUNPCKLDQ; break;
        case 0x63: op = OCERZ_OP_PACKSSWB; break;
        case 0x64: op = OCERZ_OP_PCMPGTB; break;
        case 0x65: op = OCERZ_OP_PCMPGTW; break;
        case 0x66: op = OCERZ_OP_PCMPGTD; break;
        case 0x67: op = OCERZ_OP_PACKUSWB; break;
        case 0x68: op = OCERZ_OP_PUNPCKHBW; break;
        case 0x69: op = OCERZ_OP_PUNPCKHWD; break;
        case 0x6a: op = OCERZ_OP_PUNPCKHDQ; break;
        case 0x6b: op = OCERZ_OP_PACKSSDW; break;
        default: op = OCERZ_OP_PUNPCKLQDQ; break;
        }
        return decode_pint(s, op, 1);
    }

    switch (op2) {
    case 0x6e: {
        if (mand != MAND_66)
            return OCERZ_EUNDEF;
        ModRM m;
        int gsize = s->rex_w ? 8 : 4;
        e = decode_modrm(s, &m, gsize);
        if (e)
            return e;
        set_op(s, s->rex_w ? OCERZ_OP_MOVQX : OCERZ_OP_MOVD);
        s->out->opsize = (uint8_t)gsize;
        s->out->nops = 2;
        set_xmm(&s->out->ops[0], m.reg, 16);
        place_rm(s, &m, &s->out->ops[1], gsize, 1);
        return OCERZ_OK;
    }
    case 0x6f:
        if (mand == MAND_66)
            return decode_sse_rr(s, OCERZ_OP_MOVDQA, 16, 1);
        if (mand == MAND_F3)
            return decode_sse_rr(s, OCERZ_OP_MOVDQU, 16, 1);
        return OCERZ_EUNDEF;
    case 0x70: {
        int op;
        switch (mand) {
        case MAND_66: op = OCERZ_OP_PSHUFD; break;
        case MAND_F2: op = OCERZ_OP_PSHUFLW; break;
        case MAND_F3: op = OCERZ_OP_PSHUFHW; break;
        default: return OCERZ_EUNDEF;
        }
        return decode_sse_rri(s, op, 16, 1);
    }
    case 0x71:
    case 0x72:
    case 0x73: {
        if (mand != MAND_66)
            return OCERZ_EUNDEF;
        ModRM m;
        e = decode_modrm(s, &m, 16);
        if (e)
            return e;
        if (!rm_is_reg(&m))
            return OCERZ_EUNDEF;
        int idx = m.reg & 7;
        int op = OCERZ_OP_INVALID;
        if (op2 == 0x71) {
            if (idx == 2) op = OCERZ_OP_PSRLW;
            else if (idx == 4) op = OCERZ_OP_PSRAW;
            else if (idx == 6) op = OCERZ_OP_PSLLW;
        } else if (op2 == 0x72) {
            if (idx == 2) op = OCERZ_OP_PSRLD;
            else if (idx == 4) op = OCERZ_OP_PSRAD;
            else if (idx == 6) op = OCERZ_OP_PSLLD;
        } else {
            if (idx == 2) op = OCERZ_OP_PSRLQ;
            else if (idx == 3) op = OCERZ_OP_PSRLDQ;
            else if (idx == 6) op = OCERZ_OP_PSLLQ;
            else if (idx == 7) op = OCERZ_OP_PSLLDQ;
        }
        if (op == OCERZ_OP_INVALID)
            return OCERZ_EUNDEF;
        set_op(s, op);
        s->out->opsize = 16;
        s->out->nops = 2;
        set_xmm(&s->out->ops[0], m.rm, 16);
        return read_imm8s(s, &s->out->ops[1], 1);
    }
    case 0x74:
        return decode_pint(s, OCERZ_OP_PCMPEQB, 1);
    case 0x75:
        return decode_pint(s, OCERZ_OP_PCMPEQW, 1);
    case 0x76:
        return decode_pint(s, OCERZ_OP_PCMPEQD, 1);
    case 0x77:
        set_op(s, OCERZ_OP_EMMS);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0x7e: {
        if (mand == MAND_F3) {
            ModRM m;
            e = decode_modrm(s, &m, 8);
            if (e)
                return e;
            set_op(s, OCERZ_OP_MOVQX);
            s->out->opsize = 8;
            s->out->nops = 2;
            set_xmm(&s->out->ops[0], m.reg, 8);
            place_rm(s, &m, &s->out->ops[1], 8, 0);
            return OCERZ_OK;
        }
        if (mand == MAND_66) {
            ModRM m;
            int gsize = s->rex_w ? 8 : 4;
            e = decode_modrm(s, &m, gsize);
            if (e)
                return e;
            set_op(s, s->rex_w ? OCERZ_OP_MOVQX : OCERZ_OP_MOVD);
            s->out->opsize = (uint8_t)gsize;
            s->out->nops = 2;
            place_rm(s, &m, &s->out->ops[0], gsize, 1);
            set_xmm(&s->out->ops[1], m.reg, 16);
            return OCERZ_OK;
        }
        return OCERZ_EUNDEF;
    }
    case 0x7f:
        if (mand == MAND_66)
            return decode_sse_rr(s, OCERZ_OP_MOVDQA, 16, 0);
        if (mand == MAND_F3)
            return decode_sse_rr(s, OCERZ_OP_MOVDQU, 16, 0);
        return OCERZ_EUNDEF;
    case 0xa3: {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_BT);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        place_rm(s, &m, &s->out->ops[0], size, 1);
        set_reg(&s->out->ops[1], m.reg, size);
        return OCERZ_OK;
    }
    case 0xab:
    case 0xb3:
    case 0xbb: {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, op2 == 0xab ? OCERZ_OP_BTS : (op2 == 0xb3 ? OCERZ_OP_BTR : OCERZ_OP_BTC));
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        place_rm(s, &m, &s->out->ops[0], size, 1);
        set_reg(&s->out->ops[1], m.reg, size);
        return OCERZ_OK;
    }
    case 0xa4:
    case 0xa5:
    case 0xac:
    case 0xad: {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, (op2 == 0xa4 || op2 == 0xa5) ? OCERZ_OP_SHLD : OCERZ_OP_SHRD);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 3;
        place_rm(s, &m, &s->out->ops[0], size, 1);
        set_reg(&s->out->ops[1], m.reg, size);
        if (op2 == 0xa4 || op2 == 0xac)
            return read_imm8s(s, &s->out->ops[2], 1);
        set_reg(&s->out->ops[2], OCERZ_RCX, 1);
        return OCERZ_OK;
    }
    case 0xae: {
        ModRM m;
        e = decode_modrm(s, &m, 0);
        if (e)
            return e;
        int idx = m.reg & 7;
        if (rm_is_reg(&m)) {
            uint8_t lo = (uint8_t)((m.rm & 7));
            if (idx == 5 && lo == 0) {
                set_op(s, OCERZ_OP_LFENCE);
                s->out->nops = 0;
                return OCERZ_OK;
            }
            if (idx == 6 && lo == 0) {
                set_op(s, OCERZ_OP_MFENCE);
                s->out->nops = 0;
                return OCERZ_OK;
            }
            if (idx == 7 && lo == 0) {
                set_op(s, OCERZ_OP_SFENCE);
                s->out->nops = 0;
                return OCERZ_OK;
            }
            return OCERZ_EUNDEF;
        }
        switch (idx) {
        case 0:
            set_op(s, OCERZ_OP_FXSAVE);
            break;
        case 1:
            set_op(s, OCERZ_OP_FXRSTOR);
            break;
        case 2:
            if (mand == MAND_F3)
                return OCERZ_EUNDEF;
            set_op(s, OCERZ_OP_LDMXCSR);
            break;
        case 3:
            if (mand == MAND_F3)
                return OCERZ_EUNDEF;
            set_op(s, OCERZ_OP_STMXCSR);
            break;
        case 7:
            set_op(s, OCERZ_OP_CLFLUSH);
            break;
        default:
            return OCERZ_EUNDEF;
        }
        s->out->nops = 1;
        s->out->ops[0] = m.mem;
        s->out->ops[0].size = 0;
        return OCERZ_OK;
    }
    case 0xaf: {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_IMUL);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, size);
        place_rm(s, &m, &s->out->ops[1], size, 1);
        return OCERZ_OK;
    }
    case 0xb0:
    case 0xb1: {
        ModRM m;
        int byte_form = (op2 == 0xb0);
        int size = byte_form ? 1 : opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_CMPXCHG);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        if (byte_form) {
            place_rm8(s, &m, &s->out->ops[0]);
            set_reg8(s, &s->out->ops[1], m.reg);
        } else {
            place_rm(s, &m, &s->out->ops[0], size, 1);
            set_reg(&s->out->ops[1], m.reg, size);
        }
        return OCERZ_OK;
    }
    case 0xb6:
    case 0xb7: {
        ModRM m;
        int ssize = (op2 == 0xb6) ? 1 : 2;
        int dsize = opsize_default(s);
        e = decode_modrm(s, &m, ssize);
        if (e)
            return e;
        set_op(s, OCERZ_OP_MOVZX);
        s->out->opsize = (uint8_t)dsize;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, dsize);
        if (ssize == 1)
            place_rm8(s, &m, &s->out->ops[1]);
        else
            place_rm(s, &m, &s->out->ops[1], ssize, 1);
        return OCERZ_OK;
    }
    case 0xbe:
    case 0xbf: {
        ModRM m;
        int ssize = (op2 == 0xbe) ? 1 : 2;
        int dsize = opsize_default(s);
        e = decode_modrm(s, &m, ssize);
        if (e)
            return e;
        set_op(s, OCERZ_OP_MOVSX);
        s->out->opsize = (uint8_t)dsize;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, dsize);
        if (ssize == 1)
            place_rm8(s, &m, &s->out->ops[1]);
        else
            place_rm(s, &m, &s->out->ops[1], ssize, 1);
        return OCERZ_OK;
    }
    case 0xb8: {
        if (mand != MAND_F3)
            return OCERZ_EUNDEF;
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_POPCNT);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, size);
        place_rm(s, &m, &s->out->ops[1], size, 1);
        return OCERZ_OK;
    }
    case 0xba: {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        int idx = m.reg & 7;
        int op;
        switch (idx) {
        case 4: op = OCERZ_OP_BT; break;
        case 5: op = OCERZ_OP_BTS; break;
        case 6: op = OCERZ_OP_BTR; break;
        case 7: op = OCERZ_OP_BTC; break;
        default: return OCERZ_EUNDEF;
        }
        set_op(s, op);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        place_rm(s, &m, &s->out->ops[0], size, 1);
        return read_imm8s(s, &s->out->ops[1], 1);
    }
    case 0xbc:
    case 0xbd: {
        ModRM m;
        int size = opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        int op;
        if (op2 == 0xbc)
            op = (mand == MAND_F3) ? OCERZ_OP_TZCNT : OCERZ_OP_BSF;
        else
            op = (mand == MAND_F3) ? OCERZ_OP_LZCNT : OCERZ_OP_BSR;
        set_op(s, op);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, size);
        place_rm(s, &m, &s->out->ops[1], size, 1);
        return OCERZ_OK;
    }
    case 0xc0:
    case 0xc1: {
        ModRM m;
        int byte_form = (op2 == 0xc0);
        int size = byte_form ? 1 : opsize_default(s);
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        set_op(s, OCERZ_OP_XADD);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        if (byte_form) {
            place_rm8(s, &m, &s->out->ops[0]);
            set_reg8(s, &s->out->ops[1], m.reg);
        } else {
            place_rm(s, &m, &s->out->ops[0], size, 1);
            set_reg(&s->out->ops[1], m.reg, size);
        }
        return OCERZ_OK;
    }
    case 0xc2: {
        int op = select_packed(s, OCERZ_OP_CMPPS, OCERZ_OP_CMPPD, OCERZ_OP_CMPSS, OCERZ_OP_CMPSDX);
        int size = sse_scalar_size(s, 16);
        return decode_sse_rri(s, op, size, 1);
    }
    case 0xc3: {
        ModRM m;
        int size = opsize_default(s);
        if (size < 4)
            size = 4;
        e = decode_modrm(s, &m, size);
        if (e)
            return e;
        if (rm_is_reg(&m))
            return OCERZ_EUNDEF;
        set_op(s, OCERZ_OP_MOVNTI);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 2;
        s->out->ops[0] = m.mem;
        s->out->ops[0].size = (uint8_t)size;
        set_reg(&s->out->ops[1], m.reg, size);
        return OCERZ_OK;
    }
    case 0xc4: {
        if (mand != MAND_66)
            return OCERZ_EUNDEF;
        ModRM m;
        e = decode_modrm(s, &m, 4);
        if (e)
            return e;
        set_op(s, OCERZ_OP_PINSRW);
        s->out->opsize = 16;
        s->out->nops = 3;
        set_xmm(&s->out->ops[0], m.reg, 16);
        if (rm_is_reg(&m))
            set_reg(&s->out->ops[1], m.rm, 4);
        else {
            s->out->ops[1] = m.mem;
            s->out->ops[1].size = 2;
        }
        return read_imm8s(s, &s->out->ops[2], 1);
    }
    case 0xc5: {
        if (mand != MAND_66)
            return OCERZ_EUNDEF;
        ModRM m;
        e = decode_modrm(s, &m, 4);
        if (e)
            return e;
        if (!rm_is_reg(&m))
            return OCERZ_EUNDEF;
        set_op(s, OCERZ_OP_PEXTRW);
        s->out->opsize = 4;
        s->out->nops = 3;
        set_reg(&s->out->ops[0], m.reg, s->rex_w ? 8 : 4);
        set_xmm(&s->out->ops[1], m.rm, 16);
        return read_imm8s(s, &s->out->ops[2], 1);
    }
    case 0xc6:
        return decode_sse_rri(s, (mand == MAND_66) ? OCERZ_OP_SHUFPD : OCERZ_OP_SHUFPS, 16, 1);
    case 0xc7: {
        ModRM m;
        e = decode_modrm(s, &m, 8);
        if (e)
            return e;
        int idx = m.reg & 7;
        if (idx != 1)
            return OCERZ_EUNDEF;
        if (rm_is_reg(&m))
            return OCERZ_EUNDEF;
        set_op(s, OCERZ_OP_CMPXCHGXB);
        s->out->opsize = (uint8_t)(s->rex_w ? 16 : 8);
        s->out->nops = 1;
        s->out->ops[0] = m.mem;
        s->out->ops[0].size = (uint8_t)(s->rex_w ? 16 : 8);
        return OCERZ_OK;
    }
    }

    if (op2 >= 0xc8 && op2 <= 0xcf) {
        int reg = (op2 - 0xc8) | (s->rex_b ? 8 : 0);
        int size = s->rex_w ? 8 : 4;
        set_op(s, OCERZ_OP_BSWAP);
        s->out->opsize = (uint8_t)size;
        s->out->nops = 1;
        set_reg(&s->out->ops[0], reg, size);
        return OCERZ_OK;
    }

    switch (op2) {
    case 0xd1:
        return decode_pint(s, OCERZ_OP_PSRLW, 1);
    case 0xd2:
        return decode_pint(s, OCERZ_OP_PSRLD, 1);
    case 0xd3:
        return decode_pint(s, OCERZ_OP_PSRLQ, 1);
    case 0xd4:
        return decode_pint(s, OCERZ_OP_PADDQ, 1);
    case 0xd5:
        return decode_pint(s, OCERZ_OP_PMULLW, 1);
    case 0xd6: {
        if (mand != MAND_66)
            return OCERZ_EUNDEF;
        return decode_sse_rr(s, OCERZ_OP_MOVQX, 8, 0);
    }
    case 0xd7: {
        if (mand != MAND_66)
            return OCERZ_EUNDEF;
        ModRM m;
        e = decode_modrm(s, &m, 16);
        if (e)
            return e;
        if (!rm_is_reg(&m))
            return OCERZ_EUNDEF;
        set_op(s, OCERZ_OP_PMOVMSKB);
        s->out->opsize = 4;
        s->out->nops = 2;
        set_reg(&s->out->ops[0], m.reg, s->rex_w ? 8 : 4);
        set_xmm(&s->out->ops[1], m.rm, 16);
        return OCERZ_OK;
    }
    case 0xd8:
        return decode_pint(s, OCERZ_OP_PSUBUSB, 1);
    case 0xd9:
        return decode_pint(s, OCERZ_OP_PSUBUSW, 1);
    case 0xda:
        return decode_pint(s, OCERZ_OP_PMINUB, 1);
    case 0xdb:
        return decode_pint(s, OCERZ_OP_PAND, 1);
    case 0xdc:
        return decode_pint(s, OCERZ_OP_PADDUSB, 1);
    case 0xdd:
        return decode_pint(s, OCERZ_OP_PADDUSW, 1);
    case 0xde:
        return decode_pint(s, OCERZ_OP_PMAXUB, 1);
    case 0xdf:
        return decode_pint(s, OCERZ_OP_PANDN, 1);
    case 0xe0:
        return decode_pint(s, OCERZ_OP_PAVGB, 1);
    case 0xe1:
        return decode_pint(s, OCERZ_OP_PSRAW, 1);
    case 0xe2:
        return decode_pint(s, OCERZ_OP_PSRAD, 1);
    case 0xe3:
        return decode_pint(s, OCERZ_OP_PAVGW, 1);
    case 0xe4:
        return decode_pint(s, OCERZ_OP_PMULHUW, 1);
    case 0xe5:
        return decode_pint(s, OCERZ_OP_PMULHW, 1);
    case 0xe6: {
        int op;
        switch (mand) {
        case MAND_66: op = OCERZ_OP_CVTTPD2DQ; break;
        case MAND_F3: op = OCERZ_OP_CVTDQ2PD; break;
        case MAND_F2: op = OCERZ_OP_CVTPD2DQ; break;
        default: return OCERZ_EUNDEF;
        }
        return decode_sse_rr(s, op, 16, 1);
    }
    case 0xe7: {
        if (mand != MAND_66)
            return OCERZ_EUNDEF;
        return decode_sse_rr(s, OCERZ_OP_MOVDQA, 16, 0);
    }
    case 0xe8:
        return decode_pint(s, OCERZ_OP_PSUBSB, 1);
    case 0xe9:
        return decode_pint(s, OCERZ_OP_PSUBSW, 1);
    case 0xea:
        return decode_pint(s, OCERZ_OP_PMINSW, 1);
    case 0xeb:
        return decode_pint(s, OCERZ_OP_POR, 1);
    case 0xec:
        return decode_pint(s, OCERZ_OP_PADDSB, 1);
    case 0xed:
        return decode_pint(s, OCERZ_OP_PADDSW, 1);
    case 0xee:
        return decode_pint(s, OCERZ_OP_PMAXSW, 1);
    case 0xef:
        return decode_pint(s, OCERZ_OP_PXOR, 1);
    case 0xf1:
        return decode_pint(s, OCERZ_OP_PSLLW, 1);
    case 0xf2:
        return decode_pint(s, OCERZ_OP_PSLLD, 1);
    case 0xf3:
        return decode_pint(s, OCERZ_OP_PSLLQ, 1);
    case 0xf4:
        return decode_pint(s, OCERZ_OP_PMULUDQ, 1);
    case 0xf5:
        return decode_pint(s, OCERZ_OP_PMADDWD, 1);
    case 0xf6:
        return decode_pint(s, OCERZ_OP_PSADBW, 1);
    case 0xf8:
        return decode_pint(s, OCERZ_OP_PSUBB, 1);
    case 0xf9:
        return decode_pint(s, OCERZ_OP_PSUBW, 1);
    case 0xfa:
        return decode_pint(s, OCERZ_OP_PSUBD, 1);
    case 0xfb:
        return decode_pint(s, OCERZ_OP_PSUBQ, 1);
    case 0xfc:
        return decode_pint(s, OCERZ_OP_PADDB, 1);
    case 0xfd:
        return decode_pint(s, OCERZ_OP_PADDW, 1);
    case 0xfe:
        return decode_pint(s, OCERZ_OP_PADDD, 1);
    }

    return OCERZ_EUNDEF;
}

static int decode_0f38(DecState *s)
{
    uint8_t op3;
    int e = fetch8(s, &op3);
    if (e)
        return e;
    if (sse_prefix(s) != MAND_66)
        return OCERZ_EUNDEF;

    int op;
    switch (op3) {
    case 0x00: op = OCERZ_OP_PSHUFB; break;
    case 0x01: op = OCERZ_OP_PHADDW; break;
    case 0x02: op = OCERZ_OP_PHADDD; break;
    case 0x03: op = OCERZ_OP_PHADDSW; break;
    case 0x04: op = OCERZ_OP_PMADDUBSW; break;
    case 0x05: op = OCERZ_OP_PHSUBW; break;
    case 0x06: op = OCERZ_OP_PHSUBD; break;
    case 0x07: op = OCERZ_OP_PHSUBSW; break;
    case 0x08: op = OCERZ_OP_PSIGNB; break;
    case 0x09: op = OCERZ_OP_PSIGNW; break;
    case 0x0a: op = OCERZ_OP_PSIGND; break;
    case 0x0b: op = OCERZ_OP_PMULHRSW; break;
    case 0x10: op = OCERZ_OP_PBLENDVB; break;
    case 0x14: op = OCERZ_OP_BLENDVPS; break;
    case 0x15: op = OCERZ_OP_BLENDVPD; break;
    case 0x17: op = OCERZ_OP_PTEST; break;
    case 0x1c: op = OCERZ_OP_PABSB; break;
    case 0x1d: op = OCERZ_OP_PABSW; break;
    case 0x1e: op = OCERZ_OP_PABSD; break;
    case 0x20: op = OCERZ_OP_PMOVSXBW; break;
    case 0x21: op = OCERZ_OP_PMOVSXBD; break;
    case 0x22: op = OCERZ_OP_PMOVSXBQ; break;
    case 0x23: op = OCERZ_OP_PMOVSXWD; break;
    case 0x24: op = OCERZ_OP_PMOVSXWQ; break;
    case 0x25: op = OCERZ_OP_PMOVSXDQ; break;
    case 0x29: op = OCERZ_OP_PCMPEQQ; break;
    case 0x2a: op = OCERZ_OP_MOVDQA; break;
    case 0x30: op = OCERZ_OP_PMOVZXBW; break;
    case 0x31: op = OCERZ_OP_PMOVZXBD; break;
    case 0x32: op = OCERZ_OP_PMOVZXBQ; break;
    case 0x33: op = OCERZ_OP_PMOVZXWD; break;
    case 0x34: op = OCERZ_OP_PMOVZXWQ; break;
    case 0x35: op = OCERZ_OP_PMOVZXDQ; break;
    case 0x37: op = OCERZ_OP_PCMPGTQ; break;
    case 0x38: op = OCERZ_OP_PMINSB; break;
    case 0x39: op = OCERZ_OP_PMINSD; break;
    case 0x3a: op = OCERZ_OP_PMINUW; break;
    case 0x3b: op = OCERZ_OP_PMINUD; break;
    case 0x3c: op = OCERZ_OP_PMAXSB; break;
    case 0x3d: op = OCERZ_OP_PMAXSD; break;
    case 0x3e: op = OCERZ_OP_PMAXUW; break;
    case 0x3f: op = OCERZ_OP_PMAXUD; break;
    case 0x40: op = OCERZ_OP_PMULLD; break;
    case 0x2b: op = OCERZ_OP_PACKUSDW; break;
    case 0xdb: op = OCERZ_OP_AESIMC; break;
    case 0xdc: op = OCERZ_OP_AESENC; break;
    case 0xdd: op = OCERZ_OP_AESENCLAST; break;
    case 0xde: op = OCERZ_OP_AESDEC; break;
    case 0xdf: op = OCERZ_OP_AESDECLAST; break;
    default: return OCERZ_EUNDEF;
    }
    return decode_sse_rr(s, op, 16, 1);
}

static int decode_0f3a(DecState *s)
{
    uint8_t op3;
    int e = fetch8(s, &op3);
    if (e)
        return e;
    if (sse_prefix(s) != MAND_66)
        return OCERZ_EUNDEF;

    switch (op3) {
    case 0x08:
        return decode_pint_imm(s, OCERZ_OP_ROUNDPS);
    case 0x09:
        return decode_pint_imm(s, OCERZ_OP_ROUNDPD);
    case 0x0a:
        return decode_sse_rri(s, OCERZ_OP_ROUNDSS, 4, 1);
    case 0x0b:
        return decode_sse_rri(s, OCERZ_OP_ROUNDSD, 8, 1);
    case 0x0c:
        return decode_pint_imm(s, OCERZ_OP_BLENDPS);
    case 0x0d:
        return decode_pint_imm(s, OCERZ_OP_BLENDPD);
    case 0x0e:
        return decode_pint_imm(s, OCERZ_OP_PBLENDW);
    case 0x0f:
        return decode_pint_imm(s, OCERZ_OP_PALIGNR);
    case 0x14: {
        ModRM m;
        e = decode_modrm(s, &m, 1);
        if (e)
            return e;
        set_op(s, OCERZ_OP_PEXTRB);
        s->out->opsize = 1;
        s->out->nops = 3;
        if (rm_is_reg(&m))
            set_reg(&s->out->ops[0], m.rm, s->rex_w ? 8 : 4);
        else {
            s->out->ops[0] = m.mem;
            s->out->ops[0].size = 1;
        }
        set_xmm(&s->out->ops[1], m.reg, 16);
        return read_imm8s(s, &s->out->ops[2], 1);
    }
    case 0x15: {
        ModRM m;
        e = decode_modrm(s, &m, 2);
        if (e)
            return e;
        set_op(s, OCERZ_OP_PEXTRW);
        s->out->opsize = 2;
        s->out->nops = 3;
        if (rm_is_reg(&m))
            set_reg(&s->out->ops[0], m.rm, s->rex_w ? 8 : 4);
        else {
            s->out->ops[0] = m.mem;
            s->out->ops[0].size = 2;
        }
        set_xmm(&s->out->ops[1], m.reg, 16);
        return read_imm8s(s, &s->out->ops[2], 1);
    }
    case 0x16: {
        ModRM m;
        int gsize = s->rex_w ? 8 : 4;
        e = decode_modrm(s, &m, gsize);
        if (e)
            return e;
        set_op(s, s->rex_w ? OCERZ_OP_PEXTRQ : OCERZ_OP_PEXTRD);
        s->out->opsize = (uint8_t)gsize;
        s->out->nops = 3;
        place_rm(s, &m, &s->out->ops[0], gsize, 1);
        set_xmm(&s->out->ops[1], m.reg, 16);
        return read_imm8s(s, &s->out->ops[2], 1);
    }
    case 0x17: {
        ModRM m;
        e = decode_modrm(s, &m, 4);
        if (e)
            return e;
        set_op(s, OCERZ_OP_EXTRACTPS);
        s->out->opsize = 4;
        s->out->nops = 3;
        place_rm(s, &m, &s->out->ops[0], 4, 1);
        set_xmm(&s->out->ops[1], m.reg, 16);
        return read_imm8s(s, &s->out->ops[2], 1);
    }
    case 0x20: {
        ModRM m;
        e = decode_modrm(s, &m, 1);
        if (e)
            return e;
        set_op(s, OCERZ_OP_PINSRB);
        s->out->opsize = 16;
        s->out->nops = 3;
        set_xmm(&s->out->ops[0], m.reg, 16);
        if (rm_is_reg(&m))
            set_reg(&s->out->ops[1], m.rm, 4);
        else {
            s->out->ops[1] = m.mem;
            s->out->ops[1].size = 1;
        }
        return read_imm8s(s, &s->out->ops[2], 1);
    }
    case 0x21:
        return decode_sse_rri(s, OCERZ_OP_INSERTPS, 4, 1);
    case 0x44:
        return decode_pint_imm(s, OCERZ_OP_PCLMULQDQ);
    case 0xdf:
        return decode_pint_imm(s, OCERZ_OP_AESKEYGENASSIST);
    case 0x22: {
        ModRM m;
        int gsize = s->rex_w ? 8 : 4;
        e = decode_modrm(s, &m, gsize);
        if (e)
            return e;
        set_op(s, s->rex_w ? OCERZ_OP_PINSRQ : OCERZ_OP_PINSRD);
        s->out->opsize = 16;
        s->out->nops = 3;
        set_xmm(&s->out->ops[0], m.reg, 16);
        place_rm(s, &m, &s->out->ops[1], gsize, 1);
        return read_imm8s(s, &s->out->ops[2], 1);
    }
    }

    return OCERZ_EUNDEF;
}

static int x87_mem(DecState *s, ModRM *m, int op, int size)
{
    set_op(s, op);
    s->out->opsize = (uint8_t)size;
    s->out->nops = 1;
    s->out->ops[0] = m->mem;
    s->out->ops[0].size = (uint8_t)size;
    return OCERZ_OK;
}

static int x87_d8(DecState *s, ModRM *m)
{
    int idx = m->reg & 7;
    if (!rm_is_reg(m)) {
        int ops[8] = { OCERZ_OP_FADD, OCERZ_OP_FMUL, OCERZ_OP_FCOM, OCERZ_OP_FCOMP,
                       OCERZ_OP_FSUB, OCERZ_OP_FSUBR, OCERZ_OP_FDIV, OCERZ_OP_FDIVR };
        return x87_mem(s, m, ops[idx], 4);
    }
    int i = m->rm & 7;
    int ops[8] = { OCERZ_OP_FADD, OCERZ_OP_FMUL, OCERZ_OP_FCOM, OCERZ_OP_FCOMP,
                   OCERZ_OP_FSUB, OCERZ_OP_FSUBR, OCERZ_OP_FDIV, OCERZ_OP_FDIVR };
    set_op(s, ops[idx]);
    s->out->opsize = 10;
    s->out->nops = 2;
    set_st(&s->out->ops[0], 0);
    set_st(&s->out->ops[1], i);
    return OCERZ_OK;
}

static int x87_d9(DecState *s, ModRM *m)
{
    int idx = m->reg & 7;
    if (!rm_is_reg(m)) {
        switch (idx) {
        case 0: return x87_mem(s, m, OCERZ_OP_FLD, 4);
        case 2: return x87_mem(s, m, OCERZ_OP_FST, 4);
        case 3: return x87_mem(s, m, OCERZ_OP_FSTP, 4);
        case 4: return x87_mem(s, m, OCERZ_OP_FLDENV, 0);
        case 5: return x87_mem(s, m, OCERZ_OP_FLDCW, 2);
        case 6: return x87_mem(s, m, OCERZ_OP_FNSTENV, 0);
        case 7: return x87_mem(s, m, OCERZ_OP_FNSTCW, 2);
        default: return OCERZ_EUNDEF;
        }
    }
    int i = m->rm & 7;
    uint8_t modrm = (uint8_t)(0xc0 | ((m->reg & 7) << 3) | (m->rm & 7));
    if (idx == 0) {
        set_op(s, OCERZ_OP_FLD);
        s->out->opsize = 10;
        s->out->nops = 1;
        set_st(&s->out->ops[0], i);
        return OCERZ_OK;
    }
    if (idx == 1) {
        set_op(s, OCERZ_OP_FXCH);
        s->out->opsize = 10;
        s->out->nops = 1;
        set_st(&s->out->ops[0], i);
        return OCERZ_OK;
    }
    switch (modrm) {
    case 0xd0:
        set_op(s, OCERZ_OP_NOP);
        s->out->nops = 0;
        return OCERZ_OK;
    case 0xe0: set_op(s, OCERZ_OP_FCHS); s->out->nops = 0; return OCERZ_OK;
    case 0xe1: set_op(s, OCERZ_OP_FABS); s->out->nops = 0; return OCERZ_OK;
    case 0xe4: set_op(s, OCERZ_OP_FTST); s->out->nops = 0; return OCERZ_OK;
    case 0xe5: set_op(s, OCERZ_OP_FXCH); s->out->nops = 0; return OCERZ_OK;
    case 0xe8: set_op(s, OCERZ_OP_FLD1); s->out->nops = 0; return OCERZ_OK;
    case 0xe9: set_op(s, OCERZ_OP_FLDL2T); s->out->nops = 0; return OCERZ_OK;
    case 0xea: set_op(s, OCERZ_OP_FLDL2E); s->out->nops = 0; return OCERZ_OK;
    case 0xeb: set_op(s, OCERZ_OP_FLDPI); s->out->nops = 0; return OCERZ_OK;
    case 0xec: set_op(s, OCERZ_OP_FLDLG2); s->out->nops = 0; return OCERZ_OK;
    case 0xed: set_op(s, OCERZ_OP_FLDLN2); s->out->nops = 0; return OCERZ_OK;
    case 0xee: set_op(s, OCERZ_OP_FLDZ); s->out->nops = 0; return OCERZ_OK;
    case 0xf0: set_op(s, OCERZ_OP_F2XM1); s->out->nops = 0; return OCERZ_OK;
    case 0xf1: set_op(s, OCERZ_OP_FYL2X); s->out->nops = 0; return OCERZ_OK;
    case 0xf2: set_op(s, OCERZ_OP_FPTAN); s->out->nops = 0; return OCERZ_OK;
    case 0xf3: set_op(s, OCERZ_OP_FPATAN); s->out->nops = 0; return OCERZ_OK;
    case 0xf5: set_op(s, OCERZ_OP_FPREM1); s->out->nops = 0; return OCERZ_OK;
    case 0xf6: set_op(s, OCERZ_OP_FDECSTP); s->out->nops = 0; return OCERZ_OK;
    case 0xf7: set_op(s, OCERZ_OP_FINCSTP); s->out->nops = 0; return OCERZ_OK;
    case 0xf8: set_op(s, OCERZ_OP_FPREM); s->out->nops = 0; return OCERZ_OK;
    case 0xf9: set_op(s, OCERZ_OP_FYL2X); s->out->nops = 0; return OCERZ_OK;
    case 0xfa: set_op(s, OCERZ_OP_FSQRT); s->out->nops = 0; return OCERZ_OK;
    case 0xfb: set_op(s, OCERZ_OP_FSINCOS); s->out->nops = 0; return OCERZ_OK;
    case 0xfc: set_op(s, OCERZ_OP_FRNDINT); s->out->nops = 0; return OCERZ_OK;
    case 0xfd: set_op(s, OCERZ_OP_FSCALE); s->out->nops = 0; return OCERZ_OK;
    case 0xfe: set_op(s, OCERZ_OP_FSIN); s->out->nops = 0; return OCERZ_OK;
    case 0xff: set_op(s, OCERZ_OP_FCOS); s->out->nops = 0; return OCERZ_OK;
    default: return OCERZ_EUNDEF;
    }
}

static int x87_da(DecState *s, ModRM *m)
{
    int idx = m->reg & 7;
    if (!rm_is_reg(m)) {
        int ops[8] = { OCERZ_OP_FIADD, OCERZ_OP_FIMUL, OCERZ_OP_FCOM, OCERZ_OP_FCOMP,
                       OCERZ_OP_FISUB, OCERZ_OP_FISUBR, OCERZ_OP_FIDIV, OCERZ_OP_FIDIVR };
        return x87_mem(s, m, ops[idx], 4);
    }
    int i = m->rm & 7;
    uint8_t modrm = (uint8_t)(0xc0 | ((m->reg & 7) << 3) | (m->rm & 7));
    if (modrm == 0xe9) {
        set_op(s, OCERZ_OP_FUCOMPP);
        s->out->nops = 0;
        return OCERZ_OK;
    }
    int cc;
    switch (idx) {
    case 0: cc = OCERZ_CC_B; break;
    case 1: cc = OCERZ_CC_E; break;
    case 2: cc = OCERZ_CC_BE; break;
    case 3: cc = OCERZ_CC_P; break;
    default: return OCERZ_EUNDEF;
    }
    set_op(s, OCERZ_OP_FCMOVCC);
    s->out->opsize = 10;
    s->out->cc = (uint8_t)cc;
    s->out->nops = 2;
    set_st(&s->out->ops[0], 0);
    set_st(&s->out->ops[1], i);
    return OCERZ_OK;
}

static int x87_db(DecState *s, ModRM *m)
{
    int idx = m->reg & 7;
    if (!rm_is_reg(m)) {
        switch (idx) {
        case 0: return x87_mem(s, m, OCERZ_OP_FILD, 4);
        case 2: return x87_mem(s, m, OCERZ_OP_FIST, 4);
        case 3: return x87_mem(s, m, OCERZ_OP_FISTP, 4);
        case 5: return x87_mem(s, m, OCERZ_OP_FLD, 10);
        case 7: return x87_mem(s, m, OCERZ_OP_FSTP, 10);
        default: return OCERZ_EUNDEF;
        }
    }
    int i = m->rm & 7;
    uint8_t modrm = (uint8_t)(0xc0 | ((m->reg & 7) << 3) | (m->rm & 7));
    if (modrm == 0xe2) {
        set_op(s, OCERZ_OP_FNCLEX);
        s->out->nops = 0;
        return OCERZ_OK;
    }
    if (modrm == 0xe3) {
        set_op(s, OCERZ_OP_FNINIT);
        s->out->nops = 0;
        return OCERZ_OK;
    }
    int cc = -1;
    int op = OCERZ_OP_INVALID;
    switch (idx) {
    case 0: op = OCERZ_OP_FCMOVCC; cc = OCERZ_CC_AE; break;
    case 1: op = OCERZ_OP_FCMOVCC; cc = OCERZ_CC_NE; break;
    case 2: op = OCERZ_OP_FCMOVCC; cc = OCERZ_CC_A; break;
    case 3: op = OCERZ_OP_FCMOVCC; cc = OCERZ_CC_NP; break;
    case 5: op = OCERZ_OP_FUCOMI; break;
    case 6: op = OCERZ_OP_FCOMI; break;
    default: return OCERZ_EUNDEF;
    }
    set_op(s, op);
    s->out->opsize = 10;
    s->out->nops = 2;
    if (cc >= 0)
        s->out->cc = (uint8_t)cc;
    set_st(&s->out->ops[0], 0);
    set_st(&s->out->ops[1], i);
    return OCERZ_OK;
}

static int x87_dc(DecState *s, ModRM *m)
{
    int idx = m->reg & 7;
    if (!rm_is_reg(m)) {
        int ops[8] = { OCERZ_OP_FADD, OCERZ_OP_FMUL, OCERZ_OP_FCOM, OCERZ_OP_FCOMP,
                       OCERZ_OP_FSUB, OCERZ_OP_FSUBR, OCERZ_OP_FDIV, OCERZ_OP_FDIVR };
        return x87_mem(s, m, ops[idx], 8);
    }
    int i = m->rm & 7;
    int ops[8] = { OCERZ_OP_FADD, OCERZ_OP_FMUL, OCERZ_OP_FCOM, OCERZ_OP_FCOMP,
                   OCERZ_OP_FSUBR, OCERZ_OP_FSUB, OCERZ_OP_FDIVR, OCERZ_OP_FDIV };
    set_op(s, ops[idx]);
    s->out->opsize = 10;
    s->out->nops = 2;
    set_st(&s->out->ops[0], i);
    set_st(&s->out->ops[1], 0);
    return OCERZ_OK;
}

static int x87_dd(DecState *s, ModRM *m)
{
    int idx = m->reg & 7;
    if (!rm_is_reg(m)) {
        switch (idx) {
        case 0: return x87_mem(s, m, OCERZ_OP_FLD, 8);
        case 2: return x87_mem(s, m, OCERZ_OP_FST, 8);
        case 3: return x87_mem(s, m, OCERZ_OP_FSTP, 8);
        case 4: return x87_mem(s, m, OCERZ_OP_FNSTENV, 0);
        case 6: return x87_mem(s, m, OCERZ_OP_FNSTENV, 0);
        case 7: return x87_mem(s, m, OCERZ_OP_FNSTSW, 2);
        default: return OCERZ_EUNDEF;
        }
    }
    int i = m->rm & 7;
    switch (idx) {
    case 0: set_op(s, OCERZ_OP_FFREE); break;
    case 2: set_op(s, OCERZ_OP_FST); break;
    case 3: set_op(s, OCERZ_OP_FSTP); break;
    case 4: set_op(s, OCERZ_OP_FUCOM); break;
    case 5: set_op(s, OCERZ_OP_FUCOMP); break;
    default: return OCERZ_EUNDEF;
    }
    s->out->opsize = 10;
    s->out->nops = 1;
    set_st(&s->out->ops[0], i);
    return OCERZ_OK;
}

static int x87_de(DecState *s, ModRM *m)
{
    int idx = m->reg & 7;
    if (!rm_is_reg(m)) {
        int ops[8] = { OCERZ_OP_FIADD, OCERZ_OP_FIMUL, OCERZ_OP_FCOM, OCERZ_OP_FCOMP,
                       OCERZ_OP_FISUB, OCERZ_OP_FISUBR, OCERZ_OP_FIDIV, OCERZ_OP_FIDIVR };
        return x87_mem(s, m, ops[idx], 2);
    }
    int i = m->rm & 7;
    uint8_t modrm = (uint8_t)(0xc0 | ((m->reg & 7) << 3) | (m->rm & 7));
    if (modrm == 0xd9) {
        set_op(s, OCERZ_OP_FCOMPP);
        s->out->nops = 0;
        return OCERZ_OK;
    }
    int ops[8] = { OCERZ_OP_FADDP, OCERZ_OP_FMULP, OCERZ_OP_FCOMP, OCERZ_OP_FCOMP,
                   OCERZ_OP_FSUBRP, OCERZ_OP_FSUBP, OCERZ_OP_FDIVRP, OCERZ_OP_FDIVP };
    set_op(s, ops[idx]);
    s->out->opsize = 10;
    s->out->nops = 2;
    set_st(&s->out->ops[0], i);
    set_st(&s->out->ops[1], 0);
    return OCERZ_OK;
}

static int x87_df(DecState *s, ModRM *m)
{
    int idx = m->reg & 7;
    if (!rm_is_reg(m)) {
        switch (idx) {
        case 0: return x87_mem(s, m, OCERZ_OP_FILD, 2);
        case 2: return x87_mem(s, m, OCERZ_OP_FIST, 2);
        case 3: return x87_mem(s, m, OCERZ_OP_FISTP, 2);
        case 5: return x87_mem(s, m, OCERZ_OP_FILD, 8);
        case 7: return x87_mem(s, m, OCERZ_OP_FISTP, 8);
        default: return OCERZ_EUNDEF;
        }
    }
    int i = m->rm & 7;
    uint8_t modrm = (uint8_t)(0xc0 | ((m->reg & 7) << 3) | (m->rm & 7));
    if (modrm == 0xe0) {
        set_op(s, OCERZ_OP_FNSTSW);
        s->out->opsize = 2;
        s->out->nops = 1;
        set_reg(&s->out->ops[0], OCERZ_RAX, 2);
        return OCERZ_OK;
    }
    if (idx == 5) {
        set_op(s, OCERZ_OP_FUCOMIP);
        s->out->opsize = 10;
        s->out->nops = 2;
        set_st(&s->out->ops[0], 0);
        set_st(&s->out->ops[1], i);
        return OCERZ_OK;
    }
    if (idx == 6) {
        set_op(s, OCERZ_OP_FCOMIP);
        s->out->opsize = 10;
        s->out->nops = 2;
        set_st(&s->out->ops[0], 0);
        set_st(&s->out->ops[1], i);
        return OCERZ_OK;
    }
    return OCERZ_EUNDEF;
}

static int decode_x87(DecState *s, uint8_t op)
{
    ModRM m;
    int e = decode_modrm(s, &m, 4);
    if (e)
        return e;
    switch (op) {
    case 0xd8: return x87_d8(s, &m);
    case 0xd9: return x87_d9(s, &m);
    case 0xda: return x87_da(s, &m);
    case 0xdb: return x87_db(s, &m);
    case 0xdc: return x87_dc(s, &m);
    case 0xdd: return x87_dd(s, &m);
    case 0xde: return x87_de(s, &m);
    default: return x87_df(s, &m);
    }
}

static const char *op_names[OCERZ_OP_COUNT];
static int op_names_init;

static void init_op_names(void)
{
    if (op_names_init)
        return;
    op_names_init = 1;
    for (int i = 0; i < OCERZ_OP_COUNT; i++)
        op_names[i] = "?";
    op_names[OCERZ_OP_INVALID] = "(invalid)";
    op_names[OCERZ_OP_MOV] = "mov";
    op_names[OCERZ_OP_MOVZX] = "movzx";
    op_names[OCERZ_OP_MOVSX] = "movsx";
    op_names[OCERZ_OP_MOVSXD] = "movsxd";
    op_names[OCERZ_OP_LEA] = "lea";
    op_names[OCERZ_OP_XCHG] = "xchg";
    op_names[OCERZ_OP_BSWAP] = "bswap";
    op_names[OCERZ_OP_PUSH] = "push";
    op_names[OCERZ_OP_POP] = "pop";
    op_names[OCERZ_OP_PUSHF] = "pushf";
    op_names[OCERZ_OP_POPF] = "popf";
    op_names[OCERZ_OP_LAHF] = "lahf";
    op_names[OCERZ_OP_SAHF] = "sahf";
    op_names[OCERZ_OP_CBW] = "cbw";
    op_names[OCERZ_OP_CWD] = "cwd";
    op_names[OCERZ_OP_CMOVCC] = "cmovcc";
    op_names[OCERZ_OP_SETCC] = "setcc";
    op_names[OCERZ_OP_ADD] = "add";
    op_names[OCERZ_OP_ADC] = "adc";
    op_names[OCERZ_OP_SUB] = "sub";
    op_names[OCERZ_OP_SBB] = "sbb";
    op_names[OCERZ_OP_AND] = "and";
    op_names[OCERZ_OP_OR] = "or";
    op_names[OCERZ_OP_XOR] = "xor";
    op_names[OCERZ_OP_CMP] = "cmp";
    op_names[OCERZ_OP_TEST] = "test";
    op_names[OCERZ_OP_INC] = "inc";
    op_names[OCERZ_OP_DEC] = "dec";
    op_names[OCERZ_OP_NEG] = "neg";
    op_names[OCERZ_OP_NOT] = "not";
    op_names[OCERZ_OP_MUL] = "mul";
    op_names[OCERZ_OP_IMUL] = "imul";
    op_names[OCERZ_OP_DIV] = "div";
    op_names[OCERZ_OP_IDIV] = "idiv";
    op_names[OCERZ_OP_SHL] = "shl";
    op_names[OCERZ_OP_SHR] = "shr";
    op_names[OCERZ_OP_SAR] = "sar";
    op_names[OCERZ_OP_ROL] = "rol";
    op_names[OCERZ_OP_ROR] = "ror";
    op_names[OCERZ_OP_RCL] = "rcl";
    op_names[OCERZ_OP_RCR] = "rcr";
    op_names[OCERZ_OP_SHLD] = "shld";
    op_names[OCERZ_OP_SHRD] = "shrd";
    op_names[OCERZ_OP_BT] = "bt";
    op_names[OCERZ_OP_BTS] = "bts";
    op_names[OCERZ_OP_BTR] = "btr";
    op_names[OCERZ_OP_BTC] = "btc";
    op_names[OCERZ_OP_BSF] = "bsf";
    op_names[OCERZ_OP_BSR] = "bsr";
    op_names[OCERZ_OP_POPCNT] = "popcnt";
    op_names[OCERZ_OP_TZCNT] = "tzcnt";
    op_names[OCERZ_OP_LZCNT] = "lzcnt";
    op_names[OCERZ_OP_JMP] = "jmp";
    op_names[OCERZ_OP_JCC] = "jcc";
    op_names[OCERZ_OP_JRCXZ] = "jrcxz";
    op_names[OCERZ_OP_LOOP] = "loop";
    op_names[OCERZ_OP_LOOPE] = "loope";
    op_names[OCERZ_OP_LOOPNE] = "loopne";
    op_names[OCERZ_OP_CALL] = "call";
    op_names[OCERZ_OP_RET] = "ret";
    op_names[OCERZ_OP_IRET] = "iret";
    op_names[OCERZ_OP_JMPF] = "jmpf";
    op_names[OCERZ_OP_CALLF] = "callf";
    op_names[OCERZ_OP_RETF] = "retf";
    op_names[OCERZ_OP_LEAVE] = "leave";
    op_names[OCERZ_OP_INT3] = "int3";
    op_names[OCERZ_OP_INT] = "int";
    op_names[OCERZ_OP_UD2] = "ud2";
    op_names[OCERZ_OP_HLT] = "hlt";
    op_names[OCERZ_OP_MOVS] = "movs";
    op_names[OCERZ_OP_STOS] = "stos";
    op_names[OCERZ_OP_LODS] = "lods";
    op_names[OCERZ_OP_SCAS] = "scas";
    op_names[OCERZ_OP_CMPS] = "cmps";
    op_names[OCERZ_OP_XADD] = "xadd";
    op_names[OCERZ_OP_CMPXCHG] = "cmpxchg";
    op_names[OCERZ_OP_CMPXCHGXB] = "cmpxchgxb";
    op_names[OCERZ_OP_CLC] = "clc";
    op_names[OCERZ_OP_STC] = "stc";
    op_names[OCERZ_OP_CMC] = "cmc";
    op_names[OCERZ_OP_CLD] = "cld";
    op_names[OCERZ_OP_STD] = "std";
    op_names[OCERZ_OP_SYSCALL] = "syscall";
    op_names[OCERZ_OP_CPUID] = "cpuid";
    op_names[OCERZ_OP_RDTSC] = "rdtsc";
    op_names[OCERZ_OP_RDTSCP] = "rdtscp";
    op_names[OCERZ_OP_PAUSE] = "pause";
    op_names[OCERZ_OP_NOP] = "nop";
    op_names[OCERZ_OP_MFENCE] = "mfence";
    op_names[OCERZ_OP_LFENCE] = "lfence";
    op_names[OCERZ_OP_SFENCE] = "sfence";
    op_names[OCERZ_OP_XGETBV] = "xgetbv";
    op_names[OCERZ_OP_SGDT] = "sgdt";
    op_names[OCERZ_OP_SIDT] = "sidt";
    op_names[OCERZ_OP_FXSAVE] = "fxsave";
    op_names[OCERZ_OP_FXRSTOR] = "fxrstor";
    op_names[OCERZ_OP_LDMXCSR] = "ldmxcsr";
    op_names[OCERZ_OP_STMXCSR] = "stmxcsr";
    op_names[OCERZ_OP_PREFETCH] = "prefetch";
    op_names[OCERZ_OP_CLFLUSH] = "clflush";
    op_names[OCERZ_OP_EMMS] = "emms";
    op_names[OCERZ_OP_FLD] = "fld";
    op_names[OCERZ_OP_FST] = "fst";
    op_names[OCERZ_OP_FSTP] = "fstp";
    op_names[OCERZ_OP_FILD] = "fild";
    op_names[OCERZ_OP_FIST] = "fist";
    op_names[OCERZ_OP_FISTP] = "fistp";
    op_names[OCERZ_OP_FLDCW] = "fldcw";
    op_names[OCERZ_OP_FNSTCW] = "fnstcw";
    op_names[OCERZ_OP_FNSTSW] = "fnstsw";
    op_names[OCERZ_OP_FNSTENV] = "fnstenv";
    op_names[OCERZ_OP_FLDENV] = "fldenv";
    op_names[OCERZ_OP_FXCH] = "fxch";
    op_names[OCERZ_OP_FCHS] = "fchs";
    op_names[OCERZ_OP_FABS] = "fabs";
    op_names[OCERZ_OP_FADD] = "fadd";
    op_names[OCERZ_OP_FADDP] = "faddp";
    op_names[OCERZ_OP_FIADD] = "fiadd";
    op_names[OCERZ_OP_FSUB] = "fsub";
    op_names[OCERZ_OP_FSUBR] = "fsubr";
    op_names[OCERZ_OP_FSUBP] = "fsubp";
    op_names[OCERZ_OP_FSUBRP] = "fsubrp";
    op_names[OCERZ_OP_FISUB] = "fisub";
    op_names[OCERZ_OP_FISUBR] = "fisubr";
    op_names[OCERZ_OP_FMUL] = "fmul";
    op_names[OCERZ_OP_FMULP] = "fmulp";
    op_names[OCERZ_OP_FIMUL] = "fimul";
    op_names[OCERZ_OP_FDIV] = "fdiv";
    op_names[OCERZ_OP_FDIVR] = "fdivr";
    op_names[OCERZ_OP_FDIVP] = "fdivp";
    op_names[OCERZ_OP_FDIVRP] = "fdivrp";
    op_names[OCERZ_OP_FIDIV] = "fidiv";
    op_names[OCERZ_OP_FIDIVR] = "fidivr";
    op_names[OCERZ_OP_FCOM] = "fcom";
    op_names[OCERZ_OP_FCOMP] = "fcomp";
    op_names[OCERZ_OP_FCOMPP] = "fcompp";
    op_names[OCERZ_OP_FCOMI] = "fcomi";
    op_names[OCERZ_OP_FCOMIP] = "fcomip";
    op_names[OCERZ_OP_FUCOM] = "fucom";
    op_names[OCERZ_OP_FUCOMP] = "fucomp";
    op_names[OCERZ_OP_FUCOMPP] = "fucompp";
    op_names[OCERZ_OP_FUCOMI] = "fucomi";
    op_names[OCERZ_OP_FUCOMIP] = "fucomip";
    op_names[OCERZ_OP_FCMOVCC] = "fcmovcc";
    op_names[OCERZ_OP_FTST] = "ftst";
    op_names[OCERZ_OP_FLDZ] = "fldz";
    op_names[OCERZ_OP_FLD1] = "fld1";
    op_names[OCERZ_OP_FLDPI] = "fldpi";
    op_names[OCERZ_OP_FLDL2E] = "fldl2e";
    op_names[OCERZ_OP_FLDL2T] = "fldl2t";
    op_names[OCERZ_OP_FLDLG2] = "fldlg2";
    op_names[OCERZ_OP_FLDLN2] = "fldln2";
    op_names[OCERZ_OP_FSQRT] = "fsqrt";
    op_names[OCERZ_OP_FRNDINT] = "frndint";
    op_names[OCERZ_OP_F2XM1] = "f2xm1";
    op_names[OCERZ_OP_FYL2X] = "fyl2x";
    op_names[OCERZ_OP_FPTAN] = "fptan";
    op_names[OCERZ_OP_FPATAN] = "fpatan";
    op_names[OCERZ_OP_FPREM] = "fprem";
    op_names[OCERZ_OP_FPREM1] = "fprem1";
    op_names[OCERZ_OP_FSCALE] = "fscale";
    op_names[OCERZ_OP_FSIN] = "fsin";
    op_names[OCERZ_OP_FCOS] = "fcos";
    op_names[OCERZ_OP_FSINCOS] = "fsincos";
    op_names[OCERZ_OP_FNINIT] = "fninit";
    op_names[OCERZ_OP_FNCLEX] = "fnclex";
    op_names[OCERZ_OP_FFREE] = "ffree";
    op_names[OCERZ_OP_FINCSTP] = "fincstp";
    op_names[OCERZ_OP_FDECSTP] = "fdecstp";
    op_names[OCERZ_OP_FWAIT] = "fwait";
    op_names[OCERZ_OP_MOVUPS] = "movups";
    op_names[OCERZ_OP_MOVAPS] = "movaps";
    op_names[OCERZ_OP_MOVDQA] = "movdqa";
    op_names[OCERZ_OP_MOVDQU] = "movdqu";
    op_names[OCERZ_OP_MOVSS] = "movss";
    op_names[OCERZ_OP_MOVSDX] = "movsd";
    op_names[OCERZ_OP_MOVD] = "movd";
    op_names[OCERZ_OP_MOVQX] = "movq";
    op_names[OCERZ_OP_MOVLPS] = "movlps";
    op_names[OCERZ_OP_MOVHPS] = "movhps";
    op_names[OCERZ_OP_MOVLHPS] = "movlhps";
    op_names[OCERZ_OP_MOVHLPS] = "movhlps";
    op_names[OCERZ_OP_MOVMSKPS] = "movmskps";
    op_names[OCERZ_OP_MOVMSKPD] = "movmskpd";
    op_names[OCERZ_OP_PMOVMSKB] = "pmovmskb";
    op_names[OCERZ_OP_MOVSHDUP] = "movshdup";
    op_names[OCERZ_OP_MOVSLDUP] = "movsldup";
    op_names[OCERZ_OP_MOVDDUP] = "movddup";
    op_names[OCERZ_OP_MOVNTI] = "movnti";
    op_names[OCERZ_OP_ADDPS] = "addps";
    op_names[OCERZ_OP_ADDPD] = "addpd";
    op_names[OCERZ_OP_ADDSS] = "addss";
    op_names[OCERZ_OP_ADDSD] = "addsd";
    op_names[OCERZ_OP_HADDPS] = "haddps";
    op_names[OCERZ_OP_HADDPD] = "haddpd";
    op_names[OCERZ_OP_HSUBPS] = "hsubps";
    op_names[OCERZ_OP_HSUBPD] = "hsubpd";
    op_names[OCERZ_OP_SUBPS] = "subps";
    op_names[OCERZ_OP_SUBPD] = "subpd";
    op_names[OCERZ_OP_SUBSS] = "subss";
    op_names[OCERZ_OP_SUBSD] = "subsd";
    op_names[OCERZ_OP_MULPS] = "mulps";
    op_names[OCERZ_OP_MULPD] = "mulpd";
    op_names[OCERZ_OP_MULSS] = "mulss";
    op_names[OCERZ_OP_MULSD] = "mulsd";
    op_names[OCERZ_OP_DIVPS] = "divps";
    op_names[OCERZ_OP_DIVPD] = "divpd";
    op_names[OCERZ_OP_DIVSS] = "divss";
    op_names[OCERZ_OP_DIVSD] = "divsd";
    op_names[OCERZ_OP_MINPS] = "minps";
    op_names[OCERZ_OP_MINPD] = "minpd";
    op_names[OCERZ_OP_MINSS] = "minss";
    op_names[OCERZ_OP_MINSD] = "minsd";
    op_names[OCERZ_OP_MAXPS] = "maxps";
    op_names[OCERZ_OP_MAXPD] = "maxpd";
    op_names[OCERZ_OP_MAXSS] = "maxss";
    op_names[OCERZ_OP_MAXSD] = "maxsd";
    op_names[OCERZ_OP_SQRTPS] = "sqrtps";
    op_names[OCERZ_OP_SQRTPD] = "sqrtpd";
    op_names[OCERZ_OP_SQRTSS] = "sqrtss";
    op_names[OCERZ_OP_SQRTSD] = "sqrtsd";
    op_names[OCERZ_OP_RSQRTPS] = "rsqrtps";
    op_names[OCERZ_OP_RSQRTSS] = "rsqrtss";
    op_names[OCERZ_OP_RCPPS] = "rcpps";
    op_names[OCERZ_OP_RCPSS] = "rcpss";
    op_names[OCERZ_OP_ANDPS] = "andps";
    op_names[OCERZ_OP_ANDNPS] = "andnps";
    op_names[OCERZ_OP_ORPS] = "orps";
    op_names[OCERZ_OP_XORPS] = "xorps";
    op_names[OCERZ_OP_PAND] = "pand";
    op_names[OCERZ_OP_PANDN] = "pandn";
    op_names[OCERZ_OP_POR] = "por";
    op_names[OCERZ_OP_PXOR] = "pxor";
    op_names[OCERZ_OP_CMPPS] = "cmpps";
    op_names[OCERZ_OP_CMPPD] = "cmppd";
    op_names[OCERZ_OP_CMPSS] = "cmpss";
    op_names[OCERZ_OP_CMPSDX] = "cmpsd";
    op_names[OCERZ_OP_COMISS] = "comiss";
    op_names[OCERZ_OP_COMISD] = "comisd";
    op_names[OCERZ_OP_UCOMISS] = "ucomiss";
    op_names[OCERZ_OP_UCOMISD] = "ucomisd";
    op_names[OCERZ_OP_PCMPEQB] = "pcmpeqb";
    op_names[OCERZ_OP_PCMPEQW] = "pcmpeqw";
    op_names[OCERZ_OP_PCMPEQD] = "pcmpeqd";
    op_names[OCERZ_OP_PCMPEQQ] = "pcmpeqq";
    op_names[OCERZ_OP_PCMPGTB] = "pcmpgtb";
    op_names[OCERZ_OP_PCMPGTW] = "pcmpgtw";
    op_names[OCERZ_OP_PCMPGTD] = "pcmpgtd";
    op_names[OCERZ_OP_PCMPGTQ] = "pcmpgtq";
    op_names[OCERZ_OP_CVTSI2SS] = "cvtsi2ss";
    op_names[OCERZ_OP_CVTSI2SD] = "cvtsi2sd";
    op_names[OCERZ_OP_CVTSS2SI] = "cvtss2si";
    op_names[OCERZ_OP_CVTSD2SI] = "cvtsd2si";
    op_names[OCERZ_OP_CVTTSS2SI] = "cvttss2si";
    op_names[OCERZ_OP_CVTTSD2SI] = "cvttsd2si";
    op_names[OCERZ_OP_CVTSS2SD] = "cvtss2sd";
    op_names[OCERZ_OP_CVTSD2SS] = "cvtsd2ss";
    op_names[OCERZ_OP_CVTPS2PD] = "cvtps2pd";
    op_names[OCERZ_OP_CVTPD2PS] = "cvtpd2ps";
    op_names[OCERZ_OP_CVTDQ2PS] = "cvtdq2ps";
    op_names[OCERZ_OP_CVTPS2DQ] = "cvtps2dq";
    op_names[OCERZ_OP_CVTTPS2DQ] = "cvttps2dq";
    op_names[OCERZ_OP_CVTDQ2PD] = "cvtdq2pd";
    op_names[OCERZ_OP_CVTPD2DQ] = "cvtpd2dq";
    op_names[OCERZ_OP_CVTTPD2DQ] = "cvttpd2dq";
    op_names[OCERZ_OP_PADDB] = "paddb";
    op_names[OCERZ_OP_PADDW] = "paddw";
    op_names[OCERZ_OP_PADDD] = "paddd";
    op_names[OCERZ_OP_PADDQ] = "paddq";
    op_names[OCERZ_OP_PSUBB] = "psubb";
    op_names[OCERZ_OP_PSUBW] = "psubw";
    op_names[OCERZ_OP_PSUBD] = "psubd";
    op_names[OCERZ_OP_PSUBQ] = "psubq";
    op_names[OCERZ_OP_PADDSB] = "paddsb";
    op_names[OCERZ_OP_PADDSW] = "paddsw";
    op_names[OCERZ_OP_PADDUSB] = "paddusb";
    op_names[OCERZ_OP_PADDUSW] = "paddusw";
    op_names[OCERZ_OP_PSUBSB] = "psubsb";
    op_names[OCERZ_OP_PSUBSW] = "psubsw";
    op_names[OCERZ_OP_PSUBUSB] = "psubusb";
    op_names[OCERZ_OP_PSUBUSW] = "psubusw";
    op_names[OCERZ_OP_PMULLW] = "pmullw";
    op_names[OCERZ_OP_PMULLD] = "pmulld";
    op_names[OCERZ_OP_PMULHW] = "pmulhw";
    op_names[OCERZ_OP_PMULHUW] = "pmulhuw";
    op_names[OCERZ_OP_PMULUDQ] = "pmuludq";
    op_names[OCERZ_OP_PMADDWD] = "pmaddwd";
    op_names[OCERZ_OP_PAVGB] = "pavgb";
    op_names[OCERZ_OP_PAVGW] = "pavgw";
    op_names[OCERZ_OP_PMAXUB] = "pmaxub";
    op_names[OCERZ_OP_PMAXSW] = "pmaxsw";
    op_names[OCERZ_OP_PMINUB] = "pminub";
    op_names[OCERZ_OP_PMINSW] = "pminsw";
    op_names[OCERZ_OP_PMAXSB] = "pmaxsb";
    op_names[OCERZ_OP_PMAXSD] = "pmaxsd";
    op_names[OCERZ_OP_PMAXUW] = "pmaxuw";
    op_names[OCERZ_OP_PMAXUD] = "pmaxud";
    op_names[OCERZ_OP_PMINSB] = "pminsb";
    op_names[OCERZ_OP_PMINSD] = "pminsd";
    op_names[OCERZ_OP_PMINUW] = "pminuw";
    op_names[OCERZ_OP_PMINUD] = "pminud";
    op_names[OCERZ_OP_PSADBW] = "psadbw";
    op_names[OCERZ_OP_PABSB] = "pabsb";
    op_names[OCERZ_OP_PABSW] = "pabsw";
    op_names[OCERZ_OP_PABSD] = "pabsd";
    op_names[OCERZ_OP_PACKSSWB] = "packsswb";
    op_names[OCERZ_OP_PACKSSDW] = "packssdw";
    op_names[OCERZ_OP_PACKUSWB] = "packuswb";
    op_names[OCERZ_OP_PACKUSDW] = "packusdw";
    op_names[OCERZ_OP_PUNPCKLBW] = "punpcklbw";
    op_names[OCERZ_OP_PUNPCKLWD] = "punpcklwd";
    op_names[OCERZ_OP_PUNPCKLDQ] = "punpckldq";
    op_names[OCERZ_OP_PUNPCKLQDQ] = "punpcklqdq";
    op_names[OCERZ_OP_PUNPCKHBW] = "punpckhbw";
    op_names[OCERZ_OP_PUNPCKHWD] = "punpckhwd";
    op_names[OCERZ_OP_PUNPCKHDQ] = "punpckhdq";
    op_names[OCERZ_OP_PUNPCKHQDQ] = "punpckhqdq";
    op_names[OCERZ_OP_PSHUFD] = "pshufd";
    op_names[OCERZ_OP_PSHUFLW] = "pshuflw";
    op_names[OCERZ_OP_PSHUFHW] = "pshufhw";
    op_names[OCERZ_OP_PSHUFB] = "pshufb";
    op_names[OCERZ_OP_PHADDW] = "phaddw";
    op_names[OCERZ_OP_PHADDD] = "phaddd";
    op_names[OCERZ_OP_PHADDSW] = "phaddsw";
    op_names[OCERZ_OP_PHSUBW] = "phsubw";
    op_names[OCERZ_OP_PHSUBD] = "phsubd";
    op_names[OCERZ_OP_PHSUBSW] = "phsubsw";
    op_names[OCERZ_OP_PSIGNB] = "psignb";
    op_names[OCERZ_OP_PSIGNW] = "psignw";
    op_names[OCERZ_OP_PSIGND] = "psignd";
    op_names[OCERZ_OP_PMADDUBSW] = "pmaddubsw";
    op_names[OCERZ_OP_PMULHRSW] = "pmulhrsw";
    op_names[OCERZ_OP_PALIGNR] = "palignr";
    op_names[OCERZ_OP_SHUFPS] = "shufps";
    op_names[OCERZ_OP_SHUFPD] = "shufpd";
    op_names[OCERZ_OP_UNPCKLPS] = "unpcklps";
    op_names[OCERZ_OP_UNPCKHPS] = "unpckhps";
    op_names[OCERZ_OP_UNPCKLPD] = "unpcklpd";
    op_names[OCERZ_OP_UNPCKHPD] = "unpckhpd";
    op_names[OCERZ_OP_PSLLW] = "psllw";
    op_names[OCERZ_OP_PSLLD] = "pslld";
    op_names[OCERZ_OP_PSLLQ] = "psllq";
    op_names[OCERZ_OP_PSRLW] = "psrlw";
    op_names[OCERZ_OP_PSRLD] = "psrld";
    op_names[OCERZ_OP_PSRLQ] = "psrlq";
    op_names[OCERZ_OP_PSRAW] = "psraw";
    op_names[OCERZ_OP_PSRAD] = "psrad";
    op_names[OCERZ_OP_PSLLDQ] = "pslldq";
    op_names[OCERZ_OP_PSRLDQ] = "psrldq";
    op_names[OCERZ_OP_PEXTRB] = "pextrb";
    op_names[OCERZ_OP_PEXTRW] = "pextrw";
    op_names[OCERZ_OP_PEXTRD] = "pextrd";
    op_names[OCERZ_OP_PEXTRQ] = "pextrq";
    op_names[OCERZ_OP_PINSRB] = "pinsrb";
    op_names[OCERZ_OP_PINSRW] = "pinsrw";
    op_names[OCERZ_OP_PINSRD] = "pinsrd";
    op_names[OCERZ_OP_PINSRQ] = "pinsrq";
    op_names[OCERZ_OP_EXTRACTPS] = "extractps";
    op_names[OCERZ_OP_INSERTPS] = "insertps";
    op_names[OCERZ_OP_PTEST] = "ptest";
    op_names[OCERZ_OP_PMOVZXBW] = "pmovzxbw";
    op_names[OCERZ_OP_PMOVZXBD] = "pmovzxbd";
    op_names[OCERZ_OP_PMOVZXBQ] = "pmovzxbq";
    op_names[OCERZ_OP_PMOVZXWD] = "pmovzxwd";
    op_names[OCERZ_OP_PMOVZXWQ] = "pmovzxwq";
    op_names[OCERZ_OP_PMOVZXDQ] = "pmovzxdq";
    op_names[OCERZ_OP_PMOVSXBW] = "pmovsxbw";
    op_names[OCERZ_OP_PMOVSXBD] = "pmovsxbd";
    op_names[OCERZ_OP_PMOVSXBQ] = "pmovsxbq";
    op_names[OCERZ_OP_PMOVSXWD] = "pmovsxwd";
    op_names[OCERZ_OP_PMOVSXWQ] = "pmovsxwq";
    op_names[OCERZ_OP_PMOVSXDQ] = "pmovsxdq";
    op_names[OCERZ_OP_ROUNDPS] = "roundps";
    op_names[OCERZ_OP_ROUNDPD] = "roundpd";
    op_names[OCERZ_OP_ROUNDSS] = "roundss";
    op_names[OCERZ_OP_ROUNDSD] = "roundsd";
    op_names[OCERZ_OP_PBLENDW] = "pblendw";
    op_names[OCERZ_OP_BLENDPS] = "blendps";
    op_names[OCERZ_OP_BLENDPD] = "blendpd";
    op_names[OCERZ_OP_BLENDVPS] = "blendvps";
    op_names[OCERZ_OP_BLENDVPD] = "blendvpd";
    op_names[OCERZ_OP_PBLENDVB] = "pblendvb";
    op_names[OCERZ_OP_AESENC] = "aesenc";
    op_names[OCERZ_OP_AESENCLAST] = "aesenclast";
    op_names[OCERZ_OP_AESDEC] = "aesdec";
    op_names[OCERZ_OP_AESDECLAST] = "aesdeclast";
    op_names[OCERZ_OP_AESIMC] = "aesimc";
    op_names[OCERZ_OP_AESKEYGENASSIST] = "aeskeygenassist";
    op_names[OCERZ_OP_PCLMULQDQ] = "pclmulqdq";
}

const char *ocerz_op_name(unsigned op)
{
    init_op_names();
    if (op >= OCERZ_OP_COUNT)
        return "?";
    const char *n = op_names[op];
    return n ? n : "?";
}

static const char *gpr_name(int reg, int size, int high8)
{
    static const char *n64[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                   "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
    static const char *n32[16] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                   "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
    static const char *n16[16] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                   "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
    static const char *n8[16] = { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                                  "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };
    static const char *nh8[4] = { "ah", "ch", "dh", "bh" };
    if (reg < 0 || reg > 15)
        return "?";
    if (high8 && reg < 4)
        return nh8[reg];
    switch (size) {
    case 1: return n8[reg];
    case 2: return n16[reg];
    case 4: return n32[reg];
    default: return n64[reg];
    }
}

static void fmt_reg(char *b, size_t cap, size_t *n, const X86Operand *op)
{
    int written;
    if (op->kind == OCERZ_OPK_XMM)
        written = snprintf(b + *n, cap > *n ? cap - *n : 0, "xmm%u", op->reg);
    else if (op->kind == OCERZ_OPK_ST)
        written = snprintf(b + *n, cap > *n ? cap - *n : 0, "st%u", op->reg);
    else
        written = snprintf(b + *n, cap > *n ? cap - *n : 0, "%s",
                           gpr_name(op->reg, op->size, op->high8));
    if (written > 0)
        *n += (size_t)written;
}

static const char *size_kw(int size)
{
    switch (size) {
    case 1: return "byte";
    case 2: return "word";
    case 4: return "dword";
    case 8: return "qword";
    case 10: return "tword";
    case 16: return "xmmword";
    default: return "";
    }
}

static void fmt_mem(char *b, size_t cap, size_t *n, const X86Insn *insn, const X86Operand *op)
{
    int written;
    const char *seg = "";
    if (insn->seg == OCERZ_SEG_FS)
        seg = "fs:";
    else if (insn->seg == OCERZ_SEG_GS)
        seg = "gs:";
    if (op->riprel) {
        written = snprintf(b + *n, cap > *n ? cap - *n : 0, "%s [0x%llx]",
                           size_kw(op->size), (unsigned long long)(uint64_t)op->disp);
        if (written > 0)
            *n += (size_t)written;
        return;
    }
    written = snprintf(b + *n, cap > *n ? cap - *n : 0, "%s %s[", size_kw(op->size), seg);
    if (written > 0)
        *n += (size_t)written;
    int any = 0;
    if (op->base != OCERZ_REG_NONE) {
        written = snprintf(b + *n, cap > *n ? cap - *n : 0, "%s", gpr_name(op->base, 8, 0));
        if (written > 0)
            *n += (size_t)written;
        any = 1;
    }
    if (op->index != OCERZ_REG_NONE) {
        written = snprintf(b + *n, cap > *n ? cap - *n : 0, "%s%s*%d", any ? "+" : "",
                           gpr_name(op->index, 8, 0), 1 << op->scale);
        if (written > 0)
            *n += (size_t)written;
        any = 1;
    }
    if (op->disp != 0 || !any) {
        long long d = (long long)op->disp;
        if (d < 0)
            written = snprintf(b + *n, cap > *n ? cap - *n : 0, "-0x%llx", (unsigned long long)(-d));
        else
            written = snprintf(b + *n, cap > *n ? cap - *n : 0, "%s0x%llx", any ? "+" : "",
                               (unsigned long long)d);
        if (written > 0)
            *n += (size_t)written;
    }
    written = snprintf(b + *n, cap > *n ? cap - *n : 0, "]");
    if (written > 0)
        *n += (size_t)written;
}

static void fmt_operand(char *b, size_t cap, size_t *n, const X86Insn *insn, const X86Operand *op)
{
    int written;
    switch (op->kind) {
    case OCERZ_OPK_REG:
    case OCERZ_OPK_XMM:
    case OCERZ_OPK_ST:
        fmt_reg(b, cap, n, op);
        break;
    case OCERZ_OPK_MEM:
        fmt_mem(b, cap, n, insn, op);
        break;
    case OCERZ_OPK_IMM:
        written = snprintf(b + *n, cap > *n ? cap - *n : 0, "0x%llx",
                           (unsigned long long)op->imm);
        if (written > 0)
            *n += (size_t)written;
        break;
    default:
        break;
    }
}

void ocerz_format_insn(const X86Insn *insn, char *buf, size_t cap)
{
    if (cap == 0)
        return;
    size_t n = 0;
    int written = snprintf(buf, cap, "%s", ocerz_op_name(insn->op));
    if (written > 0)
        n += (size_t)written;
    if (insn->rep == OCERZ_REP_REP) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "rep %s", ocerz_op_name(insn->op));
        n = (size_t)snprintf(buf, cap, "%s", tmp);
    } else if (insn->rep == OCERZ_REP_REPNE) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "repne %s", ocerz_op_name(insn->op));
        n = (size_t)snprintf(buf, cap, "%s", tmp);
    }
    for (int i = 0; i < insn->nops; i++) {
        written = snprintf(buf + n, cap > n ? cap - n : 0, "%s", i == 0 ? " " : ", ");
        if (written > 0)
            n += (size_t)written;
        fmt_operand(buf, cap, &n, insn, &insn->ops[i]);
    }
    if (n >= cap)
        buf[cap - 1] = '\0';
}
