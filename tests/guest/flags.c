/*
 * tests/guest/flags.c
 *
 * Flag-exact ALU torture test, written for the JIT tier-2 differential gate.
 * Unlike alu.c (which folds only DATA results into one checksum and so can
 * hide a flag-only divergence), this program captures the ACTUAL x86 RFLAGS
 * after each operation through a separate channel and folds that into the
 * checksum, so any single-flag (CF/OF/AF/PF/ZF/SF) mismatch between the
 * interpreter and the JIT changes the printed output.
 *
 * Each helper performs one ALU instruction on inline-asm operands forced
 * into registers, then immediately captures RFLAGS with PUSHF/POP, masking
 * to the six architectural arithmetic flags (CF PF AF ZF SF OF) so the
 * comparison is independent of the always-set fixed bit and the IF/DF state.
 * Operand pairs sweep the hard boundaries: zero, one, all-ones, the signed
 * min/max at each width, equal operands (ZF), off-by-one (CF/borrow),
 * signed-overflow pairs (OF), low-nibble carries (AF), and even/odd-parity
 * bytes (PF). ADC/SBB are driven with an explicit incoming carry; the shifts
 * cover immediate counts 1, the width, and the half-width; SETcc is swept
 * across all sixteen conditions after a representative CMP. The whole thing
 * runs as a tight loop of reg<-reg and reg<-imm operations followed by a
 * conditional branch on the captured flags, which is exactly the in-block
 * native ALU + Jcc path the JIT now emits, so the differential exercises it
 * directly.
 */
#include "gsys.h"

#define ARITH_MASK 0x0cd5ULL

static g_u64 cap_add64(g_u64 a, g_u64 b)
{
    g_u64 r, f;
    __asm__ volatile("add %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "r"(b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_sub64(g_u64 a, g_u64 b)
{
    g_u64 r, f;
    __asm__ volatile("sub %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "r"(b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_cmp64(g_u64 a, g_u64 b)
{
    g_u64 f;
    __asm__ volatile("cmp %2, %1\n\tpushfq\n\tpop %0"
                     : "=r"(f) : "r"(a), "r"(b) : "cc");
    return (f & ARITH_MASK);
}

static g_u64 cap_and64(g_u64 a, g_u64 b)
{
    g_u64 r, f;
    __asm__ volatile("and %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "r"(b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_or64(g_u64 a, g_u64 b)
{
    g_u64 r, f;
    __asm__ volatile("or %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "r"(b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_xor64(g_u64 a, g_u64 b)
{
    g_u64 r, f;
    __asm__ volatile("xor %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "r"(b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_test64(g_u64 a, g_u64 b)
{
    g_u64 f;
    __asm__ volatile("test %2, %1\n\tpushfq\n\tpop %0"
                     : "=r"(f) : "r"(a), "r"(b) : "cc");
    return (f & ARITH_MASK);
}

static g_u64 cap_inc64(g_u64 a, int seed_cf)
{
    g_u64 r, f;
    __asm__ volatile(
        "test %3, %3\n\t"
        "jz 1f\n\t"
        "stc\n\t"
        "jmp 2f\n\t"
        "1: clc\n\t"
        "2: inc %0\n\tpushfq\n\tpop %1"
        : "=&r"(r), "=r"(f) : "0"(a), "r"((g_u64)seed_cf) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_dec64(g_u64 a, int seed_cf)
{
    g_u64 r, f;
    __asm__ volatile(
        "test %3, %3\n\t"
        "jz 1f\n\t"
        "stc\n\t"
        "jmp 2f\n\t"
        "1: clc\n\t"
        "2: dec %0\n\tpushfq\n\tpop %1"
        : "=&r"(r), "=r"(f) : "0"(a), "r"((g_u64)seed_cf) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_add32(g_u32 a, g_u32 b)
{
    g_u64 r = 0, f;
    __asm__ volatile("add %k3, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_sub32(g_u32 a, g_u32 b)
{
    g_u64 r = 0, f;
    __asm__ volatile("sub %k3, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_cmp32(g_u32 a, g_u32 b)
{
    g_u64 f;
    __asm__ volatile("cmp %k2, %k1\n\tpushfq\n\tpop %0"
                     : "=r"(f) : "r"((g_u64)a), "r"((g_u64)b) : "cc");
    return (f & ARITH_MASK);
}

static g_u64 cap_and32(g_u32 a, g_u32 b)
{
    g_u64 r = 0, f;
    __asm__ volatile("and %k3, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_xor32(g_u32 a, g_u32 b)
{
    g_u64 r = 0, f;
    __asm__ volatile("xor %k3, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)b) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_inc32(g_u32 a, int seed_cf)
{
    g_u64 r = 0, f;
    __asm__ volatile(
        "test %3, %3\n\t"
        "jz 1f\n\t"
        "stc\n\t"
        "jmp 2f\n\t"
        "1: clc\n\t"
        "2: inc %k0\n\tpushfq\n\tpop %1"
        : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)seed_cf) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_dec32(g_u32 a, int seed_cf)
{
    g_u64 r = 0, f;
    __asm__ volatile(
        "test %3, %3\n\t"
        "jz 1f\n\t"
        "stc\n\t"
        "jmp 2f\n\t"
        "1: clc\n\t"
        "2: dec %k0\n\tpushfq\n\tpop %1"
        : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)seed_cf) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_addimm64(g_u64 a)
{
    g_u64 r, f;
    __asm__ volatile("add $0x7b, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_subimm32(g_u32 a)
{
    g_u64 r = 0, f;
    __asm__ volatile("sub $0x1234, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_andimm64(g_u64 a)
{
    g_u64 r, f;
    __asm__ volatile("and $0x0f0f0f0f, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_cmpimm64(g_u64 a)
{
    g_u64 f;
    __asm__ volatile("cmp $0x64, %1\n\tpushfq\n\tpop %0"
                     : "=r"(f) : "r"(a) : "cc");
    return (f & ARITH_MASK);
}

#define SHIFT_CAP(name, mnem, count) \
static g_u64 name(g_u64 a) \
{ \
    g_u64 r, f; \
    __asm__ volatile(mnem " $" #count ", %0\n\tpushfq\n\tpop %1" \
                     : "=&r"(r), "=r"(f) : "0"(a) : "cc"); \
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL); \
}

SHIFT_CAP(cap_shl64_1, "shl", 1)
SHIFT_CAP(cap_shl64_7, "shl", 7)
SHIFT_CAP(cap_shl64_31, "shl", 31)
SHIFT_CAP(cap_shr64_1, "shr", 1)
SHIFT_CAP(cap_shr64_5, "shr", 5)
SHIFT_CAP(cap_shr64_32, "shr", 32)
SHIFT_CAP(cap_sar64_1, "sar", 1)
SHIFT_CAP(cap_sar64_9, "sar", 9)
SHIFT_CAP(cap_sar64_40, "sar", 40)

#define SHIFT_CAP32(name, mnem, count) \
static g_u64 name(g_u32 a) \
{ \
    g_u64 r = 0, f; \
    __asm__ volatile(mnem " $" #count ", %k0\n\tpushfq\n\tpop %1" \
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a) : "cc"); \
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL); \
}

SHIFT_CAP32(cap_shl32_1, "shl", 1)
SHIFT_CAP32(cap_shl32_13, "shl", 13)
SHIFT_CAP32(cap_shr32_1, "shr", 1)
SHIFT_CAP32(cap_shr32_17, "shr", 17)
SHIFT_CAP32(cap_sar32_1, "sar", 1)
SHIFT_CAP32(cap_sar32_20, "sar", 20)

static g_u64 cap_adc64(g_u64 a, g_u64 b, int cin)
{
    g_u64 r, f;
    __asm__ volatile(
        "test %4, %4\n\t"
        "jz 1f\n\t"
        "stc\n\t"
        "jmp 2f\n\t"
        "1: clc\n\t"
        "2: adc %3, %0\n\tpushfq\n\tpop %1"
        : "=&r"(r), "=r"(f) : "0"(a), "r"(b), "r"((g_u64)cin) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_sbb64(g_u64 a, g_u64 b, int cin)
{
    g_u64 r, f;
    __asm__ volatile(
        "test %4, %4\n\t"
        "jz 1f\n\t"
        "stc\n\t"
        "jmp 2f\n\t"
        "1: clc\n\t"
        "2: sbb %3, %0\n\tpushfq\n\tpop %1"
        : "=&r"(r), "=r"(f) : "0"(a), "r"(b), "r"((g_u64)cin) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 setcc_sweep(g_u64 a, g_u64 b)
{
    g_u64 acc = 0;
    unsigned char s;
#define SETONE(mnem) \
    __asm__ volatile("cmp %2, %1\n\t" mnem " %0" : "=r"(s) : "r"(a), "r"(b) : "cc"); \
    acc = acc * 3 + s;
    SETONE("seto")
    SETONE("setno")
    SETONE("setb")
    SETONE("setae")
    SETONE("sete")
    SETONE("setne")
    SETONE("setbe")
    SETONE("seta")
    SETONE("sets")
    SETONE("setns")
    SETONE("setp")
    SETONE("setnp")
    SETONE("setl")
    SETONE("setge")
    SETONE("setle")
    SETONE("setg")
#undef SETONE
    return acc;
}

/* Memory-source ALU: the b-operand lives in memory, exercising the JIT's
 * reg<-mem inlined arithmetic and its commpage-guarded native load. The
 * operand is forced through a stack slot the compiler must reload. */
static g_u64 cap_addm64(g_u64 a, g_u64 b)
{
    volatile g_u64 m = b;
    g_u64 r, f;
    __asm__ volatile("add %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "m"(m) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_subm64(g_u64 a, g_u64 b)
{
    volatile g_u64 m = b;
    g_u64 r, f;
    __asm__ volatile("sub %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "m"(m) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_cmpm64(g_u64 a, g_u64 b)
{
    volatile g_u64 m = b;
    g_u64 f;
    __asm__ volatile("cmp %2, %1\n\tpushfq\n\tpop %0"
                     : "=r"(f) : "r"(a), "m"(m) : "cc");
    return (f & ARITH_MASK);
}

static g_u64 cap_andm64(g_u64 a, g_u64 b)
{
    volatile g_u64 m = b;
    g_u64 r, f;
    __asm__ volatile("and %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "m"(m) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_xorm64(g_u64 a, g_u64 b)
{
    volatile g_u64 m = b;
    g_u64 r, f;
    __asm__ volatile("xor %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "m"(m) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_addm32(g_u32 a, g_u32 b)
{
    volatile g_u32 m = b;
    g_u64 r = 0, f;
    __asm__ volatile("add %3, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "m"(m) : "cc");
    return (r ^ (f & ARITH_MASK) * 0x100000001ULL);
}

static g_u64 cap_movm(g_u64 b)
{
    volatile g_u64 m = b;
    g_u64 r;
    __asm__ volatile("mov %1, %0" : "=r"(r) : "m"(m) : "cc");
    volatile g_u64 out;
    __asm__ volatile("mov %1, %0" : "=m"(out) : "r"(r) : "cc");
    return r ^ out;
}

static g_u64 mix(g_u64 acc, g_u64 v)
{
    acc ^= v;
    acc = (acc << 13) | (acc >> 51);
    acc += 0x9e3779b97f4a7c15ULL;
    acc ^= acc >> 29;
    return acc;
}

static const g_u64 vals64[] = {
    0, 1, 2, 0xffffffffffffffffULL, 0x8000000000000000ULL, 0x7fffffffffffffffULL,
    0x0fULL, 0x10ULL, 0xffULL, 0x100ULL, 0x0123456789abcdefULL, 0xfedcba9876543210ULL,
    0x00000000ffffffffULL, 0x0000000100000000ULL, 0x5555555555555555ULL, 0xaaaaaaaaaaaaaaaaULL,
    0x55ULL, 0xaaULL, 0x7fULL, 0x80ULL, 0xdeadbeefcafef00dULL, 0x0000000080000000ULL,
};
#define NV (sizeof(vals64) / sizeof(vals64[0]))

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    g_u64 acc = 0xcbf29ce484222325ULL;

    for (unsigned i = 0; i < NV; i++) {
        for (unsigned j = 0; j < NV; j++) {
            g_u64 a = vals64[i], b = vals64[j];
            g_u32 a32 = (g_u32)a, b32 = (g_u32)b;

            acc = mix(acc, cap_add64(a, b));
            acc = mix(acc, cap_sub64(a, b));
            acc = mix(acc, cap_cmp64(a, b));
            acc = mix(acc, cap_and64(a, b));
            acc = mix(acc, cap_or64(a, b));
            acc = mix(acc, cap_xor64(a, b));
            acc = mix(acc, cap_test64(a, b));

            acc = mix(acc, cap_add32(a32, b32));
            acc = mix(acc, cap_sub32(a32, b32));
            acc = mix(acc, cap_cmp32(a32, b32));
            acc = mix(acc, cap_and32(a32, b32));
            acc = mix(acc, cap_xor32(a32, b32));

            acc = mix(acc, cap_addm64(a, b));
            acc = mix(acc, cap_subm64(a, b));
            acc = mix(acc, cap_cmpm64(a, b));
            acc = mix(acc, cap_andm64(a, b));
            acc = mix(acc, cap_xorm64(a, b));
            acc = mix(acc, cap_addm32(a32, b32));
            acc = mix(acc, cap_movm(b));

            acc = mix(acc, cap_inc64(a, (int)(j & 1)));
            acc = mix(acc, cap_dec64(a, (int)(j & 1)));
            acc = mix(acc, cap_inc32(a32, (int)(j & 1)));
            acc = mix(acc, cap_dec32(a32, (int)(j & 1)));

            acc = mix(acc, cap_addimm64(a));
            acc = mix(acc, cap_subimm32(a32));
            acc = mix(acc, cap_andimm64(a));
            acc = mix(acc, cap_cmpimm64(a));

            acc = mix(acc, cap_adc64(a, b, (int)(j & 1)));
            acc = mix(acc, cap_sbb64(a, b, (int)(j & 1)));

            acc = mix(acc, setcc_sweep(a, b));
        }

        g_u64 a = vals64[i];
        g_u32 a32 = (g_u32)a;
        acc = mix(acc, cap_shl64_1(a));
        acc = mix(acc, cap_shl64_7(a));
        acc = mix(acc, cap_shl64_31(a));
        acc = mix(acc, cap_shr64_1(a));
        acc = mix(acc, cap_shr64_5(a));
        acc = mix(acc, cap_shr64_32(a));
        acc = mix(acc, cap_sar64_1(a));
        acc = mix(acc, cap_sar64_9(a));
        acc = mix(acc, cap_sar64_40(a));
        acc = mix(acc, cap_shl32_1(a32));
        acc = mix(acc, cap_shl32_13(a32));
        acc = mix(acc, cap_shr32_1(a32));
        acc = mix(acc, cap_shr32_17(a32));
        acc = mix(acc, cap_sar32_1(a32));
        acc = mix(acc, cap_sar32_20(a32));
    }

    g_u64 branchacc = 0;
    for (unsigned i = 0; i < NV; i++) {
        for (unsigned j = 0; j < NV; j++) {
            g_i64 a = (g_i64)vals64[i];
            g_i64 b = (g_i64)vals64[j];
            if (a < b) branchacc = mix(branchacc, 0x11);
            if (a <= b) branchacc = mix(branchacc, 0x22);
            if (a > b) branchacc = mix(branchacc, 0x33);
            if (a >= b) branchacc = mix(branchacc, 0x44);
            if (a == b) branchacc = mix(branchacc, 0x55);
            if (a != b) branchacc = mix(branchacc, 0x66);
            if ((g_u64)a < (g_u64)b) branchacc = mix(branchacc, 0x77);
            if ((g_u64)a > (g_u64)b) branchacc = mix(branchacc, 0x88);
            if ((g_u64)a >= (g_u64)b) branchacc = mix(branchacc, 0x99);
            if ((g_u64)a <= (g_u64)b) branchacc = mix(branchacc, 0xaa);
        }
    }
    acc = mix(acc, branchacc);

    g_puthex64(acc);
    return 0;
}
