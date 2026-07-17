/*
 * tests/guest/imul_flags.c
 *
 * The gate for src/jit.c:emit_imul (the inlined IMUL that landed alongside the
 * OCERZ_OP_MUL/IMUL def=ALL/use=0 entry in src/flags_live.c).
 *
 * WHY THIS FILE HAD TO EXIST. The corpus had an exact IMUL-shaped hole:
 *   - tests/guest/flags.c captures RFLAGS after every ALU op with PUSHF and is
 *     the file whose whole purpose is flag exactness — but it contains no imul
 *     at all, not one.
 *   - tests/guest/alu.c does multiply ("signed multiply including 32->64
 *     widening and full 64x64") but folds only the DATA result into its
 *     checksum, which is precisely the divergence class alu.c's own header says
 *     flags.c exists to cover.
 * So before this file, emit_imul's entire flag computation — the CF/OF
 * high-half-vs-sign-extension predicate, ZF/SF off the sized result, PF over
 * the low byte, and the AF=0 that flags.c:put_arith pins — was unobserved by
 * any golden. The liveness pass makes that hole self-concealing: with no reader
 * of the flags, `need` is 0 and emit_imul emits no flag code whatsoever, so a
 * wrong predicate costs nothing to hide. Every helper here reads the flags back
 * through PUSHF, which (PUSHF is not in the flags_live.c whitelist, so it
 * defaults to use=ALL) forces need=ALL and makes the full predicate real.
 *
 * ---------------------------------------------------------------------------
 * THE TWO CHECKSUMS, AND WHY THERE ARE TWO. This is the whole subtlety of
 * testing IMUL flags, and getting it wrong produces a test that looks strict
 * and is actually just false.
 *
 * x86 defines only CF and OF after IMUL/MUL. SF, ZF, AF and PF are
 * architecturally UNDEFINED. src/flags.c pins them to fixed deterministic
 * values on purpose (its header: "Architecturally-undefined flags ... get fixed
 * deterministic values so the interpreter and JIT can be diffed bit-exactly").
 * That is an ocerz-internal convention and it is NOT required to match any
 * particular silicon. Measured here over a 121-case imul64 sweep, this
 * machine's native x86_64 execution and ocerz agree on CF/OF in every single
 * case and disagree on PF/ZF/SF exactly where the result is zero or negative
 * (native leaves them clear; ocerz computes them from the result). Holding the
 * undefined bits to a native golden would therefore assert something false
 * about the contract — and a first draft of this file did exactly that and
 * "failed", which is the trap this comment exists to stop the next reader from
 * falling into.
 *
 * So the two observations are separated and gated by different mechanisms:
 *
 *   arch=   data + (flags & (CF|OF)) — ONLY the architecturally defined bits.
 *           This line's golden in expect/imul_flags.out is generated from the
 *           NATIVE x86_64 run, so it holds both ocerz tiers to real hardware.
 *           This is the line that catches a wrong overflow predicate.
 *
 *   pinned= data + all six arithmetic flags, including the undefined ones.
 *           This line's golden is ocerz's own value, and what enforces it is
 *           tests/run_diff_test.sh: the interpreter (src/interp.c op_mul, THE
 *           semantic reference) and the JIT must produce it identically. This
 *           is the line that catches emit_imul disagreeing with op_mul about a
 *           bit hardware does not care about but the project has decided to pin.
 *
 * Both are needed: `arch` alone cannot see a JIT/interp split on the pinned
 * bits, and `pinned` alone is self-referential and could ratify a predicate
 * that is simply wrong about overflow.
 * ---------------------------------------------------------------------------
 *
 * WHAT IS SWEPT. Every operand shape emit_imul accepts, and the ones it must
 * REFUSE and hand to the interpreter:
 *   - 2-op imul r64,r64 and imul r32,r32
 *   - 3-op imul r64,r64,imm8 / imm32 and imul r32,r32,imm8 / imm32
 *   - dst==src (imul rax,rax), pinning that both sources are read before the
 *     destination is committed
 *   - REFUSED, but still required to be bit-exact through the slow path:
 *     imul r16,r16 (size 2), imul r64,[mem] (also a use=ALL fault barrier), the
 *     1-operand rdx:rax form (nops < 2), and the unsigned 1-operand MUL that
 *     shares op_mul and the same def=ALL table entry.
 *
 * Values hit the exact CF/OF edges the predicate turns on:
 *   -1 * -1: the high half is all-ones and IS the sign extension of the low
 *     half -> CF/OF CLEAR. A MUL-style "high half nonzero" predicate says SET.
 *     This single case separates IMUL's predicate from MUL's.
 *   0x10000 * 0x7fff == 0x7fff0000 fits in int32 -> CLEAR, while
 *     0x10000 * 0x8000 == 0x80000000 does not -> SET. The size-4 boundary.
 *   INT_MIN * -1 and INT64_MIN * -1: product 2^63, high half zero but low half
 *     negative -> SET.
 *   zero (ZF), and byte parities either side (PF).
 * The 32-bit helper reads its result back as a full 64-bit register, so a
 * size-4 destination write that failed to zero-extend is caught too.
 *
 * MUTATION-TESTED — this is not a test that cannot fail. See the mutation log
 * in the review notes; each broken build of src/jit.c:emit_imul below was
 * caught here while alu.c and flags.c both stayed GREEN, which is exactly why
 * this file exists:
 *   1. size-4 CF/OF predicate replaced by MUL's "high half nonzero"
 *      -> arch= changes (caught against NATIVE hardware).
 *   2. size-4 sources zero-extended instead of sign-extended (ldrsw -> ldr)
 *      -> arch= changes.
 *   3. PF dropped from the emitted flags -> pinned= changes and the JIT/interp
 *      differential fails.
 */
#include "gsys.h"

/* CF PF AF ZF SF OF — the six arithmetic flags, masking out the always-set
 * fixed bit and IF/DF so the comparison is state-independent. Same mask
 * flags.c and flags_live.c use. */
#define ARITH_MASK 0x0cd5ULL
/* The only two x86 actually DEFINES after IMUL/MUL. */
#define CFOF_MASK  0x0801ULL

static g_u64 sum_arch;
static g_u64 sum_pinned;

/* Fold one (result, flags) observation into both checksums. The flags are
 * splattered into both halves of the word, as flags.c does, so they cannot be
 * masked off by a result that happens to share their low bits. */
static void obs(g_u64 r, g_u64 f)
{
    sum_arch = sum_arch * 1000003 + (r ^ ((f & CFOF_MASK) * 0x100000001ULL));
    sum_pinned = sum_pinned * 1000003 + (r ^ ((f & ARITH_MASK) * 0x100000001ULL));
}

/* ---- Shapes emit_imul INLINES ---------------------------------------------*/

/* 2-op, size 8: MUL + SMULH, CF/OF from hi != (lo asr 63). */
static void cap_imul64_rr(g_u64 a, g_u64 b)
{
    g_u64 r, f;
    __asm__ volatile("imulq %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "r"(b) : "cc");
    obs(r, f);
}

/* 2-op, size 4: both sources sign-extended through ldrsw, one x-form MUL holds
 * the exact product, and the destination write must ZERO-EXTEND to 64. r is
 * read back as a full 64-bit register on purpose. */
static void cap_imul32_rr(g_u32 a, g_u32 b)
{
    g_u64 r = 0, f;
    __asm__ volatile("imull %k3, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)b) : "cc");
    obs(r, f);
}

/* 3-op, size 8, imm8 (the decoder carries it sign-extended). */
static void cap_imul64_rri8(g_u64 a)
{
    g_u64 r, f;
    __asm__ volatile("imulq $-5, %2, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"(a) : "cc");
    obs(r, f);
}

/* 3-op, size 8, imm32 at both sign boundaries. */
static void cap_imul64_rri32(g_u64 a)
{
    g_u64 r, f;
    __asm__ volatile("imulq $0x7fffffff, %2, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"(a) : "cc");
    obs(r, f);
    __asm__ volatile("imulq $-2147483648, %2, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"(a) : "cc");
    obs(r, f);
}

/* 3-op, size 4, imm8. */
static void cap_imul32_rri8(g_u32 a)
{
    g_u64 r = 0, f;
    __asm__ volatile("imull $-5, %k2, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"((g_u64)a) : "cc");
    obs(r, f);
}

/* 3-op, size 4, imm32 = 0x80000000. As a size-4 immediate this sign-extends to
 * 0xffffffff80000000 — it is INT32_MIN, NOT +2^31. The case that separates
 * sign-extending the immediate by its own operand size from any zero-extending
 * shortcut in emit_imul_src. */
static void cap_imul32_rri32(g_u32 a)
{
    g_u64 r = 0, f;
    __asm__ volatile("imull $0x80000000, %k2, %k0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "r"((g_u64)a) : "cc");
    obs(r, f);
}

/* dst == src: imul rax,rax. Both sources must be read before the destination is
 * committed; a commit-then-read-second-source order squares the wrong value. */
static void cap_imul64_square(g_u64 a)
{
    g_u64 r, f;
    __asm__ volatile("imulq %0, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a) : "cc");
    obs(r, f);
}

/* ---- Shapes emit_imul must REFUSE (interpreter slow path) ------------------*/

/* size 2 — emit_imul rejects anything but size 4/8. */
static void cap_imul16_rr(unsigned short a, unsigned short b)
{
    g_u64 r = 0, f;
    __asm__ volatile("imulw %w3, %w0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"((g_u64)a), "r"((g_u64)b) : "cc");
    obs(r, f);
}

/* Memory source — refused by try_inline AND a use=ALL fault barrier. */
static void cap_imul64_rm(g_u64 a, g_u64 *p)
{
    g_u64 r, f;
    __asm__ volatile("imulq %3, %0\n\tpushfq\n\tpop %1"
                     : "=&r"(r), "=r"(f) : "0"(a), "m"(*p) : "cc");
    obs(r, f);
}

/* 1-operand IMUL: the rdx:rax widening form. Refused (nops < 2), so this pins
 * that the def=ALL table entry stays honest for the shape emit_imul never sees
 * — the entry is keyed on the OPCODE, not on whether the JIT inlined it. */
static void cap_imul64_1op(g_u64 a, g_u64 b)
{
    g_u64 lo, hi, f;
    __asm__ volatile("imulq %3\n\tpushfq\n\tpop %2"
                     : "=a"(lo), "=d"(hi), "=r"(f) : "r"(b), "0"(a) : "cc");
    obs(lo, f);
    obs(hi, 0);
}

/* 1-operand MUL: shares op_mul and the same flags_live entry, but its CF/OF
 * predicate is "high half nonzero", NOT IMUL's sign-extension test. Included so
 * a "fix" that collapses the two predicates cannot pass. */
static void cap_mul64_1op(g_u64 a, g_u64 b)
{
    g_u64 lo, hi, f;
    __asm__ volatile("mulq %3\n\tpushfq\n\tpop %2"
                     : "=a"(lo), "=d"(hi), "=r"(f) : "r"(b), "0"(a) : "cc");
    obs(lo, f);
    obs(hi, 0);
}

/* ---- The value sweep -------------------------------------------------------*/

#define S64_MIN 0x8000000000000000ULL
#define S64_MAX 0x7fffffffffffffffULL

/* Chosen for the CF/OF edge, not for volume. See the header. */
static const g_u64 vals64[] = {
    0, 1, 2, 3,
    0xffffffffffffffffULL,          /* -1: high half all-ones AND the sign
                                     * extension -> CF/OF CLEAR. The case a
                                     * "high half nonzero" predicate fails. */
    S64_MIN, S64_MAX,
    S64_MIN + 1,
    0x100000000ULL,                 /* 2^32: squared -> exactly 2^64 */
    0x80000000ULL,                  /* 2^31 * 2^32 == 2^63 -> lo negative,
                                     * hi zero -> CF/OF SET */
    0x7fffffffULL,
    0x5555555555555555ULL,
    0xaaaaaaaaaaaaaaaaULL,
    0x0f0f0f0f0f0f0f0fULL,
    0x00000000000000ffULL,          /* parity edges for PF (low byte) */
    0x000000000000007fULL,
    0x0000000100000001ULL,
    0xdeadbeefcafebabeULL,
};
#define N64 (sizeof(vals64) / sizeof(vals64[0]))

static const g_u32 vals32[] = {
    0, 1, 2, 3,
    0xffffffffu,                    /* -1 */
    0x80000000u, 0x7fffffffu,
    0x80000001u,
    0x00010000u,                    /* with 0x7fff -> 0x7fff0000, fits, CLEAR;
                                     * with 0x8000 -> 0x80000000, does NOT fit
                                     * as int32, SET. The size-4 predicate. */
    0x00007fffu,
    0x00008000u,
    0x0000ffffu,
    0x55555555u,
    0xaaaaaaaau,
    0x000000ffu,
    0x0000007fu,
    0xcafebabeu,
};
#define N32 (sizeof(vals32) / sizeof(vals32[0]))

int main(void)
{
    g_u64 mem;

    for (unsigned i = 0; i < N64; i++) {
        for (unsigned j = 0; j < N64; j++) {
            g_u64 a = vals64[i], b = vals64[j];
            cap_imul64_rr(a, b);
            cap_imul64_1op(a, b);
            cap_mul64_1op(a, b);
            mem = b;
            cap_imul64_rm(a, &mem);
        }
        cap_imul64_rri8(vals64[i]);
        cap_imul64_rri32(vals64[i]);
        cap_imul64_square(vals64[i]);
    }

    for (unsigned i = 0; i < N32; i++) {
        for (unsigned j = 0; j < N32; j++) {
            cap_imul32_rr(vals32[i], vals32[j]);
            cap_imul16_rr((unsigned short)vals32[i], (unsigned short)vals32[j]);
        }
        cap_imul32_rri8(vals32[i]);
        cap_imul32_rri32(vals32[i]);
    }

    /* arch=   defined bits only; golden generated from NATIVE x86_64.
     * pinned= all six; golden is ocerz's convention, enforced across tiers by
     *         tests/run_diff_test.sh. See the header. */
    g_puts("imul_flags arch=");
    g_puthex64(sum_arch);
    g_puts("imul_flags pinned=");
    g_puthex64(sum_pinned);
    return 0;
}
