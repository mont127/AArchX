/* The SSE through SSE4.1 interpreter tier. */
#include "ocerz/interp_common.h"

#include <math.h>

#define OP(x) (x)

typedef union vec {
    Ocerz128 q;
    float f[4];
    double d[2];
    int8_t i8[16];
    uint8_t u8[16];
    int16_t i16[8];
    uint16_t u16[8];
    int32_t i32[4];
    uint32_t u32[4];
    int64_t i64[2];
    uint64_t u64[2];
} vec;

static inline vec vec_of(Ocerz128 q)
{
    vec v;
    v.q = q;
    return v;
}

static inline vec vec_read(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    return vec_of(ocerz_read_op128(cpu, insn, op));
}

static inline void vec_write(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op, vec v)
{
    ocerz_write_op128(cpu, insn, op, v.q);
}

static inline uint32_t f2bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline uint64_t d2bits(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static inline int8_t sat_s8(int32_t v)
{
    if (v > 127)
        return 127;
    if (v < -128)
        return -128;
    return (int8_t)v;
}

static inline uint8_t sat_u8(int32_t v)
{
    if (v > 255)
        return 255;
    if (v < 0)
        return 0;
    return (uint8_t)v;
}

static inline int16_t sat_s16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

static inline uint16_t sat_u16(int32_t v)
{
    if (v > 65535)
        return 65535;
    if (v < 0)
        return 0;
    return (uint16_t)v;
}

static inline float minf_x86(float a, float b)
{
    return (a < b) ? a : b;
}

static inline float maxf_x86(float a, float b)
{
    return (a > b) ? a : b;
}

static inline double mind_x86(double a, double b)
{
    return (a < b) ? a : b;
}

static inline double maxd_x86(double a, double b)
{
    return (a > b) ? a : b;
}

static int cmp_pred_f(float a, float b, int pred)
{
    int unord = isnan(a) || isnan(b);
    switch (pred & 7) {
    case 0: return !unord && a == b;
    case 1: return !unord && a < b;
    case 2: return !unord && a <= b;
    case 3: return unord;
    case 4: return unord || a != b;
    case 5: return unord || !(a < b);
    case 6: return unord || !(a <= b);
    default: return !unord;
    }
}

static int cmp_pred_d(double a, double b, int pred)
{
    int unord = isnan(a) || isnan(b);
    switch (pred & 7) {
    case 0: return !unord && a == b;
    case 1: return !unord && a < b;
    case 2: return !unord && a <= b;
    case 3: return unord;
    case 4: return unord || a != b;
    case 5: return unord || !(a < b);
    case 6: return unord || !(a <= b);
    default: return !unord;
    }
}

static void set_comis_flags(OcerzCPU *cpu, int unord, int lt, int eq)
{
    ocerz_flag_assign(cpu, OCERZ_OF, 0);
    ocerz_flag_assign(cpu, OCERZ_SF, 0);
    ocerz_flag_assign(cpu, OCERZ_AF, 0);
    if (unord) {
        ocerz_flag_assign(cpu, OCERZ_ZF, 1);
        ocerz_flag_assign(cpu, OCERZ_PF, 1);
        ocerz_flag_assign(cpu, OCERZ_CF, 1);
    } else {
        ocerz_flag_assign(cpu, OCERZ_PF, 0);
        ocerz_flag_assign(cpu, OCERZ_ZF, eq);
        ocerz_flag_assign(cpu, OCERZ_CF, lt);
    }
}

static int32_t cvt_f2i32(float f, int trunc_mode)
{
    if (isnan(f) || f >= 2147483648.0f || f < -2147483648.0f)
        return (int32_t)0x80000000u;
    return trunc_mode ? (int32_t)truncf(f) : (int32_t)lrintf(f);
}

static int64_t cvt_f2i64(float f, int trunc_mode)
{
    if (isnan(f) || f >= 9223372036854775808.0f || f < -9223372036854775808.0f)
        return (int64_t)0x8000000000000000ull;
    return trunc_mode ? (int64_t)truncf(f) : (int64_t)llrintf(f);
}

static int32_t cvt_d2i32(double d, int trunc_mode)
{
    if (isnan(d) || d >= 2147483648.0 || d < -2147483648.0)
        return (int32_t)0x80000000u;
    return trunc_mode ? (int32_t)trunc(d) : (int32_t)lrint(d);
}

static int64_t cvt_d2i64(double d, int trunc_mode)
{
    if (isnan(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0)
        return (int64_t)0x8000000000000000ull;
    return trunc_mode ? (int64_t)trunc(d) : (int64_t)llrint(d);
}

static float round_imm_f(float f, int imm)
{
    if (imm & 4)
        return rintf(f);
    switch (imm & 3) {
    case 0: return rintf(f);
    case 1: return floorf(f);
    case 2: return ceilf(f);
    default: return truncf(f);
    }
}

static double round_imm_d(double d, int imm)
{
    if (imm & 4)
        return rint(d);
    switch (imm & 3) {
    case 0: return rint(d);
    case 1: return floor(d);
    case 2: return ceil(d);
    default: return trunc(d);
    }
}

static int do_moves(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    switch (insn->op) {
    case OP(OCERZ_OP_MOVUPS):
    case OP(OCERZ_OP_MOVAPS):
    case OP(OCERZ_OP_MOVDQA):
    case OP(OCERZ_OP_MOVDQU):
        ocerz_write_op128(cpu, insn, d, ocerz_read_op128(cpu, insn, s));
        return OCERZ_STEP_OK;
    case OP(OCERZ_OP_MOVSS): {
        if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_XMM) {
            cpu->xmm[d->reg].lo = (cpu->xmm[d->reg].lo & ~(uint64_t)0xffffffff)
                                  | (uint32_t)cpu->xmm[s->reg].lo;
        } else if (d->kind == OCERZ_OPK_XMM) {
            Ocerz128 v = { 0, 0 };
            v.lo = ocerz_ld(ocerz_ea(cpu, insn, s), 4) & 0xffffffff;
            cpu->xmm[d->reg] = v;
        } else {
            ocerz_st(ocerz_ea(cpu, insn, d), 4, (uint32_t)cpu->xmm[s->reg].lo);
        }
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVSDX): {
        if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_XMM) {
            cpu->xmm[d->reg].lo = cpu->xmm[s->reg].lo;
        } else if (d->kind == OCERZ_OPK_XMM) {
            Ocerz128 v = { 0, 0 };
            v.lo = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
            cpu->xmm[d->reg] = v;
        } else {
            ocerz_st(ocerz_ea(cpu, insn, d), 8, cpu->xmm[s->reg].lo);
        }
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVD): {
        if (d->kind == OCERZ_OPK_XMM) {
            uint32_t v = (uint32_t)ocerz_read_op(cpu, insn, s);
            cpu->xmm[d->reg].lo = v;
            cpu->xmm[d->reg].hi = 0;
        } else {
            ocerz_write_op(cpu, insn, d, (uint32_t)cpu->xmm[s->reg].lo);
        }
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVQX): {
        if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_XMM) {
            cpu->xmm[d->reg].lo = cpu->xmm[s->reg].lo;
            cpu->xmm[d->reg].hi = 0;
        } else if (d->kind == OCERZ_OPK_XMM && s->kind == OCERZ_OPK_MEM) {
            cpu->xmm[d->reg].lo = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
            cpu->xmm[d->reg].hi = 0;
        } else if (d->kind == OCERZ_OPK_XMM) {
            cpu->xmm[d->reg].lo = ocerz_read_op(cpu, insn, s);
            cpu->xmm[d->reg].hi = 0;
        } else if (d->kind == OCERZ_OPK_MEM) {
            ocerz_st(ocerz_ea(cpu, insn, d), 8, cpu->xmm[s->reg].lo);
        } else {
            ocerz_write_op(cpu, insn, d, cpu->xmm[s->reg].lo);
        }
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVLPS): {
        if (d->kind == OCERZ_OPK_XMM)
            cpu->xmm[d->reg].lo = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
        else
            ocerz_st(ocerz_ea(cpu, insn, d), 8, cpu->xmm[s->reg].lo);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVHPS): {
        if (d->kind == OCERZ_OPK_XMM)
            cpu->xmm[d->reg].hi = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
        else
            ocerz_st(ocerz_ea(cpu, insn, d), 8, cpu->xmm[s->reg].hi);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVLHPS):
        cpu->xmm[d->reg].hi = cpu->xmm[s->reg].lo;
        return OCERZ_STEP_OK;
    case OP(OCERZ_OP_MOVHLPS):
        cpu->xmm[d->reg].lo = cpu->xmm[s->reg].hi;
        return OCERZ_STEP_OK;
    case OP(OCERZ_OP_MOVMSKPS): {
        vec v = vec_of(cpu->xmm[s->reg]);
        uint32_t m = 0;
        for (int i = 0; i < 4; i++)
            m |= (uint32_t)((v.u32[i] >> 31) & 1) << i;
        ocerz_write_gpr(cpu, d->reg, 4, 0, m);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVMSKPD): {
        vec v = vec_of(cpu->xmm[s->reg]);
        uint32_t m = 0;
        for (int i = 0; i < 2; i++)
            m |= (uint32_t)((v.u64[i] >> 63) & 1) << i;
        ocerz_write_gpr(cpu, d->reg, 4, 0, m);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PMOVMSKB): {
        vec v = vec_of(cpu->xmm[s->reg]);
        uint32_t m = 0;
        for (int i = 0; i < 16; i++)
            m |= (uint32_t)((v.u8[i] >> 7) & 1) << i;
        ocerz_write_gpr(cpu, d->reg, 4, 0, m);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVSHDUP): {
        vec sv = vec_read(cpu, insn, s);
        vec r;
        r.u32[0] = sv.u32[1];
        r.u32[1] = sv.u32[1];
        r.u32[2] = sv.u32[3];
        r.u32[3] = sv.u32[3];
        vec_write(cpu, insn, d, r);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVSLDUP): {
        vec sv = vec_read(cpu, insn, s);
        vec r;
        r.u32[0] = sv.u32[0];
        r.u32[1] = sv.u32[0];
        r.u32[2] = sv.u32[2];
        r.u32[3] = sv.u32[2];
        vec_write(cpu, insn, d, r);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_MOVDDUP): {
        vec r;
        if (s->kind == OCERZ_OPK_XMM) {
            r.u64[0] = cpu->xmm[s->reg].lo;
        } else {
            r.u64[0] = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
        }
        r.u64[1] = r.u64[0];
        vec_write(cpu, insn, d, r);
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

static double fixnan_d(double a, double b, double res)
{
    if (!isnan(res))
        return res;
    uint64_t u;
    if (isnan(a)) {
        memcpy(&u, &a, 8);
        u |= 0x0008000000000000ull;
    } else if (isnan(b)) {
        memcpy(&u, &b, 8);
        u |= 0x0008000000000000ull;
    } else {
        u = 0xfff8000000000000ull;
    }
    memcpy(&res, &u, 8);
    return res;
}

static float fixnan_f(float a, float b, float res)
{
    if (!isnan(res))
        return res;
    uint32_t u;
    if (isnan(a)) {
        memcpy(&u, &a, 4);
        u |= 0x00400000u;
    } else if (isnan(b)) {
        memcpy(&u, &b, 4);
        u |= 0x00400000u;
    } else {
        u = 0xffc00000u;
    }
    memcpy(&res, &u, 4);
    return res;
}

static int do_fp_arith(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    vec a = vec_of(cpu->xmm[d->reg]);
    vec b = vec_read(cpu, insn, s);
    vec r = a;
    switch (insn->op) {
    case OP(OCERZ_OP_ADDPS): for (int i = 0; i < 4; i++) r.f[i] = fixnan_f(a.f[i], b.f[i], a.f[i] + b.f[i]); break;
    case OP(OCERZ_OP_HADDPS):
        r.f[0] = fixnan_f(a.f[0], a.f[1], a.f[0] + a.f[1]);
        r.f[1] = fixnan_f(a.f[2], a.f[3], a.f[2] + a.f[3]);
        r.f[2] = fixnan_f(b.f[0], b.f[1], b.f[0] + b.f[1]);
        r.f[3] = fixnan_f(b.f[2], b.f[3], b.f[2] + b.f[3]);
        break;
    case OP(OCERZ_OP_HSUBPS):
        r.f[0] = fixnan_f(a.f[0], a.f[1], a.f[0] - a.f[1]);
        r.f[1] = fixnan_f(a.f[2], a.f[3], a.f[2] - a.f[3]);
        r.f[2] = fixnan_f(b.f[0], b.f[1], b.f[0] - b.f[1]);
        r.f[3] = fixnan_f(b.f[2], b.f[3], b.f[2] - b.f[3]);
        break;
    case OP(OCERZ_OP_HADDPD):
        r.d[0] = fixnan_d(a.d[0], a.d[1], a.d[0] + a.d[1]);
        r.d[1] = fixnan_d(b.d[0], b.d[1], b.d[0] + b.d[1]);
        break;
    case OP(OCERZ_OP_HSUBPD):
        r.d[0] = fixnan_d(a.d[0], a.d[1], a.d[0] - a.d[1]);
        r.d[1] = fixnan_d(b.d[0], b.d[1], b.d[0] - b.d[1]);
        break;
    case OP(OCERZ_OP_ADDSUBPS):
        r.f[0] = fixnan_f(a.f[0], b.f[0], a.f[0] - b.f[0]);
        r.f[1] = fixnan_f(a.f[1], b.f[1], a.f[1] + b.f[1]);
        r.f[2] = fixnan_f(a.f[2], b.f[2], a.f[2] - b.f[2]);
        r.f[3] = fixnan_f(a.f[3], b.f[3], a.f[3] + b.f[3]);
        break;
    case OP(OCERZ_OP_ADDSUBPD):
        r.d[0] = fixnan_d(a.d[0], b.d[0], a.d[0] - b.d[0]);
        r.d[1] = fixnan_d(a.d[1], b.d[1], a.d[1] + b.d[1]);
        break;
    case OP(OCERZ_OP_SUBPS): for (int i = 0; i < 4; i++) r.f[i] = fixnan_f(a.f[i], b.f[i], a.f[i] - b.f[i]); break;
    case OP(OCERZ_OP_MULPS): for (int i = 0; i < 4; i++) r.f[i] = fixnan_f(a.f[i], b.f[i], a.f[i] * b.f[i]); break;
    case OP(OCERZ_OP_DIVPS): for (int i = 0; i < 4; i++) r.f[i] = fixnan_f(a.f[i], b.f[i], a.f[i] / b.f[i]); break;
    case OP(OCERZ_OP_MINPS): for (int i = 0; i < 4; i++) r.f[i] = minf_x86(a.f[i], b.f[i]); break;
    case OP(OCERZ_OP_MAXPS): for (int i = 0; i < 4; i++) r.f[i] = maxf_x86(a.f[i], b.f[i]); break;
    case OP(OCERZ_OP_SQRTPS): for (int i = 0; i < 4; i++) r.f[i] = fixnan_f(b.f[i], b.f[i], sqrtf(b.f[i])); break;
    case OP(OCERZ_OP_RSQRTPS): for (int i = 0; i < 4; i++) r.f[i] = 1.0f / sqrtf(b.f[i]); break;
    case OP(OCERZ_OP_RCPPS): for (int i = 0; i < 4; i++) r.f[i] = 1.0f / b.f[i]; break;

    case OP(OCERZ_OP_ADDPD): for (int i = 0; i < 2; i++) r.d[i] = fixnan_d(a.d[i], b.d[i], a.d[i] + b.d[i]); break;
    case OP(OCERZ_OP_SUBPD): for (int i = 0; i < 2; i++) r.d[i] = fixnan_d(a.d[i], b.d[i], a.d[i] - b.d[i]); break;
    case OP(OCERZ_OP_MULPD): for (int i = 0; i < 2; i++) r.d[i] = fixnan_d(a.d[i], b.d[i], a.d[i] * b.d[i]); break;
    case OP(OCERZ_OP_DIVPD): for (int i = 0; i < 2; i++) r.d[i] = fixnan_d(a.d[i], b.d[i], a.d[i] / b.d[i]); break;
    case OP(OCERZ_OP_MINPD): for (int i = 0; i < 2; i++) r.d[i] = mind_x86(a.d[i], b.d[i]); break;
    case OP(OCERZ_OP_MAXPD): for (int i = 0; i < 2; i++) r.d[i] = maxd_x86(a.d[i], b.d[i]); break;
    case OP(OCERZ_OP_SQRTPD): for (int i = 0; i < 2; i++) r.d[i] = fixnan_d(b.d[i], b.d[i], sqrt(b.d[i])); break;

    case OP(OCERZ_OP_ADDSS): r.f[0] = fixnan_f(a.f[0], b.f[0], a.f[0] + b.f[0]); break;
    case OP(OCERZ_OP_SUBSS): r.f[0] = fixnan_f(a.f[0], b.f[0], a.f[0] - b.f[0]); break;
    case OP(OCERZ_OP_MULSS): r.f[0] = fixnan_f(a.f[0], b.f[0], a.f[0] * b.f[0]); break;
    case OP(OCERZ_OP_DIVSS): r.f[0] = fixnan_f(a.f[0], b.f[0], a.f[0] / b.f[0]); break;
    case OP(OCERZ_OP_MINSS): r.f[0] = minf_x86(a.f[0], b.f[0]); break;
    case OP(OCERZ_OP_MAXSS): r.f[0] = maxf_x86(a.f[0], b.f[0]); break;
    case OP(OCERZ_OP_SQRTSS): r.f[0] = fixnan_f(b.f[0], b.f[0], sqrtf(b.f[0])); break;
    case OP(OCERZ_OP_RSQRTSS): r.f[0] = 1.0f / sqrtf(b.f[0]); break;
    case OP(OCERZ_OP_RCPSS): r.f[0] = 1.0f / b.f[0]; break;

    case OP(OCERZ_OP_ADDSD): r.d[0] = fixnan_d(a.d[0], b.d[0], a.d[0] + b.d[0]); break;
    case OP(OCERZ_OP_SUBSD): r.d[0] = fixnan_d(a.d[0], b.d[0], a.d[0] - b.d[0]); break;
    case OP(OCERZ_OP_MULSD): r.d[0] = fixnan_d(a.d[0], b.d[0], a.d[0] * b.d[0]); break;
    case OP(OCERZ_OP_DIVSD): r.d[0] = fixnan_d(a.d[0], b.d[0], a.d[0] / b.d[0]); break;
    case OP(OCERZ_OP_MINSD): r.d[0] = mind_x86(a.d[0], b.d[0]); break;
    case OP(OCERZ_OP_MAXSD): r.d[0] = maxd_x86(a.d[0], b.d[0]); break;
    case OP(OCERZ_OP_SQRTSD): r.d[0] = fixnan_d(b.d[0], b.d[0], sqrt(b.d[0])); break;
    default:
        return OCERZ_EUNSUP;
    }
    cpu->xmm[d->reg] = r.q;
    return OCERZ_STEP_OK;
}

static int do_logic(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    Ocerz128 a = cpu->xmm[d->reg];
    Ocerz128 b = ocerz_read_op128(cpu, insn, s);
    Ocerz128 r;
    switch (insn->op) {
    case OP(OCERZ_OP_ANDPS):
    case OP(OCERZ_OP_PAND):
        r.lo = a.lo & b.lo; r.hi = a.hi & b.hi; break;
    case OP(OCERZ_OP_ANDNPS):
    case OP(OCERZ_OP_PANDN):
        r.lo = ~a.lo & b.lo; r.hi = ~a.hi & b.hi; break;
    case OP(OCERZ_OP_ORPS):
    case OP(OCERZ_OP_POR):
        r.lo = a.lo | b.lo; r.hi = a.hi | b.hi; break;
    case OP(OCERZ_OP_XORPS):
    case OP(OCERZ_OP_PXOR):
        r.lo = a.lo ^ b.lo; r.hi = a.hi ^ b.hi; break;
    default:
        return OCERZ_EUNSUP;
    }
    cpu->xmm[d->reg] = r;
    return OCERZ_STEP_OK;
}

static int do_cmpfp(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    int pred = (int)(insn->ops[2].imm & 7);
    vec a = vec_of(cpu->xmm[d->reg]);
    vec b = vec_read(cpu, insn, s);
    vec r = a;
    switch (insn->op) {
    case OP(OCERZ_OP_CMPPS):
        for (int i = 0; i < 4; i++)
            r.u32[i] = cmp_pred_f(a.f[i], b.f[i], pred) ? 0xffffffffu : 0;
        break;
    case OP(OCERZ_OP_CMPSS):
        r.u32[0] = cmp_pred_f(a.f[0], b.f[0], pred) ? 0xffffffffu : 0;
        break;
    case OP(OCERZ_OP_CMPPD):
        for (int i = 0; i < 2; i++)
            r.u64[i] = cmp_pred_d(a.d[i], b.d[i], pred) ? ~(uint64_t)0 : 0;
        break;
    case OP(OCERZ_OP_CMPSDX):
        r.u64[0] = cmp_pred_d(a.d[0], b.d[0], pred) ? ~(uint64_t)0 : 0;
        break;
    default:
        return OCERZ_EUNSUP;
    }
    cpu->xmm[d->reg] = r.q;
    return OCERZ_STEP_OK;
}

static int do_comis(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    if (insn->op == OP(OCERZ_OP_COMISS) || insn->op == OP(OCERZ_OP_UCOMISS)) {
        vec a = vec_of(cpu->xmm[d->reg]);
        vec b = vec_read(cpu, insn, s);
        float x = a.f[0], y = b.f[0];
        int unord = isnan(x) || isnan(y);
        set_comis_flags(cpu, unord, !unord && x < y, !unord && x == y);
    } else {
        vec a = vec_of(cpu->xmm[d->reg]);
        vec b = vec_read(cpu, insn, s);
        double x = a.d[0], y = b.d[0];
        int unord = isnan(x) || isnan(y);
        set_comis_flags(cpu, unord, !unord && x < y, !unord && x == y);
    }
    return OCERZ_STEP_OK;
}

static int do_pcmp(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    vec a = vec_of(cpu->xmm[d->reg]);
    vec b = vec_read(cpu, insn, s);
    vec r;
    switch (insn->op) {
    case OP(OCERZ_OP_PCMPEQB): for (int i = 0; i < 16; i++) r.u8[i] = (a.u8[i] == b.u8[i]) ? 0xff : 0; break;
    case OP(OCERZ_OP_PCMPEQW): for (int i = 0; i < 8; i++) r.u16[i] = (a.u16[i] == b.u16[i]) ? 0xffff : 0; break;
    case OP(OCERZ_OP_PCMPEQD): for (int i = 0; i < 4; i++) r.u32[i] = (a.u32[i] == b.u32[i]) ? 0xffffffffu : 0; break;
    case OP(OCERZ_OP_PCMPEQQ): for (int i = 0; i < 2; i++) r.u64[i] = (a.u64[i] == b.u64[i]) ? ~(uint64_t)0 : 0; break;
    case OP(OCERZ_OP_PCMPGTB): for (int i = 0; i < 16; i++) r.u8[i] = (a.i8[i] > b.i8[i]) ? 0xff : 0; break;
    case OP(OCERZ_OP_PCMPGTW): for (int i = 0; i < 8; i++) r.u16[i] = (a.i16[i] > b.i16[i]) ? 0xffff : 0; break;
    case OP(OCERZ_OP_PCMPGTD): for (int i = 0; i < 4; i++) r.u32[i] = (a.i32[i] > b.i32[i]) ? 0xffffffffu : 0; break;
    case OP(OCERZ_OP_PCMPGTQ): for (int i = 0; i < 2; i++) r.u64[i] = (a.i64[i] > b.i64[i]) ? ~(uint64_t)0 : 0; break;
    default:
        return OCERZ_EUNSUP;
    }
    cpu->xmm[d->reg] = r.q;
    return OCERZ_STEP_OK;
}

static int do_convert(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    switch (insn->op) {
    case OP(OCERZ_OP_CVTSI2SS): {
        uint64_t g = ocerz_read_op(cpu, insn, s);
        float f = (s->size == 8) ? (float)(int64_t)g : (float)(int32_t)g;
        cpu->xmm[d->reg].lo = (cpu->xmm[d->reg].lo & ~(uint64_t)0xffffffff) | f2bits(f);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTSI2SD): {
        uint64_t g = ocerz_read_op(cpu, insn, s);
        double dv = (s->size == 8) ? (double)(int64_t)g : (double)(int32_t)g;
        cpu->xmm[d->reg].lo = d2bits(dv);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTSS2SI):
    case OP(OCERZ_OP_CVTTSS2SI): {
        vec b = vec_read(cpu, insn, s);
        int trunc_mode = (insn->op == OP(OCERZ_OP_CVTTSS2SI));
        if (d->size == 8)
            ocerz_write_op(cpu, insn, d, (uint64_t)cvt_f2i64(b.f[0], trunc_mode));
        else
            ocerz_write_op(cpu, insn, d, (uint32_t)cvt_f2i32(b.f[0], trunc_mode));
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTSD2SI):
    case OP(OCERZ_OP_CVTTSD2SI): {
        vec b = vec_read(cpu, insn, s);
        int trunc_mode = (insn->op == OP(OCERZ_OP_CVTTSD2SI));
        if (d->size == 8)
            ocerz_write_op(cpu, insn, d, (uint64_t)cvt_d2i64(b.d[0], trunc_mode));
        else
            ocerz_write_op(cpu, insn, d, (uint32_t)cvt_d2i32(b.d[0], trunc_mode));
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTSS2SD): {
        vec b = vec_read(cpu, insn, s);
        cpu->xmm[d->reg].lo = d2bits((double)b.f[0]);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTSD2SS): {
        vec b = vec_read(cpu, insn, s);
        cpu->xmm[d->reg].lo = (cpu->xmm[d->reg].lo & ~(uint64_t)0xffffffff) | f2bits((float)b.d[0]);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTPS2PD): {
        vec b = vec_read(cpu, insn, s);
        vec r;
        r.d[0] = (double)b.f[0];
        r.d[1] = (double)b.f[1];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTPD2PS): {
        vec b = vec_read(cpu, insn, s);
        vec r;
        r.q.lo = 0;
        r.q.hi = 0;
        r.f[0] = (float)b.d[0];
        r.f[1] = (float)b.d[1];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTDQ2PS): {
        vec b = vec_read(cpu, insn, s);
        vec r;
        for (int i = 0; i < 4; i++)
            r.f[i] = (float)b.i32[i];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTPS2DQ):
    case OP(OCERZ_OP_CVTTPS2DQ): {
        vec b = vec_read(cpu, insn, s);
        int trunc_mode = (insn->op == OP(OCERZ_OP_CVTTPS2DQ));
        vec r;
        for (int i = 0; i < 4; i++)
            r.i32[i] = cvt_f2i32(b.f[i], trunc_mode);
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTDQ2PD): {
        vec b = vec_read(cpu, insn, s);
        vec r;
        r.d[0] = (double)b.i32[0];
        r.d[1] = (double)b.i32[1];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_CVTPD2DQ):
    case OP(OCERZ_OP_CVTTPD2DQ): {
        vec b = vec_read(cpu, insn, s);
        int trunc_mode = (insn->op == OP(OCERZ_OP_CVTTPD2DQ));
        vec r;
        r.q.lo = 0;
        r.q.hi = 0;
        r.i32[0] = cvt_d2i32(b.d[0], trunc_mode);
        r.i32[1] = cvt_d2i32(b.d[1], trunc_mode);
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

static int do_int_arith(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    vec a = vec_of(cpu->xmm[d->reg]);
    vec b = vec_read(cpu, insn, s);
    vec r = a;
    switch (insn->op) {
    case OP(OCERZ_OP_PADDB): for (int i = 0; i < 16; i++) r.u8[i] = (uint8_t)(a.u8[i] + b.u8[i]); break;
    case OP(OCERZ_OP_PADDW): for (int i = 0; i < 8; i++) r.u16[i] = (uint16_t)(a.u16[i] + b.u16[i]); break;
    case OP(OCERZ_OP_PADDD): for (int i = 0; i < 4; i++) r.u32[i] = a.u32[i] + b.u32[i]; break;
    case OP(OCERZ_OP_PADDQ): for (int i = 0; i < 2; i++) r.u64[i] = a.u64[i] + b.u64[i]; break;
    case OP(OCERZ_OP_PSUBB): for (int i = 0; i < 16; i++) r.u8[i] = (uint8_t)(a.u8[i] - b.u8[i]); break;
    case OP(OCERZ_OP_PSUBW): for (int i = 0; i < 8; i++) r.u16[i] = (uint16_t)(a.u16[i] - b.u16[i]); break;
    case OP(OCERZ_OP_PSUBD): for (int i = 0; i < 4; i++) r.u32[i] = a.u32[i] - b.u32[i]; break;
    case OP(OCERZ_OP_PSUBQ): for (int i = 0; i < 2; i++) r.u64[i] = a.u64[i] - b.u64[i]; break;
    case OP(OCERZ_OP_PADDSB): for (int i = 0; i < 16; i++) r.i8[i] = sat_s8((int32_t)a.i8[i] + b.i8[i]); break;
    case OP(OCERZ_OP_PADDSW): for (int i = 0; i < 8; i++) r.i16[i] = sat_s16((int32_t)a.i16[i] + b.i16[i]); break;
    case OP(OCERZ_OP_PADDUSB): for (int i = 0; i < 16; i++) r.u8[i] = sat_u8((int32_t)a.u8[i] + b.u8[i]); break;
    case OP(OCERZ_OP_PADDUSW): for (int i = 0; i < 8; i++) r.u16[i] = sat_u16((int32_t)a.u16[i] + b.u16[i]); break;
    case OP(OCERZ_OP_PSUBSB): for (int i = 0; i < 16; i++) r.i8[i] = sat_s8((int32_t)a.i8[i] - b.i8[i]); break;
    case OP(OCERZ_OP_PSUBSW): for (int i = 0; i < 8; i++) r.i16[i] = sat_s16((int32_t)a.i16[i] - b.i16[i]); break;
    case OP(OCERZ_OP_PSUBUSB): for (int i = 0; i < 16; i++) r.u8[i] = sat_u8((int32_t)a.u8[i] - b.u8[i]); break;
    case OP(OCERZ_OP_PSUBUSW): for (int i = 0; i < 8; i++) r.u16[i] = sat_u16((int32_t)a.u16[i] - b.u16[i]); break;
    case OP(OCERZ_OP_PMULLW): for (int i = 0; i < 8; i++) r.u16[i] = (uint16_t)((int32_t)a.i16[i] * b.i16[i]); break;
    case OP(OCERZ_OP_PMULLD): for (int i = 0; i < 4; i++) r.u32[i] = (uint32_t)((int64_t)a.i32[i] * b.i32[i]); break;
    case OP(OCERZ_OP_PMULHW): for (int i = 0; i < 8; i++) r.i16[i] = (int16_t)(((int32_t)a.i16[i] * b.i16[i]) >> 16); break;
    case OP(OCERZ_OP_PMULHUW): for (int i = 0; i < 8; i++) r.u16[i] = (uint16_t)(((uint32_t)a.u16[i] * b.u16[i]) >> 16); break;
    case OP(OCERZ_OP_PMULUDQ):
        r.u64[0] = (uint64_t)a.u32[0] * b.u32[0];
        r.u64[1] = (uint64_t)a.u32[2] * b.u32[2];
        break;
    case OP(OCERZ_OP_PMADDWD):
        for (int i = 0; i < 4; i++)
            r.i32[i] = (int32_t)a.i16[2 * i] * b.i16[2 * i]
                       + (int32_t)a.i16[2 * i + 1] * b.i16[2 * i + 1];
        break;
    case OP(OCERZ_OP_PAVGB): for (int i = 0; i < 16; i++) r.u8[i] = (uint8_t)(((uint32_t)a.u8[i] + b.u8[i] + 1) >> 1); break;
    case OP(OCERZ_OP_PAVGW): for (int i = 0; i < 8; i++) r.u16[i] = (uint16_t)(((uint32_t)a.u16[i] + b.u16[i] + 1) >> 1); break;
    case OP(OCERZ_OP_PMAXUB): for (int i = 0; i < 16; i++) r.u8[i] = a.u8[i] > b.u8[i] ? a.u8[i] : b.u8[i]; break;
    case OP(OCERZ_OP_PMINUB): for (int i = 0; i < 16; i++) r.u8[i] = a.u8[i] < b.u8[i] ? a.u8[i] : b.u8[i]; break;
    case OP(OCERZ_OP_PMAXSW): for (int i = 0; i < 8; i++) r.i16[i] = a.i16[i] > b.i16[i] ? a.i16[i] : b.i16[i]; break;
    case OP(OCERZ_OP_PMINSW): for (int i = 0; i < 8; i++) r.i16[i] = a.i16[i] < b.i16[i] ? a.i16[i] : b.i16[i]; break;
    case OP(OCERZ_OP_PMAXSB): for (int i = 0; i < 16; i++) r.i8[i] = a.i8[i] > b.i8[i] ? a.i8[i] : b.i8[i]; break;
    case OP(OCERZ_OP_PMINSB): for (int i = 0; i < 16; i++) r.i8[i] = a.i8[i] < b.i8[i] ? a.i8[i] : b.i8[i]; break;
    case OP(OCERZ_OP_PMAXUW): for (int i = 0; i < 8; i++) r.u16[i] = a.u16[i] > b.u16[i] ? a.u16[i] : b.u16[i]; break;
    case OP(OCERZ_OP_PMINUW): for (int i = 0; i < 8; i++) r.u16[i] = a.u16[i] < b.u16[i] ? a.u16[i] : b.u16[i]; break;
    case OP(OCERZ_OP_PMAXSD): for (int i = 0; i < 4; i++) r.i32[i] = a.i32[i] > b.i32[i] ? a.i32[i] : b.i32[i]; break;
    case OP(OCERZ_OP_PMINSD): for (int i = 0; i < 4; i++) r.i32[i] = a.i32[i] < b.i32[i] ? a.i32[i] : b.i32[i]; break;
    case OP(OCERZ_OP_PMAXUD): for (int i = 0; i < 4; i++) r.u32[i] = a.u32[i] > b.u32[i] ? a.u32[i] : b.u32[i]; break;
    case OP(OCERZ_OP_PMINUD): for (int i = 0; i < 4; i++) r.u32[i] = a.u32[i] < b.u32[i] ? a.u32[i] : b.u32[i]; break;
    case OP(OCERZ_OP_PSADBW): {
        uint32_t lo = 0, hi = 0;
        for (int i = 0; i < 8; i++) {
            int dlo = (int)a.u8[i] - b.u8[i];
            int dhi = (int)a.u8[i + 8] - b.u8[i + 8];
            lo += (uint32_t)(dlo < 0 ? -dlo : dlo);
            hi += (uint32_t)(dhi < 0 ? -dhi : dhi);
        }
        r.q.lo = 0;
        r.q.hi = 0;
        r.u16[0] = (uint16_t)lo;
        r.u16[4] = (uint16_t)hi;
        break;
    }
    case OP(OCERZ_OP_PABSB): for (int i = 0; i < 16; i++) r.u8[i] = (uint8_t)(b.i8[i] < 0 ? -b.i8[i] : b.i8[i]); break;
    case OP(OCERZ_OP_PABSW): for (int i = 0; i < 8; i++) r.u16[i] = (uint16_t)(b.i16[i] < 0 ? -b.i16[i] : b.i16[i]); break;
    case OP(OCERZ_OP_PABSD): for (int i = 0; i < 4; i++) r.u32[i] = (uint32_t)(b.i32[i] < 0 ? -(int64_t)b.i32[i] : b.i32[i]); break;
    case OP(OCERZ_OP_PHADDW):
        for (int i = 0; i < 4; i++) r.u16[i] = (uint16_t)(a.i16[2 * i] + a.i16[2 * i + 1]);
        for (int i = 0; i < 4; i++) r.u16[i + 4] = (uint16_t)(b.i16[2 * i] + b.i16[2 * i + 1]);
        break;
    case OP(OCERZ_OP_PHADDD):
        for (int i = 0; i < 2; i++) r.u32[i] = (uint32_t)(a.i32[2 * i] + a.i32[2 * i + 1]);
        for (int i = 0; i < 2; i++) r.u32[i + 2] = (uint32_t)(b.i32[2 * i] + b.i32[2 * i + 1]);
        break;
    case OP(OCERZ_OP_PHADDSW):
        for (int i = 0; i < 4; i++) r.i16[i] = sat_s16((int32_t)a.i16[2 * i] + a.i16[2 * i + 1]);
        for (int i = 0; i < 4; i++) r.i16[i + 4] = sat_s16((int32_t)b.i16[2 * i] + b.i16[2 * i + 1]);
        break;
    case OP(OCERZ_OP_PHSUBW):
        for (int i = 0; i < 4; i++) r.u16[i] = (uint16_t)(a.i16[2 * i] - a.i16[2 * i + 1]);
        for (int i = 0; i < 4; i++) r.u16[i + 4] = (uint16_t)(b.i16[2 * i] - b.i16[2 * i + 1]);
        break;
    case OP(OCERZ_OP_PHSUBD):
        for (int i = 0; i < 2; i++) r.u32[i] = (uint32_t)(a.i32[2 * i] - a.i32[2 * i + 1]);
        for (int i = 0; i < 2; i++) r.u32[i + 2] = (uint32_t)(b.i32[2 * i] - b.i32[2 * i + 1]);
        break;
    case OP(OCERZ_OP_PHSUBSW):
        for (int i = 0; i < 4; i++) r.i16[i] = sat_s16((int32_t)a.i16[2 * i] - a.i16[2 * i + 1]);
        for (int i = 0; i < 4; i++) r.i16[i + 4] = sat_s16((int32_t)b.i16[2 * i] - b.i16[2 * i + 1]);
        break;
    case OP(OCERZ_OP_PSIGNB):
        for (int i = 0; i < 16; i++) r.i8[i] = b.i8[i] < 0 ? (int8_t)-a.i8[i] : (b.i8[i] == 0 ? 0 : a.i8[i]);
        break;
    case OP(OCERZ_OP_PSIGNW):
        for (int i = 0; i < 8; i++) r.i16[i] = b.i16[i] < 0 ? (int16_t)-a.i16[i] : (b.i16[i] == 0 ? 0 : a.i16[i]);
        break;
    case OP(OCERZ_OP_PSIGND):
        for (int i = 0; i < 4; i++) r.i32[i] = b.i32[i] < 0 ? (int32_t)-a.i32[i] : (b.i32[i] == 0 ? 0 : a.i32[i]);
        break;
    case OP(OCERZ_OP_PMADDUBSW):
        for (int i = 0; i < 8; i++)
            r.i16[i] = sat_s16((int32_t)a.u8[2 * i] * b.i8[2 * i]
                               + (int32_t)a.u8[2 * i + 1] * b.i8[2 * i + 1]);
        break;
    case OP(OCERZ_OP_PMULHRSW):
        for (int i = 0; i < 8; i++)
            r.i16[i] = (int16_t)(((((int32_t)a.i16[i] * b.i16[i]) >> 14) + 1) >> 1);
        break;
    default:
        return OCERZ_EUNSUP;
    }
    cpu->xmm[d->reg] = r.q;
    return OCERZ_STEP_OK;
}

static int do_pack_shuffle(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    vec a = vec_of(cpu->xmm[d->reg]);
    vec b = vec_read(cpu, insn, s);
    vec r;
    switch (insn->op) {
    case OP(OCERZ_OP_PACKSSWB):
        for (int i = 0; i < 8; i++) r.i8[i] = sat_s8(a.i16[i]);
        for (int i = 0; i < 8; i++) r.i8[i + 8] = sat_s8(b.i16[i]);
        break;
    case OP(OCERZ_OP_PACKSSDW):
        for (int i = 0; i < 4; i++) r.i16[i] = sat_s16(a.i32[i]);
        for (int i = 0; i < 4; i++) r.i16[i + 4] = sat_s16(b.i32[i]);
        break;
    case OP(OCERZ_OP_PACKUSWB):
        for (int i = 0; i < 8; i++) r.u8[i] = sat_u8(a.i16[i]);
        for (int i = 0; i < 8; i++) r.u8[i + 8] = sat_u8(b.i16[i]);
        break;
    case OP(OCERZ_OP_PACKUSDW):
        for (int i = 0; i < 4; i++) r.u16[i] = sat_u16(a.i32[i]);
        for (int i = 0; i < 4; i++) r.u16[i + 4] = sat_u16(b.i32[i]);
        break;
    case OP(OCERZ_OP_PUNPCKLBW):
        for (int i = 0; i < 8; i++) { r.u8[2 * i] = a.u8[i]; r.u8[2 * i + 1] = b.u8[i]; }
        break;
    case OP(OCERZ_OP_PUNPCKHBW):
        for (int i = 0; i < 8; i++) { r.u8[2 * i] = a.u8[i + 8]; r.u8[2 * i + 1] = b.u8[i + 8]; }
        break;
    case OP(OCERZ_OP_PUNPCKLWD):
        for (int i = 0; i < 4; i++) { r.u16[2 * i] = a.u16[i]; r.u16[2 * i + 1] = b.u16[i]; }
        break;
    case OP(OCERZ_OP_PUNPCKHWD):
        for (int i = 0; i < 4; i++) { r.u16[2 * i] = a.u16[i + 4]; r.u16[2 * i + 1] = b.u16[i + 4]; }
        break;
    case OP(OCERZ_OP_PUNPCKLDQ):
        r.u32[0] = a.u32[0]; r.u32[1] = b.u32[0]; r.u32[2] = a.u32[1]; r.u32[3] = b.u32[1];
        break;
    case OP(OCERZ_OP_PUNPCKHDQ):
        r.u32[0] = a.u32[2]; r.u32[1] = b.u32[2]; r.u32[2] = a.u32[3]; r.u32[3] = b.u32[3];
        break;
    case OP(OCERZ_OP_PUNPCKLQDQ):
        r.u64[0] = a.u64[0]; r.u64[1] = b.u64[0];
        break;
    case OP(OCERZ_OP_PUNPCKHQDQ):
        r.u64[0] = a.u64[1]; r.u64[1] = b.u64[1];
        break;
    case OP(OCERZ_OP_UNPCKLPS):
        r.u32[0] = a.u32[0]; r.u32[1] = b.u32[0]; r.u32[2] = a.u32[1]; r.u32[3] = b.u32[1];
        break;
    case OP(OCERZ_OP_UNPCKHPS):
        r.u32[0] = a.u32[2]; r.u32[1] = b.u32[2]; r.u32[2] = a.u32[3]; r.u32[3] = b.u32[3];
        break;
    case OP(OCERZ_OP_UNPCKLPD):
        r.u64[0] = a.u64[0]; r.u64[1] = b.u64[0];
        break;
    case OP(OCERZ_OP_UNPCKHPD):
        r.u64[0] = a.u64[1]; r.u64[1] = b.u64[1];
        break;
    case OP(OCERZ_OP_PSHUFD): {
        int imm = (int)insn->ops[2].imm;
        for (int i = 0; i < 4; i++) r.u32[i] = b.u32[(imm >> (2 * i)) & 3];
        break;
    }
    case OP(OCERZ_OP_PSHUFLW): {
        int imm = (int)insn->ops[2].imm;
        for (int i = 0; i < 4; i++) r.u16[i] = b.u16[(imm >> (2 * i)) & 3];
        r.u64[1] = b.u64[1];
        break;
    }
    case OP(OCERZ_OP_PSHUFHW): {
        int imm = (int)insn->ops[2].imm;
        r.u64[0] = b.u64[0];
        for (int i = 0; i < 4; i++) r.u16[i + 4] = b.u16[4 + ((imm >> (2 * i)) & 3)];
        break;
    }
    case OP(OCERZ_OP_PSHUFB): {
        vec old = a;
        for (int i = 0; i < 16; i++) {
            uint8_t ctl = b.u8[i];
            r.u8[i] = (ctl & 0x80) ? 0 : old.u8[ctl & 0x0f];
        }
        break;
    }
    case OP(OCERZ_OP_PALIGNR): {
        int imm = (int)(insn->ops[2].imm & 0xff);
        uint8_t cat[32];
        for (int i = 0; i < 16; i++) cat[i] = b.u8[i];
        for (int i = 0; i < 16; i++) cat[i + 16] = a.u8[i];
        for (int i = 0; i < 16; i++) {
            int idx = imm + i;
            r.u8[i] = (idx < 32) ? cat[idx] : 0;
        }
        break;
    }
    case OP(OCERZ_OP_SHUFPS): {
        int imm = (int)insn->ops[2].imm;
        r.u32[0] = a.u32[(imm >> 0) & 3];
        r.u32[1] = a.u32[(imm >> 2) & 3];
        r.u32[2] = b.u32[(imm >> 4) & 3];
        r.u32[3] = b.u32[(imm >> 6) & 3];
        break;
    }
    case OP(OCERZ_OP_SHUFPD): {
        int imm = (int)insn->ops[2].imm;
        r.u64[0] = a.u64[(imm >> 0) & 1];
        r.u64[1] = b.u64[(imm >> 1) & 1];
        break;
    }
    default:
        return OCERZ_EUNSUP;
    }
    cpu->xmm[d->reg] = r.q;
    return OCERZ_STEP_OK;
}

static unsigned shift_count(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *s = &insn->ops[1];
    if (s->kind == OCERZ_OPK_IMM)
        return (unsigned)(s->imm & 0xff);
    vec b = vec_read(cpu, insn, s);
    return b.u64[0] > 255 ? 256 : (unsigned)b.u64[0];
}

static int do_shift(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    vec a = vec_of(cpu->xmm[d->reg]);
    vec r = a;
    unsigned c = shift_count(cpu, insn);
    switch (insn->op) {
    case OP(OCERZ_OP_PSLLW):
        for (int i = 0; i < 8; i++) r.u16[i] = c < 16 ? (uint16_t)(a.u16[i] << c) : 0;
        break;
    case OP(OCERZ_OP_PSLLD):
        for (int i = 0; i < 4; i++) r.u32[i] = c < 32 ? (a.u32[i] << c) : 0;
        break;
    case OP(OCERZ_OP_PSLLQ):
        for (int i = 0; i < 2; i++) r.u64[i] = c < 64 ? (a.u64[i] << c) : 0;
        break;
    case OP(OCERZ_OP_PSRLW):
        for (int i = 0; i < 8; i++) r.u16[i] = c < 16 ? (uint16_t)(a.u16[i] >> c) : 0;
        break;
    case OP(OCERZ_OP_PSRLD):
        for (int i = 0; i < 4; i++) r.u32[i] = c < 32 ? (a.u32[i] >> c) : 0;
        break;
    case OP(OCERZ_OP_PSRLQ):
        for (int i = 0; i < 2; i++) r.u64[i] = c < 64 ? (a.u64[i] >> c) : 0;
        break;
    case OP(OCERZ_OP_PSRAW):
        for (int i = 0; i < 8; i++) {
            unsigned sh = c < 16 ? c : 15;
            r.i16[i] = (int16_t)(a.i16[i] >> sh);
        }
        break;
    case OP(OCERZ_OP_PSRAD):
        for (int i = 0; i < 4; i++) {
            unsigned sh = c < 32 ? c : 31;
            r.i32[i] = a.i32[i] >> sh;
        }
        break;
    case OP(OCERZ_OP_PSLLDQ): {
        uint8_t tmp[16];
        for (int i = 0; i < 16; i++) tmp[i] = (i >= (int)c && c <= 16) ? a.u8[i - c] : 0;
        for (int i = 0; i < 16; i++) r.u8[i] = tmp[i];
        break;
    }
    case OP(OCERZ_OP_PSRLDQ): {
        uint8_t tmp[16];
        for (int i = 0; i < 16; i++) tmp[i] = (i + (int)c < 16 && c <= 16) ? a.u8[i + c] : 0;
        for (int i = 0; i < 16; i++) r.u8[i] = tmp[i];
        break;
    }
    default:
        return OCERZ_EUNSUP;
    }
    cpu->xmm[d->reg] = r.q;
    return OCERZ_STEP_OK;
}

static int do_insert_extract(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    switch (insn->op) {
    case OP(OCERZ_OP_PEXTRB): {
        int idx = (int)(insn->ops[2].imm & 0x0f);
        vec v = vec_of(cpu->xmm[s->reg]);
        uint8_t e = v.u8[idx];
        if (d->kind == OCERZ_OPK_MEM)
            ocerz_st(ocerz_ea(cpu, insn, d), 1, e);
        else
            ocerz_write_gpr(cpu, d->reg, 4, 0, e);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PEXTRW): {
        int idx = (int)(insn->ops[2].imm & 0x07);
        vec v = vec_of(cpu->xmm[s->reg]);
        uint16_t e = v.u16[idx];
        if (d->kind == OCERZ_OPK_MEM)
            ocerz_st(ocerz_ea(cpu, insn, d), 2, e);
        else
            ocerz_write_gpr(cpu, d->reg, 4, 0, e);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PEXTRD): {
        int idx = (int)(insn->ops[2].imm & 0x03);
        vec v = vec_of(cpu->xmm[s->reg]);
        uint32_t e = v.u32[idx];
        if (d->kind == OCERZ_OPK_MEM)
            ocerz_st(ocerz_ea(cpu, insn, d), 4, e);
        else
            ocerz_write_gpr(cpu, d->reg, 4, 0, e);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PEXTRQ): {
        int idx = (int)(insn->ops[2].imm & 0x01);
        vec v = vec_of(cpu->xmm[s->reg]);
        uint64_t e = v.u64[idx];
        if (d->kind == OCERZ_OPK_MEM)
            ocerz_st(ocerz_ea(cpu, insn, d), 8, e);
        else
            ocerz_write_gpr(cpu, d->reg, 8, 0, e);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PINSRB): {
        int idx = (int)(insn->ops[2].imm & 0x0f);
        uint8_t e;
        if (s->kind == OCERZ_OPK_MEM)
            e = (uint8_t)ocerz_ld(ocerz_ea(cpu, insn, s), 1);
        else
            e = (uint8_t)ocerz_read_op(cpu, insn, s);
        vec v = vec_of(cpu->xmm[d->reg]);
        v.u8[idx] = e;
        cpu->xmm[d->reg] = v.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PINSRW): {
        int idx = (int)(insn->ops[2].imm & 0x07);
        uint16_t e;
        if (s->kind == OCERZ_OPK_MEM)
            e = (uint16_t)ocerz_ld(ocerz_ea(cpu, insn, s), 2);
        else
            e = (uint16_t)ocerz_read_op(cpu, insn, s);
        vec v = vec_of(cpu->xmm[d->reg]);
        v.u16[idx] = e;
        cpu->xmm[d->reg] = v.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PINSRD): {
        int idx = (int)(insn->ops[2].imm & 0x03);
        uint32_t e;
        if (s->kind == OCERZ_OPK_MEM)
            e = (uint32_t)ocerz_ld(ocerz_ea(cpu, insn, s), 4);
        else
            e = (uint32_t)ocerz_read_op(cpu, insn, s);
        vec v = vec_of(cpu->xmm[d->reg]);
        v.u32[idx] = e;
        cpu->xmm[d->reg] = v.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PINSRQ): {
        int idx = (int)(insn->ops[2].imm & 0x01);
        uint64_t e;
        if (s->kind == OCERZ_OPK_MEM)
            e = ocerz_ld(ocerz_ea(cpu, insn, s), 8);
        else
            e = ocerz_read_op(cpu, insn, s);
        vec v = vec_of(cpu->xmm[d->reg]);
        v.u64[idx] = e;
        cpu->xmm[d->reg] = v.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_EXTRACTPS): {
        int idx = (int)(insn->ops[2].imm & 0x03);
        vec v = vec_of(cpu->xmm[s->reg]);
        uint32_t e = v.u32[idx];
        if (d->kind == OCERZ_OPK_MEM)
            ocerz_st(ocerz_ea(cpu, insn, d), 4, e);
        else
            ocerz_write_gpr(cpu, d->reg, 4, 0, e);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_INSERTPS): {
        int imm = (int)(insn->ops[2].imm & 0xff);
        int count_s = (imm >> 6) & 3;
        int count_d = (imm >> 4) & 3;
        int zmask = imm & 0x0f;
        uint32_t sel;
        if (s->kind == OCERZ_OPK_MEM) {
            sel = (uint32_t)ocerz_ld(ocerz_ea(cpu, insn, s), 4);
        } else {
            vec sv = vec_of(cpu->xmm[s->reg]);
            sel = sv.u32[count_s];
        }
        vec v = vec_of(cpu->xmm[d->reg]);
        v.u32[count_d] = sel;
        for (int i = 0; i < 4; i++)
            if (zmask & (1 << i))
                v.u32[i] = 0;
        cpu->xmm[d->reg] = v.q;
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

static int do_ptest_blend(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    switch (insn->op) {
    case OP(OCERZ_OP_PTEST): {
        Ocerz128 a = cpu->xmm[d->reg];
        Ocerz128 b = ocerz_read_op128(cpu, insn, s);
        int zf = ((a.lo & b.lo) == 0 && (a.hi & b.hi) == 0);
        int cf = ((~a.lo & b.lo) == 0 && (~a.hi & b.hi) == 0);
        ocerz_flag_assign(cpu, OCERZ_ZF, zf);
        ocerz_flag_assign(cpu, OCERZ_CF, cf);
        ocerz_flag_assign(cpu, OCERZ_PF, 0);
        ocerz_flag_assign(cpu, OCERZ_AF, 0);
        ocerz_flag_assign(cpu, OCERZ_SF, 0);
        ocerz_flag_assign(cpu, OCERZ_OF, 0);
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PBLENDW): {
        int imm = (int)(insn->ops[2].imm & 0xff);
        vec a = vec_of(cpu->xmm[d->reg]);
        vec b = vec_read(cpu, insn, s);
        vec r = a;
        for (int i = 0; i < 8; i++)
            if (imm & (1 << i))
                r.u16[i] = b.u16[i];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_BLENDPS): {
        int imm = (int)(insn->ops[2].imm & 0x0f);
        vec a = vec_of(cpu->xmm[d->reg]);
        vec b = vec_read(cpu, insn, s);
        vec r = a;
        for (int i = 0; i < 4; i++)
            if (imm & (1 << i))
                r.u32[i] = b.u32[i];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_BLENDPD): {
        int imm = (int)(insn->ops[2].imm & 0x03);
        vec a = vec_of(cpu->xmm[d->reg]);
        vec b = vec_read(cpu, insn, s);
        vec r = a;
        for (int i = 0; i < 2; i++)
            if (imm & (1 << i))
                r.u64[i] = b.u64[i];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_PBLENDVB): {
        vec a = vec_of(cpu->xmm[d->reg]);
        vec b = vec_read(cpu, insn, s);
        vec m = vec_of(cpu->xmm[0]);
        vec r = a;
        for (int i = 0; i < 16; i++)
            if (m.u8[i] & 0x80)
                r.u8[i] = b.u8[i];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_BLENDVPS): {
        vec a = vec_of(cpu->xmm[d->reg]);
        vec b = vec_read(cpu, insn, s);
        vec m = vec_of(cpu->xmm[0]);
        vec r = a;
        for (int i = 0; i < 4; i++)
            if (m.u32[i] & 0x80000000u)
                r.u32[i] = b.u32[i];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_BLENDVPD): {
        vec a = vec_of(cpu->xmm[d->reg]);
        vec b = vec_read(cpu, insn, s);
        vec m = vec_of(cpu->xmm[0]);
        vec r = a;
        for (int i = 0; i < 2; i++)
            if (m.u64[i] & 0x8000000000000000ull)
                r.u64[i] = b.u64[i];
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

static int do_pmovx(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    int srcw;
    switch (insn->op) {
    case OP(OCERZ_OP_PMOVZXBQ): case OP(OCERZ_OP_PMOVSXBQ): srcw = 2; break;
    case OP(OCERZ_OP_PMOVZXBD): case OP(OCERZ_OP_PMOVSXBD):
    case OP(OCERZ_OP_PMOVZXWQ): case OP(OCERZ_OP_PMOVSXWQ): srcw = 4; break;
    default: srcw = 8; break;
    }
    vec b;
    if (s->kind == OCERZ_OPK_XMM) {
        b = vec_of(cpu->xmm[s->reg]);
    } else {
        b.q.lo = ocerz_ld(ocerz_ea(cpu, insn, s), srcw);
        b.q.hi = 0;
    }
    vec r;
    r.q.lo = 0;
    r.q.hi = 0;
    switch (insn->op) {
    case OP(OCERZ_OP_PMOVZXBW): for (int i = 0; i < 8; i++) r.u16[i] = b.u8[i]; break;
    case OP(OCERZ_OP_PMOVZXBD): for (int i = 0; i < 4; i++) r.u32[i] = b.u8[i]; break;
    case OP(OCERZ_OP_PMOVZXBQ): for (int i = 0; i < 2; i++) r.u64[i] = b.u8[i]; break;
    case OP(OCERZ_OP_PMOVZXWD): for (int i = 0; i < 4; i++) r.u32[i] = b.u16[i]; break;
    case OP(OCERZ_OP_PMOVZXWQ): for (int i = 0; i < 2; i++) r.u64[i] = b.u16[i]; break;
    case OP(OCERZ_OP_PMOVZXDQ): for (int i = 0; i < 2; i++) r.u64[i] = b.u32[i]; break;
    case OP(OCERZ_OP_PMOVSXBW): for (int i = 0; i < 8; i++) r.i16[i] = b.i8[i]; break;
    case OP(OCERZ_OP_PMOVSXBD): for (int i = 0; i < 4; i++) r.i32[i] = b.i8[i]; break;
    case OP(OCERZ_OP_PMOVSXBQ): for (int i = 0; i < 2; i++) r.i64[i] = b.i8[i]; break;
    case OP(OCERZ_OP_PMOVSXWD): for (int i = 0; i < 4; i++) r.i32[i] = b.i16[i]; break;
    case OP(OCERZ_OP_PMOVSXWQ): for (int i = 0; i < 2; i++) r.i64[i] = b.i16[i]; break;
    case OP(OCERZ_OP_PMOVSXDQ): for (int i = 0; i < 2; i++) r.i64[i] = b.i32[i]; break;
    default:
        return OCERZ_EUNSUP;
    }
    cpu->xmm[d->reg] = r.q;
    return OCERZ_STEP_OK;
}

static int do_round(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    const X86Operand *s = &insn->ops[1];
    int imm = (int)(insn->ops[2].imm & 0xff);
    vec b = vec_read(cpu, insn, s);
    vec r;
    switch (insn->op) {
    case OP(OCERZ_OP_ROUNDPS):
        r.q.lo = 0; r.q.hi = 0;
        for (int i = 0; i < 4; i++) r.f[i] = round_imm_f(b.f[i], imm);
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    case OP(OCERZ_OP_ROUNDPD):
        r.q.lo = 0; r.q.hi = 0;
        for (int i = 0; i < 2; i++) r.d[i] = round_imm_d(b.d[i], imm);
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    case OP(OCERZ_OP_ROUNDSS): {
        vec a = vec_of(cpu->xmm[d->reg]);
        a.f[0] = round_imm_f(b.f[0], imm);
        cpu->xmm[d->reg] = a.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_ROUNDSD): {
        vec a = vec_of(cpu->xmm[d->reg]);
        a.d[0] = round_imm_d(b.d[0], imm);
        cpu->xmm[d->reg] = a.q;
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t aes_isbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

static const uint8_t aes_shift_fwd[16] = { 0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11 };
static const uint8_t aes_shift_inv[16] = { 0,13,10,7,4,1,14,11,8,5,2,15,12,9,6,3 };

static uint8_t aes_xtime(uint8_t a)
{
    return (uint8_t)((a << 1) ^ ((a >> 7) * 0x1b));
}

static uint8_t aes_gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1)
            p ^= a;
        b = (uint8_t)(b >> 1);
        a = aes_xtime(a);
    }
    return p;
}

static vec aes_subbytes(vec s, const uint8_t *box)
{
    vec r;
    for (int i = 0; i < 16; i++)
        r.u8[i] = box[s.u8[i]];
    return r;
}

static vec aes_shiftrows(vec s, const uint8_t *perm)
{
    vec r;
    for (int i = 0; i < 16; i++)
        r.u8[i] = s.u8[perm[i]];
    return r;
}

static vec aes_mixcolumns(vec s)
{
    vec r;
    for (int c = 0; c < 4; c++) {
        const uint8_t *col = &s.u8[c * 4];
        r.u8[c * 4 + 0] = aes_gmul(col[0], 2) ^ aes_gmul(col[1], 3) ^ col[2] ^ col[3];
        r.u8[c * 4 + 1] = col[0] ^ aes_gmul(col[1], 2) ^ aes_gmul(col[2], 3) ^ col[3];
        r.u8[c * 4 + 2] = col[0] ^ col[1] ^ aes_gmul(col[2], 2) ^ aes_gmul(col[3], 3);
        r.u8[c * 4 + 3] = aes_gmul(col[0], 3) ^ col[1] ^ col[2] ^ aes_gmul(col[3], 2);
    }
    return r;
}

static vec aes_invmixcolumns(vec s)
{
    vec r;
    for (int c = 0; c < 4; c++) {
        const uint8_t *col = &s.u8[c * 4];
        r.u8[c * 4 + 0] = aes_gmul(col[0], 14) ^ aes_gmul(col[1], 11) ^ aes_gmul(col[2], 13) ^ aes_gmul(col[3], 9);
        r.u8[c * 4 + 1] = aes_gmul(col[0], 9) ^ aes_gmul(col[1], 14) ^ aes_gmul(col[2], 11) ^ aes_gmul(col[3], 13);
        r.u8[c * 4 + 2] = aes_gmul(col[0], 13) ^ aes_gmul(col[1], 9) ^ aes_gmul(col[2], 14) ^ aes_gmul(col[3], 11);
        r.u8[c * 4 + 3] = aes_gmul(col[0], 11) ^ aes_gmul(col[1], 13) ^ aes_gmul(col[2], 9) ^ aes_gmul(col[3], 14);
    }
    return r;
}

static uint32_t aes_subword(uint32_t w)
{
    return (uint32_t)aes_sbox[w & 0xff]
         | ((uint32_t)aes_sbox[(w >> 8) & 0xff] << 8)
         | ((uint32_t)aes_sbox[(w >> 16) & 0xff] << 16)
         | ((uint32_t)aes_sbox[(w >> 24) & 0xff] << 24);
}

static uint32_t aes_rotr8(uint32_t w)
{
    return (w >> 8) | (w << 24);
}

static int do_aes(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    vec a = vec_of(cpu->xmm[d->reg]);
    switch (insn->op) {
    case OP(OCERZ_OP_AESENC):
    case OP(OCERZ_OP_AESENCLAST): {
        vec k = vec_read(cpu, insn, &insn->ops[1]);
        vec t = aes_subbytes(aes_shiftrows(a, aes_shift_fwd), aes_sbox);
        if (insn->op == OP(OCERZ_OP_AESENC))
            t = aes_mixcolumns(t);
        for (int i = 0; i < 16; i++)
            t.u8[i] ^= k.u8[i];
        cpu->xmm[d->reg] = t.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_AESDEC):
    case OP(OCERZ_OP_AESDECLAST): {
        vec k = vec_read(cpu, insn, &insn->ops[1]);
        vec t = aes_subbytes(aes_shiftrows(a, aes_shift_inv), aes_isbox);
        if (insn->op == OP(OCERZ_OP_AESDEC))
            t = aes_invmixcolumns(t);
        for (int i = 0; i < 16; i++)
            t.u8[i] ^= k.u8[i];
        cpu->xmm[d->reg] = t.q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_AESIMC): {
        vec b = vec_read(cpu, insn, &insn->ops[1]);
        cpu->xmm[d->reg] = aes_invmixcolumns(b).q;
        return OCERZ_STEP_OK;
    }
    case OP(OCERZ_OP_AESKEYGENASSIST): {
        vec b = vec_read(cpu, insn, &insn->ops[1]);
        uint32_t rcon = (uint32_t)(insn->ops[2].imm & 0xff);
        uint32_t s1 = aes_subword(b.u32[1]);
        uint32_t s3 = aes_subword(b.u32[3]);
        vec r;
        r.u32[0] = s1;
        r.u32[1] = aes_rotr8(s1) ^ rcon;
        r.u32[2] = s3;
        r.u32[3] = aes_rotr8(s3) ^ rcon;
        cpu->xmm[d->reg] = r.q;
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

static int do_pclmul(OcerzCPU *cpu, const X86Insn *insn)
{
    const X86Operand *d = &insn->ops[0];
    vec a = vec_of(cpu->xmm[d->reg]);
    vec b = vec_read(cpu, insn, &insn->ops[1]);
    int imm = (int)(insn->ops[2].imm & 0xff);
    uint64_t x = (imm & 0x01) ? a.u64[1] : a.u64[0];
    uint64_t y = (imm & 0x10) ? b.u64[1] : b.u64[0];
    uint64_t lo = 0, hi = 0;
    for (int i = 0; i < 64; i++) {
        if ((y >> i) & 1) {
            lo ^= x << i;
            if (i)
                hi ^= x >> (64 - i);
        }
    }
    vec r;
    r.u64[0] = lo;
    r.u64[1] = hi;
    cpu->xmm[d->reg] = r.q;
    return OCERZ_STEP_OK;
}

int ocerz_interp_sse(struct OcerzVM *vm, OcerzCPU *cpu, const X86Insn *insn)
{
    (void)vm;
    switch (insn->op) {
    case OP(OCERZ_OP_MOVUPS):
    case OP(OCERZ_OP_MOVAPS):
    case OP(OCERZ_OP_MOVDQA):
    case OP(OCERZ_OP_MOVDQU):
    case OP(OCERZ_OP_MOVSS):
    case OP(OCERZ_OP_MOVSDX):
    case OP(OCERZ_OP_MOVD):
    case OP(OCERZ_OP_MOVQX):
    case OP(OCERZ_OP_MOVLPS):
    case OP(OCERZ_OP_MOVHPS):
    case OP(OCERZ_OP_MOVLHPS):
    case OP(OCERZ_OP_MOVHLPS):
    case OP(OCERZ_OP_MOVMSKPS):
    case OP(OCERZ_OP_MOVMSKPD):
    case OP(OCERZ_OP_PMOVMSKB):
    case OP(OCERZ_OP_MOVSHDUP):
    case OP(OCERZ_OP_MOVSLDUP):
    case OP(OCERZ_OP_MOVDDUP):
        return do_moves(cpu, insn);
    case OP(OCERZ_OP_MOVNTI):
        ocerz_write_op(cpu, insn, &insn->ops[0], ocerz_read_op(cpu, insn, &insn->ops[1]));
        return OCERZ_STEP_OK;

    case OP(OCERZ_OP_ADDPS): case OP(OCERZ_OP_ADDPD): case OP(OCERZ_OP_ADDSS): case OP(OCERZ_OP_ADDSD):
    case OP(OCERZ_OP_HADDPS): case OP(OCERZ_OP_HADDPD): case OP(OCERZ_OP_HSUBPS): case OP(OCERZ_OP_HSUBPD):
    case OP(OCERZ_OP_ADDSUBPS): case OP(OCERZ_OP_ADDSUBPD):
    case OP(OCERZ_OP_SUBPS): case OP(OCERZ_OP_SUBPD): case OP(OCERZ_OP_SUBSS): case OP(OCERZ_OP_SUBSD):
    case OP(OCERZ_OP_MULPS): case OP(OCERZ_OP_MULPD): case OP(OCERZ_OP_MULSS): case OP(OCERZ_OP_MULSD):
    case OP(OCERZ_OP_DIVPS): case OP(OCERZ_OP_DIVPD): case OP(OCERZ_OP_DIVSS): case OP(OCERZ_OP_DIVSD):
    case OP(OCERZ_OP_MINPS): case OP(OCERZ_OP_MINPD): case OP(OCERZ_OP_MINSS): case OP(OCERZ_OP_MINSD):
    case OP(OCERZ_OP_MAXPS): case OP(OCERZ_OP_MAXPD): case OP(OCERZ_OP_MAXSS): case OP(OCERZ_OP_MAXSD):
    case OP(OCERZ_OP_SQRTPS): case OP(OCERZ_OP_SQRTPD): case OP(OCERZ_OP_SQRTSS): case OP(OCERZ_OP_SQRTSD):
    case OP(OCERZ_OP_RSQRTPS): case OP(OCERZ_OP_RSQRTSS): case OP(OCERZ_OP_RCPPS): case OP(OCERZ_OP_RCPSS):
        return do_fp_arith(cpu, insn);

    case OP(OCERZ_OP_ANDPS): case OP(OCERZ_OP_ANDNPS): case OP(OCERZ_OP_ORPS): case OP(OCERZ_OP_XORPS):
    case OP(OCERZ_OP_PAND): case OP(OCERZ_OP_PANDN): case OP(OCERZ_OP_POR): case OP(OCERZ_OP_PXOR):
        return do_logic(cpu, insn);

    case OP(OCERZ_OP_CMPPS): case OP(OCERZ_OP_CMPPD): case OP(OCERZ_OP_CMPSS): case OP(OCERZ_OP_CMPSDX):
        return do_cmpfp(cpu, insn);
    case OP(OCERZ_OP_COMISS): case OP(OCERZ_OP_COMISD): case OP(OCERZ_OP_UCOMISS): case OP(OCERZ_OP_UCOMISD):
        return do_comis(cpu, insn);
    case OP(OCERZ_OP_PCMPEQB): case OP(OCERZ_OP_PCMPEQW): case OP(OCERZ_OP_PCMPEQD): case OP(OCERZ_OP_PCMPEQQ):
    case OP(OCERZ_OP_PCMPGTB): case OP(OCERZ_OP_PCMPGTW): case OP(OCERZ_OP_PCMPGTD): case OP(OCERZ_OP_PCMPGTQ):
        return do_pcmp(cpu, insn);

    case OP(OCERZ_OP_CVTSI2SS): case OP(OCERZ_OP_CVTSI2SD): case OP(OCERZ_OP_CVTSS2SI): case OP(OCERZ_OP_CVTSD2SI):
    case OP(OCERZ_OP_CVTTSS2SI): case OP(OCERZ_OP_CVTTSD2SI): case OP(OCERZ_OP_CVTSS2SD): case OP(OCERZ_OP_CVTSD2SS):
    case OP(OCERZ_OP_CVTPS2PD): case OP(OCERZ_OP_CVTPD2PS): case OP(OCERZ_OP_CVTDQ2PS): case OP(OCERZ_OP_CVTPS2DQ):
    case OP(OCERZ_OP_CVTTPS2DQ): case OP(OCERZ_OP_CVTDQ2PD): case OP(OCERZ_OP_CVTPD2DQ): case OP(OCERZ_OP_CVTTPD2DQ):
        return do_convert(cpu, insn);

    case OP(OCERZ_OP_PADDB): case OP(OCERZ_OP_PADDW): case OP(OCERZ_OP_PADDD): case OP(OCERZ_OP_PADDQ):
    case OP(OCERZ_OP_PSUBB): case OP(OCERZ_OP_PSUBW): case OP(OCERZ_OP_PSUBD): case OP(OCERZ_OP_PSUBQ):
    case OP(OCERZ_OP_PADDSB): case OP(OCERZ_OP_PADDSW): case OP(OCERZ_OP_PADDUSB): case OP(OCERZ_OP_PADDUSW):
    case OP(OCERZ_OP_PSUBSB): case OP(OCERZ_OP_PSUBSW): case OP(OCERZ_OP_PSUBUSB): case OP(OCERZ_OP_PSUBUSW):
    case OP(OCERZ_OP_PMULLW): case OP(OCERZ_OP_PMULLD): case OP(OCERZ_OP_PMULHW): case OP(OCERZ_OP_PMULHUW):
    case OP(OCERZ_OP_PMULUDQ): case OP(OCERZ_OP_PMADDWD): case OP(OCERZ_OP_PAVGB): case OP(OCERZ_OP_PAVGW):
    case OP(OCERZ_OP_PMAXUB): case OP(OCERZ_OP_PMAXSW): case OP(OCERZ_OP_PMINUB): case OP(OCERZ_OP_PMINSW):
    case OP(OCERZ_OP_PMAXSB): case OP(OCERZ_OP_PMAXSD): case OP(OCERZ_OP_PMAXUW): case OP(OCERZ_OP_PMAXUD):
    case OP(OCERZ_OP_PMINSB): case OP(OCERZ_OP_PMINSD): case OP(OCERZ_OP_PMINUW): case OP(OCERZ_OP_PMINUD):
    case OP(OCERZ_OP_PSADBW): case OP(OCERZ_OP_PABSB): case OP(OCERZ_OP_PABSW): case OP(OCERZ_OP_PABSD):
    case OP(OCERZ_OP_PHADDW): case OP(OCERZ_OP_PHADDD): case OP(OCERZ_OP_PHADDSW):
    case OP(OCERZ_OP_PHSUBW): case OP(OCERZ_OP_PHSUBD): case OP(OCERZ_OP_PHSUBSW):
    case OP(OCERZ_OP_PSIGNB): case OP(OCERZ_OP_PSIGNW): case OP(OCERZ_OP_PSIGND):
    case OP(OCERZ_OP_PMADDUBSW): case OP(OCERZ_OP_PMULHRSW):
        return do_int_arith(cpu, insn);

    case OP(OCERZ_OP_PACKSSWB): case OP(OCERZ_OP_PACKSSDW): case OP(OCERZ_OP_PACKUSWB): case OP(OCERZ_OP_PACKUSDW):
    case OP(OCERZ_OP_PUNPCKLBW): case OP(OCERZ_OP_PUNPCKLWD): case OP(OCERZ_OP_PUNPCKLDQ): case OP(OCERZ_OP_PUNPCKLQDQ):
    case OP(OCERZ_OP_PUNPCKHBW): case OP(OCERZ_OP_PUNPCKHWD): case OP(OCERZ_OP_PUNPCKHDQ): case OP(OCERZ_OP_PUNPCKHQDQ):
    case OP(OCERZ_OP_PSHUFD): case OP(OCERZ_OP_PSHUFLW): case OP(OCERZ_OP_PSHUFHW): case OP(OCERZ_OP_PSHUFB):
    case OP(OCERZ_OP_PALIGNR): case OP(OCERZ_OP_SHUFPS): case OP(OCERZ_OP_SHUFPD):
    case OP(OCERZ_OP_UNPCKLPS): case OP(OCERZ_OP_UNPCKHPS): case OP(OCERZ_OP_UNPCKLPD): case OP(OCERZ_OP_UNPCKHPD):
        return do_pack_shuffle(cpu, insn);

    case OP(OCERZ_OP_PSLLW): case OP(OCERZ_OP_PSLLD): case OP(OCERZ_OP_PSLLQ):
    case OP(OCERZ_OP_PSRLW): case OP(OCERZ_OP_PSRLD): case OP(OCERZ_OP_PSRLQ):
    case OP(OCERZ_OP_PSRAW): case OP(OCERZ_OP_PSRAD):
    case OP(OCERZ_OP_PSLLDQ): case OP(OCERZ_OP_PSRLDQ):
        return do_shift(cpu, insn);

    case OP(OCERZ_OP_PEXTRB): case OP(OCERZ_OP_PEXTRW): case OP(OCERZ_OP_PEXTRD): case OP(OCERZ_OP_PEXTRQ):
    case OP(OCERZ_OP_PINSRB): case OP(OCERZ_OP_PINSRW): case OP(OCERZ_OP_PINSRD): case OP(OCERZ_OP_PINSRQ):
    case OP(OCERZ_OP_EXTRACTPS): case OP(OCERZ_OP_INSERTPS):
        return do_insert_extract(cpu, insn);

    case OP(OCERZ_OP_PTEST): case OP(OCERZ_OP_PBLENDW): case OP(OCERZ_OP_BLENDPS): case OP(OCERZ_OP_BLENDPD):
    case OP(OCERZ_OP_PBLENDVB): case OP(OCERZ_OP_BLENDVPS): case OP(OCERZ_OP_BLENDVPD):
        return do_ptest_blend(cpu, insn);

    case OP(OCERZ_OP_PMOVZXBW): case OP(OCERZ_OP_PMOVZXBD): case OP(OCERZ_OP_PMOVZXBQ):
    case OP(OCERZ_OP_PMOVZXWD): case OP(OCERZ_OP_PMOVZXWQ): case OP(OCERZ_OP_PMOVZXDQ):
    case OP(OCERZ_OP_PMOVSXBW): case OP(OCERZ_OP_PMOVSXBD): case OP(OCERZ_OP_PMOVSXBQ):
    case OP(OCERZ_OP_PMOVSXWD): case OP(OCERZ_OP_PMOVSXWQ): case OP(OCERZ_OP_PMOVSXDQ):
        return do_pmovx(cpu, insn);

    case OP(OCERZ_OP_ROUNDPS): case OP(OCERZ_OP_ROUNDPD): case OP(OCERZ_OP_ROUNDSS): case OP(OCERZ_OP_ROUNDSD):
        return do_round(cpu, insn);

    case OP(OCERZ_OP_AESENC): case OP(OCERZ_OP_AESENCLAST):
    case OP(OCERZ_OP_AESDEC): case OP(OCERZ_OP_AESDECLAST):
    case OP(OCERZ_OP_AESIMC): case OP(OCERZ_OP_AESKEYGENASSIST):
        return do_aes(cpu, insn);

    case OP(OCERZ_OP_PCLMULQDQ):
        return do_pclmul(cpu, insn);

    default:
        return OCERZ_EUNSUP;
    }
}
