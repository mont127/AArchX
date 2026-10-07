/*
 * The x87 unit, interpreted.
 *
 * ---- registers ----
 * The eight data registers are IEEE doubles in OcerzCPU.fpr, indexed by
 * physical register; ST(i) is fpr[(ftop + i) & 7].  The real registers are
 * 80-bit extended, and so are m80 operands and the FNSAVE and FXSAVE images,
 * but a double keeps x87 arithmetic on the host's floating-point unit, and
 * rosettax87_jit, which 32-bit games ran on before, makes the same choice.
 * What a double cannot hold is a value loaded from memory as more than a
 * double: a 64-bit integer, which is not a matter of precision at all -
 * Delphi's and Free Pascal's Move() copy eight bytes at a time as fild qword /
 * fistp qword, so a rounded integer is corrupted memory (OMSI 2 died reading
 * its own form resource) - and an 80-bit value moved with fld tbyte / fstp
 * tbyte, which is how compilers copy a long double.  Each register therefore
 * also has an exact 80-bit image in fpr_xm and fpr_xe, which counts while its
 * bit in fpr_x_ok is set AND the double is still, bit for bit, the image
 * rounded to double.  FILD, FBLD, the constant loads, an m80 load and the
 * FRSTOR and FXRSTOR images fill it; FLD ST(i), FST ST(i), FXCH and FCMOVcc
 * carry it; every other write clears the bit.  Integer stores, FBSTP, the
 * 80-bit images, FXAM and the tag word read it, so an integer, a long double, a
 * denormal or a NaN payload goes through loads, stores and context switches
 * unchanged.  The second condition keeps the image safe from a writer outside
 * this file that forgets the bit.
 *
 * ftw is the abridged tag word, bit p set while physical register p holds a
 * value, as FXSAVE stores it; FNSTENV and FNSAVE expand it to the two-bit form
 * from the register contents.  Pushes, pops, FFREE and FXCH keep it, which
 * FXAM's "empty" class needs.  A push onto a full register or a read of an
 * empty one is not a stack fault: the value passes through, since a tag that
 * went wrong somewhere outside the unit must never turn data into NaNs.  TOP is
 * ftop; the TOP field of fsw is ignored and synthesized whenever fsw is read.
 *
 * ---- arithmetic ----
 * Each operation runs on the host's floating-point unit with FPCR set from the
 * control word - rounding from RC, flush-to-zero off - whatever MXCSR asked of
 * SSE, and FPSR is saved around it, so the flags it raises go to fsw only and
 * x87 arithmetic never shows in MXCSR, as on x86.  Precision control 24, which
 * Direct3D 9 sets, rounds FADD, FSUB, FMUL, FDIV and FSQRT results to a
 * 24-bit significand without narrowing the exponent; 53 and 64 both leave the
 * double.  NaNs follow the x87 rules rather than arm64's or SSE's: an invalid
 * operation with no NaN operand gives the negative default NaN, a single NaN
 * operand comes back quieted, and of two NaNs a QNaN beats an SNaN and
 * otherwise the larger significand wins, the positive one on a tie, as Rosetta
 * does.  FCHS and FABS change the sign bit of
 * anything, NaNs included.  Unmasked exceptions do not trap, C1 does not say
 * which way a result was rounded, and the instruction and operand pointers in
 * the environment images are zero.
 *
 * ---- the rest ----
 * FIST, FISTP and FISTTP store the integer indefinite (0x8000, 0x80000000 or
 * 0x8000000000000000) for a NaN or a value out of range, with IE.  FPREM and
 * FPREM1 reduce in one step when the exponents differ by less than 64, with
 * C2 clear and the low three quotient bits in C0, C3 and C1, and otherwise
 * take a partial step and set C2, as the hardware does; the partial remainder
 * is fmod against the divisor scaled by a power of two, which is exact.  FSIN,
 * FCOS, FPTAN and FSINCOS leave an operand of magnitude 2^63 or more alone and
 * set C2.  The control word's bit 6 always reads as one.  Where Rosetta and the
 * SDM part, Rosetta wins, since it is what these programs ran on: FNSTENV
 * leaves the exception masks alone, and the pad words of the environment
 * images are zero.
 */
#include "ocerz/x87.h"
#include "ocerz/interp.h"
#include "ocerz/interp_common.h"

#include <arm_acle.h>
#include <math.h>
#include <string.h>

#pragma STDC FENV_ACCESS ON

#define X87_IE 0x0001u
#define X87_ZE 0x0004u
#define X87_OE 0x0008u
#define X87_UE 0x0010u
#define X87_PE 0x0020u
#define X87_FLAGS (X87_IE | X87_ZE | X87_OE | X87_UE | X87_PE)
#define X87_C0 0x0100u
#define X87_C1 0x0200u
#define X87_C2 0x0400u
#define X87_C3 0x4000u
#define X87_CC (X87_C0 | X87_C1 | X87_C2 | X87_C3)

#define DBL_SIGN 0x8000000000000000ull
#define DBL_EXP 0x7ff0000000000000ull
#define DBL_FRAC 0x000fffffffffffffull
#define DBL_QUIET 0x0008000000000000ull
#define X87_DEFAULT_NAN 0xfff8000000000000ull

/* ---- bits ---- */

static inline uint64_t dbits(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static inline double bitsd(uint64_t u)
{
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static inline int is_nan_bits(uint64_t u)
{
    return (u & DBL_EXP) == DBL_EXP && (u & DBL_FRAC);
}

static inline int is_snan(double d)
{
    uint64_t u = dbits(d);
    return is_nan_bits(u) && !(u & DBL_QUIET);
}

static inline double quieted(double d)
{
    return bitsd(dbits(d) | DBL_QUIET);
}

static double nan1(double a)
{
    return isnan(a) ? quieted(a) : bitsd(X87_DEFAULT_NAN);
}

static double nan2(double a, double b)
{
    int an = isnan(a), bn = isnan(b);
    if (an && bn) {
        int as = is_snan(a), bs = is_snan(b);
        if (as != bs)
            return quieted(as ? b : a);
        uint64_t fa = dbits(a) & DBL_FRAC, fb = dbits(b) & DBL_FRAC;
        if (fa == fb)
            return quieted(signbit(a) ? b : a);
        return quieted(fb > fa ? b : a);
    }
    if (an)
        return quieted(a);
    if (bn)
        return quieted(b);
    return bitsd(X87_DEFAULT_NAN);
}

/* ---- the host's floating-point environment around one operation ---- */

typedef struct {
    uint64_t fpcr, fpsr, want;
} X87Env;

#define FPCR_FIZ 0x1ull
#define FPCR_RMODE (3ull << 22)
#define FPCR_FZ (1ull << 24)
#define FPCR_DN (1ull << 25)

static inline X87Env env_enter(const OcerzCPU *cpu)
{
    static const uint64_t rmode[4] = { 0, 2ull << 22, 1ull << 22, 3ull << 22 };
    X87Env e;
    e.fpcr = __arm_rsr64("fpcr");
    e.fpsr = __arm_rsr64("fpsr");
    e.want = (e.fpcr & ~(FPCR_RMODE | FPCR_FZ | FPCR_DN | FPCR_FIZ)) | rmode[(cpu->fcw >> 10) & 3];
    if (e.want != e.fpcr)
        __arm_wsr64("fpcr", e.want);
    __arm_wsr64("fpsr", 0);
    return e;
}

static inline void env_leave(OcerzCPU *cpu, X87Env e, unsigned keep)
{
    uint64_t f = __arm_rsr64("fpsr");
    unsigned x = 0;
    if (f & 0x01) x |= X87_IE;
    if (f & 0x02) x |= X87_ZE;
    if (f & 0x04) x |= X87_OE;
    if (f & 0x08) x |= X87_UE;
    if (f & 0x10) x |= X87_PE;
    cpu->fsw |= (uint16_t)(x & keep);
    __arm_wsr64("fpsr", e.fpsr);
    if (e.want != e.fpcr)
        __arm_wsr64("fpcr", e.fpcr);
}

/* Precision control 24: round the significand only; the exponent range stays. */
static double pc_round(const OcerzCPU *cpu, double r)
{
    if (((cpu->fcw >> 8) & 3) != 0 || r == 0.0 || !isfinite(r))
        return r;
    int e = ilogb(r);
    return ldexp((double)(float)ldexp(r, -e), e);
}

/* ---- 80-bit images ---- */

/* The double nearest an 80-bit value, ties to even, computed without the FPU. */
static uint64_t f80_to_dbits(uint64_t mant, unsigned se)
{
    uint64_t sign = (uint64_t)(se >> 15) << 63;
    int e = (int)(se & 0x7fff);
    if (e == 0x7fff) {
        if ((mant << 1) == 0)
            return sign | DBL_EXP;
        uint64_t f = (mant >> 11) & DBL_FRAC;
        return sign | DBL_EXP | (f ? f : 1);
    }
    if (mant == 0)
        return sign;
    int lz = __builtin_clzll(mant);
    mant <<= lz;
    int be = (e ? e : 1) - 16383 - lz + 1023;
    if (be >= 0x7ff)
        return sign | DBL_EXP;
    int shift = 11;
    if (be <= 0) {
        shift += 1 - be;
        be = 0;
    }
    uint64_t keep, rest;
    if (shift >= 65) {
        keep = 0;
        rest = 1;
    } else if (shift == 64) {
        keep = 0;
        rest = mant;
    } else {
        keep = mant >> shift;
        rest = mant << (64 - shift);
    }
    if (rest > (1ull << 63) || (rest == (1ull << 63) && (keep & 1)))
        keep++;
    uint64_t bits = be ? ((uint64_t)(be - 1) << 52) + keep : keep;
    if ((bits & DBL_EXP) == DBL_EXP)
        bits = DBL_EXP;
    return sign | bits;
}

static void int_to_f80(int64_t x, uint64_t *mant, uint16_t *se)
{
    if (x == 0) {
        *mant = 0;
        *se = 0;
        return;
    }
    uint64_t m = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
    int lz = __builtin_clzll(m);
    *mant = m << lz;
    *se = (uint16_t)((x < 0 ? 0x8000u : 0) | (unsigned)(16383 + 63 - lz));
}

/*
 * The integer an 80-bit value rounds to under rc (0 nearest, 1 down, 2 up, 3
 * toward zero), or 0 when it is no finite number or rounds past 64 bits.
 */
static int f80_to_int(uint64_t mant, unsigned se, int rc, int64_t *out, int *inexact)
{
    int neg = (se >> 15) & 1, e = (int)(se & 0x7fff);
    *inexact = 0;
    if (e == 0x7fff)
        return 0;
    if (mant == 0) {
        *out = 0;
        return 1;
    }
    int shift = 16383 + 63 - (e ? e : 1);
    uint64_t ip, frac;
    if (shift < 0)
        return 0;
    if (shift == 0) {
        ip = mant;
        frac = 0;
    } else if (shift < 64) {
        ip = mant >> shift;
        frac = mant << (64 - shift);
    } else {
        ip = 0;
        frac = shift == 64 ? mant : 1;
    }
    *inexact = frac != 0;
    int up = 0;
    if (frac) {
        switch (rc) {
        case 0: up = frac > (1ull << 63) || (frac == (1ull << 63) && (ip & 1)); break;
        case 1: up = neg; break;
        case 2: up = !neg; break;
        default: break;
        }
    }
    if (up && ++ip == 0)
        return 0;
    if (neg) {
        if (ip > (1ull << 63))
            return 0;
        *out = (int64_t)((uint64_t)0 - ip);
    } else {
        if (ip > (uint64_t)INT64_MAX)
            return 0;
        *out = (int64_t)ip;
    }
    return 1;
}

/* FXAM's class bits for an image: C0 NaN, C2 normal, C2|C0 inf, C3 zero, C3|C2 denormal. */
static unsigned f80_class(uint64_t mant, unsigned se)
{
    unsigned e = se & 0x7fff;
    if (e == 0)
        return mant ? (X87_C3 | X87_C2) : X87_C3;
    if (!(mant >> 63))
        return 0;
    if (e == 0x7fff)
        return (mant << 1) ? X87_C0 : (X87_C2 | X87_C0);
    return X87_C2;
}

/* ---- the register stack ---- */

static inline int phys(const OcerzCPU *cpu, int i)
{
    return (cpu->ftop + i) & 7;
}

static inline double st(const OcerzCPU *cpu, int i)
{
    return cpu->fpr[phys(cpu, i)];
}

static inline void set_st(OcerzCPU *cpu, int i, double v)
{
    int p = phys(cpu, i);
    cpu->fpr[p] = v;
    cpu->fpr_x_ok &= (uint8_t)~(1u << p);
    cpu->ftw |= (uint8_t)(1u << p);
}

static inline int image_ok(const OcerzCPU *cpu, int p)
{
    return ((cpu->fpr_x_ok >> p) & 1) && dbits(cpu->fpr[p]) == f80_to_dbits(cpu->fpr_xm[p], cpu->fpr_xe[p]);
}

static inline void set_phys_image(OcerzCPU *cpu, int p, uint64_t mant, uint16_t se)
{
    cpu->fpr[p] = bitsd(f80_to_dbits(mant, se));
    cpu->fpr_xm[p] = mant;
    cpu->fpr_xe[p] = se;
    cpu->fpr_x_ok |= (uint8_t)(1u << p);
}

static inline void push_slot(OcerzCPU *cpu)
{
    cpu->ftop = (cpu->ftop - 1) & 7;
    cpu->ftw |= (uint8_t)(1u << cpu->ftop);
    cpu->fpr_x_ok &= (uint8_t)~(1u << cpu->ftop);
}

static inline void push_d(OcerzCPU *cpu, double v)
{
    push_slot(cpu);
    cpu->fpr[cpu->ftop] = v;
}

static inline void push_image(OcerzCPU *cpu, uint64_t mant, uint16_t se)
{
    push_slot(cpu);
    set_phys_image(cpu, cpu->ftop, mant, se);
}

static inline void push_i(OcerzCPU *cpu, int64_t x)
{
    uint64_t mant;
    uint16_t se;
    int_to_f80(x, &mant, &se);
    push_image(cpu, mant, se);
}

static inline void pop(OcerzCPU *cpu)
{
    cpu->ftw &= (uint8_t)~(1u << cpu->ftop);
    cpu->ftop = (cpu->ftop + 1) & 7;
}

static void copy_st(OcerzCPU *cpu, int dst, int src)
{
    int d = phys(cpu, dst), s = phys(cpu, src);
    if (d == s)
        return;
    cpu->fpr[d] = cpu->fpr[s];
    cpu->fpr_xm[d] = cpu->fpr_xm[s];
    cpu->fpr_xe[d] = cpu->fpr_xe[s];
    cpu->fpr_x_ok = (uint8_t)((cpu->fpr_x_ok & ~(1u << d)) | (((cpu->fpr_x_ok >> s) & 1u) << d));
    cpu->ftw |= (uint8_t)(1u << d);
}

static void exchange(OcerzCPU *cpu, int i)
{
    int a = phys(cpu, 0), b = phys(cpu, i);
    if (a == b)
        return;
    double t = cpu->fpr[a];
    cpu->fpr[a] = cpu->fpr[b];
    cpu->fpr[b] = t;
    uint64_t tm = cpu->fpr_xm[a];
    cpu->fpr_xm[a] = cpu->fpr_xm[b];
    cpu->fpr_xm[b] = tm;
    uint16_t te = cpu->fpr_xe[a];
    cpu->fpr_xe[a] = cpu->fpr_xe[b];
    cpu->fpr_xe[b] = te;
    unsigned oa = (cpu->fpr_x_ok >> a) & 1, ob = (cpu->fpr_x_ok >> b) & 1;
    cpu->fpr_x_ok = (uint8_t)((cpu->fpr_x_ok & ~((1u << a) | (1u << b))) | (ob << a) | (oa << b));
    cpu->ftw |= (uint8_t)((1u << a) | (1u << b));
}

uint16_t ocerz_x87_fsw(const OcerzCPU *cpu)
{
    return (uint16_t)((cpu->fsw & ~(7u << 11)) | ((cpu->ftop & 7u) << 11));
}

static inline void set_cc(OcerzCPU *cpu, unsigned cc)
{
    cpu->fsw = (uint16_t)((cpu->fsw & ~X87_CC) | cc);
}

void ocerz_x87_push(OcerzCPU *cpu, double v)
{
    push_d(cpu, v);
}

void ocerz_x87_reset(OcerzCPU *cpu)
{
    cpu->fcw = 0x037f;
    cpu->fsw = 0;
    cpu->ftw = 0;
    cpu->ftop = 0;
}

void ocerz_x87_to_f80(const OcerzCPU *cpu, int p, uint8_t out[10])
{
    uint64_t mant;
    unsigned se;
    if (image_ok(cpu, p)) {
        mant = cpu->fpr_xm[p];
        se = cpu->fpr_xe[p];
    } else {
        uint64_t u = dbits(cpu->fpr[p]);
        unsigned sign = (unsigned)(u >> 63) << 15;
        unsigned e = (unsigned)((u >> 52) & 0x7ff);
        uint64_t f = u & DBL_FRAC;
        if (e == 0x7ff) {
            mant = (1ull << 63) | (f << 11);
            se = sign | 0x7fff;
        } else if (e == 0) {
            if (f == 0) {
                mant = 0;
                se = sign;
            } else {
                int lz = __builtin_clzll(f);
                mant = f << lz;
                se = sign | (unsigned)(15372 - lz);
            }
        } else {
            mant = (1ull << 63) | (f << 11);
            se = sign | (e - 1023 + 16383);
        }
    }
    memcpy(out, &mant, 8);
    out[8] = (uint8_t)se;
    out[9] = (uint8_t)(se >> 8);
}

void ocerz_x87_from_f80(OcerzCPU *cpu, int p, const uint8_t in[10])
{
    uint64_t mant;
    memcpy(&mant, in, 8);
    set_phys_image(cpu, p, mant, (uint16_t)(in[8] | (in[9] << 8)));
}

/* ---- memory operands ---- */

static double load_real(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    uint64_t ea = ocerz_ea(cpu, insn, op);
    if (op->size == 4) {
        uint32_t bits = (uint32_t)ocerz_ld(ea, 4);
        if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) && !(bits & 0x00400000u))
            cpu->fsw |= X87_IE;
        float f;
        memcpy(&f, &bits, 4);
        double d = (double)f;
        return isnan(d) ? quieted(d) : d;
    }
    uint64_t bits = ocerz_ld(ea, 8);
    if (is_nan_bits(bits) && !(bits & DBL_QUIET)) {
        cpu->fsw |= X87_IE;
        bits |= DBL_QUIET;
    }
    return bitsd(bits);
}

static int64_t load_int(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    return ocerz_sext(ocerz_ld(ocerz_ea(cpu, insn, op), op->size), op->size);
}

static void store_real(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op)
{
    uint64_t ea = ocerz_ea(cpu, insn, op);
    int p = phys(cpu, 0);
    double v = cpu->fpr[p];
    if (op->size == 10) {
        uint8_t buf[10];
        ocerz_x87_to_f80(cpu, p, buf);
        memcpy(ocerz_g2h(ea), buf, 10);
        return;
    }
    if (is_snan(v)) {
        cpu->fsw |= X87_IE;
        v = quieted(v);
    }
    if (op->size == 8) {
        ocerz_st(ea, 8, dbits(v));
        return;
    }
    X87Env e = env_enter(cpu);
    volatile float f = (float)v;
    env_leave(cpu, e, X87_FLAGS);
    float g = f;
    uint32_t bits;
    memcpy(&bits, &g, 4);
    ocerz_st(ea, 4, bits);
}

static double round_rc(OcerzCPU *cpu, double v)
{
    X87Env e = env_enter(cpu);
    volatile double r = nearbyint(v);
    env_leave(cpu, e, 0);
    return r;
}

/* ST(0) as an integer under rc, from its image when it has one; 0 for no finite number. */
static int st0_to_int(OcerzCPU *cpu, int rc, int64_t *out, int *inexact)
{
    int p = phys(cpu, 0);
    if (image_ok(cpu, p))
        return f80_to_int(cpu->fpr_xm[p], cpu->fpr_xe[p], rc, out, inexact);
    double v = cpu->fpr[p];
    *inexact = 0;
    if (isnan(v) || isinf(v))
        return 0;
    double r = rc == 3 ? trunc(v) : round_rc(cpu, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return 0;
    *inexact = r != v;
    *out = (int64_t)r;
    return 1;
}

static void store_int(OcerzCPU *cpu, const X86Insn *insn, const X86Operand *op, int truncate)
{
    int size = op->size;
    uint64_t ea = ocerz_ea(cpu, insn, op);
    int64_t x = 0;
    int inexact = 0;
    int ok = st0_to_int(cpu, truncate ? 3 : (cpu->fcw >> 10) & 3, &x, &inexact);
    if (ok && size < 8) {
        int64_t lim = (int64_t)1 << (size * 8 - 1);
        ok = x >= -lim && x < lim;
    }
    if (!ok) {
        x = (int64_t)(1ull << (size * 8 - 1));
        cpu->fsw |= X87_IE;
    } else if (inexact) {
        cpu->fsw |= X87_PE;
    }
    ocerz_st(ea, size, (uint64_t)x);
}

/* ---- packed BCD ---- */

static void fbld(OcerzCPU *cpu, uint64_t ea)
{
    uint8_t b[10];
    memcpy(b, ocerz_g2h(ea), 10);
    int64_t x = 0;
    for (int i = 8; i >= 0; i--)
        x = x * 100 + (b[i] >> 4) * 10 + (b[i] & 15);
    if (b[9] & 0x80) {
        if (x == 0)
            push_image(cpu, 0, 0x8000);
        else
            push_i(cpu, -x);
    } else {
        push_i(cpu, x);
    }
}

static void fbstp(OcerzCPU *cpu, uint64_t ea)
{
    int64_t x = 0;
    int inexact = 0, neg = signbit(st(cpu, 0)) != 0;
    int ok = st0_to_int(cpu, (cpu->fcw >> 10) & 3, &x, &inexact);
    uint8_t b[10];
    if (ok && (x >= 1000000000000000000ll || x <= -1000000000000000000ll))
        ok = 0;
    if (!ok) {
        static const uint8_t indefinite[10] = { 0, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0xff };
        memcpy(b, indefinite, 10);
        cpu->fsw |= X87_IE;
    } else {
        if (inexact)
            cpu->fsw |= X87_PE;
        uint64_t m = x < 0 ? (uint64_t)(-x) : (uint64_t)x;
        for (int i = 0; i < 9; i++) {
            unsigned lo = (unsigned)(m % 10);
            m /= 10;
            unsigned hi = (unsigned)(m % 10);
            m /= 10;
            b[i] = (uint8_t)((hi << 4) | lo);
        }
        b[9] = (x < 0 || (x == 0 && neg)) ? 0x80 : 0;
    }
    memcpy(ocerz_g2h(ea), b, 10);
    pop(cpu);
}

/* ---- arithmetic ---- */

enum { X87_ADD, X87_SUB, X87_MUL, X87_DIV };

static double binop(OcerzCPU *cpu, int kind, double a, double b)
{
    X87Env e = env_enter(cpu);
    volatile double va = a, vb = b;
    volatile double r;
    switch (kind) {
    case X87_ADD: r = va + vb; break;
    case X87_SUB: r = va - vb; break;
    case X87_MUL: r = va * vb; break;
    default: r = va / vb; break;
    }
    double out = pc_round(cpu, r);
    env_leave(cpu, e, X87_FLAGS);
    return isnan(out) ? nan2(a, b) : out;
}

static int arith(OcerzCPU *cpu, const X86Insn *insn)
{
    int kind, rev = 0, popit = 0, intform = 0;
    switch (insn->op) {
    case OCERZ_OP_FADD: kind = X87_ADD; break;
    case OCERZ_OP_FADDP: kind = X87_ADD; popit = 1; break;
    case OCERZ_OP_FIADD: kind = X87_ADD; intform = 1; break;
    case OCERZ_OP_FSUB: kind = X87_SUB; break;
    case OCERZ_OP_FSUBP: kind = X87_SUB; popit = 1; break;
    case OCERZ_OP_FISUB: kind = X87_SUB; intform = 1; break;
    case OCERZ_OP_FSUBR: kind = X87_SUB; rev = 1; break;
    case OCERZ_OP_FSUBRP: kind = X87_SUB; rev = 1; popit = 1; break;
    case OCERZ_OP_FISUBR: kind = X87_SUB; rev = 1; intform = 1; break;
    case OCERZ_OP_FMUL: kind = X87_MUL; break;
    case OCERZ_OP_FMULP: kind = X87_MUL; popit = 1; break;
    case OCERZ_OP_FIMUL: kind = X87_MUL; intform = 1; break;
    case OCERZ_OP_FDIV: kind = X87_DIV; break;
    case OCERZ_OP_FDIVP: kind = X87_DIV; popit = 1; break;
    case OCERZ_OP_FIDIV: kind = X87_DIV; intform = 1; break;
    case OCERZ_OP_FDIVR: kind = X87_DIV; rev = 1; break;
    case OCERZ_OP_FDIVRP: kind = X87_DIV; rev = 1; popit = 1; break;
    case OCERZ_OP_FIDIVR: kind = X87_DIV; rev = 1; intform = 1; break;
    default: return OCERZ_EUNSUP;
    }

    int dst = 0;
    double a, b;
    if (insn->nops >= 1 && insn->ops[0].kind != OCERZ_OPK_ST) {
        a = st(cpu, 0);
        b = intform ? (double)load_int(cpu, insn, &insn->ops[0]) : load_real(cpu, insn, &insn->ops[0]);
    } else if (insn->nops >= 2) {
        dst = insn->ops[0].reg;
        a = st(cpu, dst);
        b = st(cpu, insn->ops[1].reg);
    } else {
        /* No operands: the old faddp-style shorthand for ST(1) op ST(0). */
        dst = popit ? 1 : 0;
        a = st(cpu, dst);
        b = st(cpu, popit ? 0 : 1);
    }
    double r = rev ? binop(cpu, kind, b, a) : binop(cpu, kind, a, b);
    set_st(cpu, dst, r);
    if (popit)
        pop(cpu);
    return OCERZ_STEP_OK;
}

/* ---- compares ---- */

static void fcom(OcerzCPU *cpu, double a, double b, int quiet)
{
    if (isnan(a) || isnan(b)) {
        set_cc(cpu, X87_C0 | X87_C2 | X87_C3);
        if (!quiet || is_snan(a) || is_snan(b))
            cpu->fsw |= X87_IE;
    } else if (a < b) {
        set_cc(cpu, X87_C0);
    } else if (a == b) {
        set_cc(cpu, X87_C3);
    } else {
        set_cc(cpu, 0);
    }
}

static void fcomi(OcerzCPU *cpu, double a, double b, int quiet)
{
    cpu->rflags &= ~(uint64_t)(OCERZ_OF | OCERZ_AF | OCERZ_SF | OCERZ_ZF | OCERZ_PF | OCERZ_CF);
    cpu->fsw &= (uint16_t)~X87_C1;
    if (isnan(a) || isnan(b)) {
        cpu->rflags |= OCERZ_ZF | OCERZ_PF | OCERZ_CF;
        if (!quiet || is_snan(a) || is_snan(b))
            cpu->fsw |= X87_IE;
    } else if (a < b) {
        cpu->rflags |= OCERZ_CF;
    } else if (a == b) {
        cpu->rflags |= OCERZ_ZF;
    }
}

/* The ST(i) a one-operand compare names, or ST(1) when it names none. */
static int st_operand(const X86Insn *insn)
{
    if (insn->nops >= 2)
        return insn->ops[1].reg;
    if (insn->nops == 1 && insn->ops[0].kind == OCERZ_OPK_ST)
        return insn->ops[0].reg;
    return 1;
}

static int compare(OcerzCPU *cpu, const X86Insn *insn)
{
    int op = insn->op;
    double a = st(cpu, 0), b;
    int mem = insn->nops >= 1 && insn->ops[0].kind != OCERZ_OPK_ST;

    switch (op) {
    case OCERZ_OP_FCOM: case OCERZ_OP_FCOMP:
    case OCERZ_OP_FUCOM: case OCERZ_OP_FUCOMP:
        b = mem ? load_real(cpu, insn, &insn->ops[0]) : st(cpu, st_operand(insn));
        fcom(cpu, a, b, op == OCERZ_OP_FUCOM || op == OCERZ_OP_FUCOMP);
        if (op == OCERZ_OP_FCOMP || op == OCERZ_OP_FUCOMP)
            pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FICOM: case OCERZ_OP_FICOMP:
        fcom(cpu, a, (double)load_int(cpu, insn, &insn->ops[0]), 0);
        if (op == OCERZ_OP_FICOMP)
            pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FCOMPP: case OCERZ_OP_FUCOMPP:
        fcom(cpu, a, st(cpu, 1), op == OCERZ_OP_FUCOMPP);
        pop(cpu);
        pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FTST:
        fcom(cpu, a, 0.0, 0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FCOMI: case OCERZ_OP_FCOMIP:
    case OCERZ_OP_FUCOMI: case OCERZ_OP_FUCOMIP:
        fcomi(cpu, a, st(cpu, st_operand(insn)), op == OCERZ_OP_FUCOMI || op == OCERZ_OP_FUCOMIP);
        if (op == OCERZ_OP_FCOMIP || op == OCERZ_OP_FUCOMIP)
            pop(cpu);
        return OCERZ_STEP_OK;
    default:
        return OCERZ_EUNSUP;
    }
}

static void fxam(OcerzCPU *cpu)
{
    int p = phys(cpu, 0);
    double v = cpu->fpr[p];
    unsigned cc = signbit(v) ? X87_C1 : 0;
    if (!((cpu->ftw >> p) & 1))
        cc |= X87_C3 | X87_C0;
    else if (image_ok(cpu, p))
        cc |= f80_class(cpu->fpr_xm[p], cpu->fpr_xe[p]);
    else if (isnan(v))
        cc |= X87_C0;
    else if (isinf(v))
        cc |= X87_C2 | X87_C0;
    else if (v == 0.0)
        cc |= X87_C3;
    else
        cc |= X87_C2;
    set_cc(cpu, cc);
}

/* ---- the transcendental and partial-remainder group ---- */

static int out_of_range(OcerzCPU *cpu, double v)
{
    if (isfinite(v) && fabs(v) >= 9223372036854775808.0) {
        cpu->fsw = (uint16_t)((cpu->fsw & ~X87_CC) | X87_C2);
        return 1;
    }
    cpu->fsw &= (uint16_t)~(X87_C2 | X87_C1);
    return 0;
}

static void fprem(OcerzCPU *cpu, int ieee)
{
    double x = st(cpu, 0), y = st(cpu, 1), r;
    unsigned q = 0;
    if (isnan(x) || isnan(y)) {
        r = nan2(x, y);
        if (is_snan(x) || is_snan(y))
            cpu->fsw |= X87_IE;
    } else if (isinf(x) || y == 0.0) {
        r = bitsd(X87_DEFAULT_NAN);
        cpu->fsw |= X87_IE;
    } else if (isinf(y) || x == 0.0) {
        r = x;
    } else if (ilogb(x) - ilogb(y) >= 64) {
        /* A partial step, as the hardware takes one: C2 says to go again. */
        r = fmod(x, ldexp(y, ilogb(x) - ilogb(y) - 32));
        set_st(cpu, 0, r);
        set_cc(cpu, X87_C2);
        return;
    } else {
        int quo;
        double rn = remquo(x, y, &quo);
        unsigned qn = (unsigned)(quo < 0 ? -quo : quo);
        if (ieee) {
            r = rn;
            q = qn;
        } else {
            r = fmod(x, y);
            q = (r == rn) ? qn : qn - 1;
        }
    }
    set_st(cpu, 0, r);
    unsigned cc = 0;
    if (q & 4) cc |= X87_C0;
    if (q & 2) cc |= X87_C3;
    if (q & 1) cc |= X87_C1;
    set_cc(cpu, cc);
}

static void fxtract(OcerzCPU *cpu)
{
    double v = st(cpu, 0), ex, sig;
    if (isnan(v)) {
        ex = sig = quieted(v);
        if (is_snan(v))
            cpu->fsw |= X87_IE;
    } else if (isinf(v)) {
        ex = INFINITY;
        sig = v;
    } else if (v == 0.0) {
        ex = -INFINITY;
        sig = v;
        cpu->fsw |= X87_ZE;
    } else {
        int e = ilogb(v);
        ex = (double)e;
        sig = scalbn(v, -e);
    }
    set_st(cpu, 0, ex);
    push_d(cpu, sig);
}

static void fscale(OcerzCPU *cpu)
{
    double x = st(cpu, 0), n = st(cpu, 1), r;
    if (isnan(x) || isnan(n)) {
        r = nan2(x, n);
    } else if (isinf(n)) {
        if ((n > 0 && x == 0.0) || (n < 0 && isinf(x))) {
            r = bitsd(X87_DEFAULT_NAN);
            cpu->fsw |= X87_IE;
        } else {
            r = n > 0 ? x * INFINITY : (isinf(x) ? x : copysign(0.0, x));
            if (x == 0.0)
                r = x;
        }
    } else {
        double t = trunc(n);
        if (t > 100000.0)
            t = 100000.0;
        if (t < -100000.0)
            t = -100000.0;
        X87Env e = env_enter(cpu);
        volatile double vr = ldexp(x, (int)t);
        env_leave(cpu, e, X87_OE | X87_UE | X87_PE);
        r = vr;
    }
    set_st(cpu, 0, r);
}

static int transcendental(OcerzCPU *cpu, int op)
{
    double x = st(cpu, 0), r;
    X87Env e;
    switch (op) {
    case OCERZ_OP_F2XM1:
        e = env_enter(cpu);
        r = expm1(x * 0.69314718055994530942);
        env_leave(cpu, e, X87_UE | X87_PE);
        set_st(cpu, 0, isnan(r) ? nan1(x) : r);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FYL2X:
    case OCERZ_OP_FYL2XP1: {
        double y = st(cpu, 1);
        e = env_enter(cpu);
        double l = op == OCERZ_OP_FYL2X ? log2(x) : log1p(x) * 1.44269504088896340736;
        volatile double vr = y * l;
        env_leave(cpu, e, X87_IE | X87_ZE | X87_OE | X87_UE | X87_PE);
        r = vr;
        pop(cpu);
        set_st(cpu, 0, isnan(r) ? nan2(x, y) : r);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FPATAN: {
        double y = st(cpu, 1);
        e = env_enter(cpu);
        r = atan2(y, x);
        env_leave(cpu, e, X87_UE | X87_PE);
        pop(cpu);
        set_st(cpu, 0, isnan(r) ? nan2(x, y) : r);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FSIN:
    case OCERZ_OP_FCOS:
    case OCERZ_OP_FPTAN:
    case OCERZ_OP_FSINCOS: {
        if (out_of_range(cpu, x))
            return OCERZ_STEP_OK;
        if (isinf(x))
            cpu->fsw |= X87_IE;
        e = env_enter(cpu);
        double s = 0, c = 0;
        if (op == OCERZ_OP_FSIN || op == OCERZ_OP_FSINCOS)
            s = sin(x);
        if (op == OCERZ_OP_FCOS || op == OCERZ_OP_FSINCOS)
            c = cos(x);
        if (op == OCERZ_OP_FPTAN)
            s = tan(x);
        env_leave(cpu, e, X87_UE | X87_PE);
        if (op == OCERZ_OP_FCOS) {
            set_st(cpu, 0, isnan(c) ? nan1(x) : c);
        } else {
            set_st(cpu, 0, isnan(s) ? nan1(x) : s);
            if (op == OCERZ_OP_FPTAN)
                push_d(cpu, 1.0);
            else if (op == OCERZ_OP_FSINCOS)
                push_d(cpu, isnan(c) ? nan1(x) : c);
        }
        return OCERZ_STEP_OK;
    }
    default:
        return OCERZ_EUNSUP;
    }
}

/* ---- environment images ---- */

static unsigned tag_of(const OcerzCPU *cpu, int p)
{
    if (!((cpu->ftw >> p) & 1))
        return 3;
    if (image_ok(cpu, p)) {
        unsigned c = f80_class(cpu->fpr_xm[p], cpu->fpr_xe[p]);
        return c == X87_C3 ? 1 : c == X87_C2 ? 0 : 2;
    }
    double v = cpu->fpr[p];
    if (v == 0.0)
        return 1;
    if (!isfinite(v))
        return 2;
    return 0;
}

static uint16_t full_tags(const OcerzCPU *cpu)
{
    uint16_t t = 0;
    for (int p = 0; p < 8; p++)
        t |= (uint16_t)(tag_of(cpu, p) << (2 * p));
    return t;
}

static void store_env(OcerzCPU *cpu, uint64_t ea, int image16)
{
    if (image16) {
        ocerz_st(ea + 0, 2, cpu->fcw);
        ocerz_st(ea + 2, 2, ocerz_x87_fsw(cpu));
        ocerz_st(ea + 4, 2, full_tags(cpu));
        for (int i = 6; i < 14; i += 2)
            ocerz_st(ea + i, 2, 0);
        return;
    }
    ocerz_st(ea + 0, 4, cpu->fcw);
    ocerz_st(ea + 4, 4, ocerz_x87_fsw(cpu));
    ocerz_st(ea + 8, 4, full_tags(cpu));
    ocerz_st(ea + 12, 4, 0);
    ocerz_st(ea + 16, 4, 0);
    ocerz_st(ea + 20, 4, 0);
    ocerz_st(ea + 24, 4, 0);
}

static void load_env(OcerzCPU *cpu, uint64_t ea, int image16)
{
    int step = image16 ? 2 : 4;
    cpu->fcw = (uint16_t)(ocerz_ld(ea, 2) | 0x40);
    uint16_t sw = (uint16_t)ocerz_ld(ea + (uint64_t)step, 2);
    uint16_t tw = (uint16_t)ocerz_ld(ea + 2 * (uint64_t)step, 2);
    cpu->fsw = sw;
    cpu->ftop = (uint8_t)((sw >> 11) & 7);
    uint8_t abridged = 0;
    for (int p = 0; p < 8; p++)
        if (((tw >> (2 * p)) & 3) != 3)
            abridged |= (uint8_t)(1u << p);
    cpu->ftw = abridged;
}

void ocerz_x87_fxsave(const OcerzCPU *cpu, uint64_t ea)
{
    ocerz_st(ea + 0, 2, cpu->fcw);
    ocerz_st(ea + 2, 2, ocerz_x87_fsw(cpu));
    ocerz_st(ea + 4, 1, cpu->ftw);
    ocerz_st(ea + 5, 1, 0);
    ocerz_st(ea + 6, 2, 0);
    ocerz_st(ea + 8, 8, 0);
    ocerz_st(ea + 16, 8, 0);
    for (int i = 0; i < 8; i++) {
        uint8_t buf[16] = { 0 };
        ocerz_x87_to_f80(cpu, phys(cpu, i), buf);
        memcpy(ocerz_g2h(ea + 32 + (uint64_t)i * 16), buf, 16);
    }
}

void ocerz_x87_fxrstor(OcerzCPU *cpu, uint64_t ea)
{
    cpu->fcw = (uint16_t)(ocerz_ld(ea + 0, 2) | 0x40);
    cpu->fsw = (uint16_t)ocerz_ld(ea + 2, 2);
    cpu->ftop = (uint8_t)((cpu->fsw >> 11) & 7);
    cpu->ftw = (uint8_t)ocerz_ld(ea + 4, 1);
    for (int i = 0; i < 8; i++) {
        uint8_t buf[10];
        memcpy(buf, ocerz_g2h(ea + 32 + (uint64_t)i * 16), 10);
        ocerz_x87_from_f80(cpu, phys(cpu, i), buf);
    }
}

static void fnsave(OcerzCPU *cpu, uint64_t ea, int image16)
{
    store_env(cpu, ea, image16);
    uint64_t regs = ea + (image16 ? 14 : 28);
    for (int i = 0; i < 8; i++) {
        uint8_t buf[10];
        ocerz_x87_to_f80(cpu, phys(cpu, i), buf);
        memcpy(ocerz_g2h(regs + (uint64_t)i * 10), buf, 10);
    }
    ocerz_x87_reset(cpu);
}

static void frstor(OcerzCPU *cpu, uint64_t ea, int image16)
{
    load_env(cpu, ea, image16);
    uint64_t regs = ea + (image16 ? 14 : 28);
    for (int i = 0; i < 8; i++) {
        uint8_t buf[10];
        memcpy(buf, ocerz_g2h(regs + (uint64_t)i * 10), 10);
        ocerz_x87_from_f80(cpu, phys(cpu, i), buf);
    }
}

/* ---- dispatch ---- */

int ocerz_x87_exec(OcerzCPU *cpu, const X86Insn *insn)
{
    int op = insn->op;
    const X86Operand *o = &insn->ops[0];

    switch (op) {
    case OCERZ_OP_FLD:
        if (o->kind == OCERZ_OPK_ST) {
            int src = o->reg;
            push_slot(cpu);
            copy_st(cpu, 0, src + 1);
        } else if (o->size == 10) {
            uint8_t buf[10];
            memcpy(buf, ocerz_g2h(ocerz_ea(cpu, insn, o)), 10);
            push_slot(cpu);
            ocerz_x87_from_f80(cpu, cpu->ftop, buf);
        } else {
            push_d(cpu, load_real(cpu, insn, o));
        }
        return OCERZ_STEP_OK;
    case OCERZ_OP_FILD:
        push_i(cpu, load_int(cpu, insn, o));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FST:
    case OCERZ_OP_FSTP:
        if (o->kind == OCERZ_OPK_ST)
            copy_st(cpu, o->reg, 0);
        else
            store_real(cpu, insn, o);
        if (op == OCERZ_OP_FSTP)
            pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FIST:
        store_int(cpu, insn, o, 0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FISTP:
        store_int(cpu, insn, o, 0);
        pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FISTTP:
        store_int(cpu, insn, o, 1);
        pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FBLD:
        fbld(cpu, ocerz_ea(cpu, insn, o));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FBSTP:
        fbstp(cpu, ocerz_ea(cpu, insn, o));
        return OCERZ_STEP_OK;

    case OCERZ_OP_FLDCW:
        cpu->fcw = (uint16_t)(ocerz_ld(ocerz_ea(cpu, insn, o), 2) | 0x40);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FNSTCW:
        ocerz_st(ocerz_ea(cpu, insn, o), 2, cpu->fcw);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FNSTSW:
        if (insn->nops > 0 && o->kind == OCERZ_OPK_REG)
            ocerz_write_gpr(cpu, o->reg, 2, 0, ocerz_x87_fsw(cpu));
        else
            ocerz_st(ocerz_ea(cpu, insn, o), 2, ocerz_x87_fsw(cpu));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FNSTENV:
        store_env(cpu, ocerz_ea(cpu, insn, o), insn->opsize == 14);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FLDENV:
        load_env(cpu, ocerz_ea(cpu, insn, o), insn->opsize == 14);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FNSAVE:
        fnsave(cpu, ocerz_ea(cpu, insn, o), insn->opsize == 94);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FRSTOR:
        frstor(cpu, ocerz_ea(cpu, insn, o), insn->opsize == 94);
        return OCERZ_STEP_OK;

    case OCERZ_OP_FXCH:
        exchange(cpu, (insn->nops > 0 && o->kind == OCERZ_OPK_ST) ? o->reg : 1);
        cpu->fsw &= (uint16_t)~X87_C1;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FCHS:
        set_st(cpu, 0, bitsd(dbits(st(cpu, 0)) ^ DBL_SIGN));
        cpu->fsw &= (uint16_t)~X87_C1;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FABS:
        set_st(cpu, 0, bitsd(dbits(st(cpu, 0)) & ~DBL_SIGN));
        cpu->fsw &= (uint16_t)~X87_C1;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FSQRT: {
        double x = st(cpu, 0);
        X87Env e = env_enter(cpu);
        volatile double vx = x;
        double r = pc_round(cpu, sqrt(vx));
        env_leave(cpu, e, X87_FLAGS);
        set_st(cpu, 0, isnan(r) ? nan1(x) : r);
        return OCERZ_STEP_OK;
    }
    case OCERZ_OP_FRNDINT: {
        double x = st(cpu, 0);
        X87Env e = env_enter(cpu);
        volatile double vx = x;
        double r = rint(vx);
        env_leave(cpu, e, X87_IE | X87_PE);
        set_st(cpu, 0, isnan(r) ? nan1(x) : r);
        return OCERZ_STEP_OK;
    }

    case OCERZ_OP_FLDZ: push_i(cpu, 0); return OCERZ_STEP_OK;
    case OCERZ_OP_FLD1: push_i(cpu, 1); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDPI: push_image(cpu, 0xc90fdaa22168c235ull, 0x4000); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDL2E: push_image(cpu, 0xb8aa3b295c17f0bcull, 0x3fff); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDL2T: push_image(cpu, 0xd49a784bcd1b8afeull, 0x4000); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDLG2: push_image(cpu, 0x9a209a84fbcff799ull, 0x3ffd); return OCERZ_STEP_OK;
    case OCERZ_OP_FLDLN2: push_image(cpu, 0xb17217f7d1cf79acull, 0x3ffe); return OCERZ_STEP_OK;

    case OCERZ_OP_F2XM1: case OCERZ_OP_FYL2X: case OCERZ_OP_FYL2XP1: case OCERZ_OP_FPATAN:
    case OCERZ_OP_FSIN: case OCERZ_OP_FCOS: case OCERZ_OP_FPTAN: case OCERZ_OP_FSINCOS:
        return transcendental(cpu, op);
    case OCERZ_OP_FPREM:
        fprem(cpu, 0);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FPREM1:
        fprem(cpu, 1);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FSCALE:
        fscale(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FXTRACT:
        fxtract(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FXAM:
        fxam(cpu);
        return OCERZ_STEP_OK;

    case OCERZ_OP_FCMOVCC:
        if (ocerz_cc_eval(cpu, insn->cc))
            copy_st(cpu, 0, st_operand(insn));
        return OCERZ_STEP_OK;

    case OCERZ_OP_FNINIT:
        ocerz_x87_reset(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FNCLEX:
        cpu->fsw &= (uint16_t)~0x80ffu;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FFREE:
        cpu->ftw &= (uint8_t)~(1u << phys(cpu, o->reg));
        return OCERZ_STEP_OK;
    case OCERZ_OP_FFREEP:
        cpu->ftw &= (uint8_t)~(1u << phys(cpu, o->reg));
        pop(cpu);
        return OCERZ_STEP_OK;
    case OCERZ_OP_FINCSTP:
        cpu->ftop = (cpu->ftop + 1) & 7;
        cpu->fsw &= (uint16_t)~X87_C1;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FDECSTP:
        cpu->ftop = (cpu->ftop - 1) & 7;
        cpu->fsw &= (uint16_t)~X87_C1;
        return OCERZ_STEP_OK;
    case OCERZ_OP_FWAIT:
        return OCERZ_STEP_OK;

    case OCERZ_OP_FADD: case OCERZ_OP_FADDP: case OCERZ_OP_FIADD:
    case OCERZ_OP_FSUB: case OCERZ_OP_FSUBP: case OCERZ_OP_FISUB:
    case OCERZ_OP_FSUBR: case OCERZ_OP_FSUBRP: case OCERZ_OP_FISUBR:
    case OCERZ_OP_FMUL: case OCERZ_OP_FMULP: case OCERZ_OP_FIMUL:
    case OCERZ_OP_FDIV: case OCERZ_OP_FDIVP: case OCERZ_OP_FIDIV:
    case OCERZ_OP_FDIVR: case OCERZ_OP_FDIVRP: case OCERZ_OP_FIDIVR:
        return arith(cpu, insn);

    case OCERZ_OP_FCOM: case OCERZ_OP_FCOMP: case OCERZ_OP_FCOMPP:
    case OCERZ_OP_FUCOM: case OCERZ_OP_FUCOMP: case OCERZ_OP_FUCOMPP:
    case OCERZ_OP_FICOM: case OCERZ_OP_FICOMP:
    case OCERZ_OP_FTST:
    case OCERZ_OP_FCOMI: case OCERZ_OP_FCOMIP:
    case OCERZ_OP_FUCOMI: case OCERZ_OP_FUCOMIP:
        return compare(cpu, insn);

    default:
        return OCERZ_EUNSUP;
    }
}
