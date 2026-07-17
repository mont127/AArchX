/*
 * tests/guest/flags_carry.c
 *
 * A second gate for LAZY FLAG EMISSION (src/flags_live.c + the backward
 * liveness pass in src/jit.c:translate), targeting the shapes neither existing
 * test can reach.
 *
 * WHY tests/guest/flags.c CANNOT COVER THIS. It checks per-op flag exactness by
 * capturing with `pushfq` IMMEDIATELY after the op under test:
 *
 *     2: inc %0 ; pushfq ; pop %1
 *
 * PUSHF is not in the def/use whitelist, so it is modelled use=ALL. That forces
 * all six flags live at the op under test, and the liveness pass degrades to
 * eager emission. Every capture in that file is therefore blind to liveness by
 * construction: it pins the VALUES but exercises need==def on every single op.
 *
 * WHAT flags_live.c COVERS, and what it leaves. It gates the two structural
 * rules — live-out=ALL across a block boundary, and the fault barrier. Both are
 * about flags being read where the pass did not look. Neither touches the
 * per-op def/use table itself, and that table has one entry its own header
 * calls a trap:
 *
 *     INC/DEC def = ALL & ~CF, use = 0
 *
 * because flags.c:ocerz_flags_inc folds `cpu->rflags & OCERZ_CF` back into the
 * value it stores. CF is PRESERVED, not written. Model INC as the obvious
 * def=ALL and the pass concludes a producer's CF is dead across it, the emitter
 * skips computing CF, emit_incdec never writes CF back (its commit mask
 * excludes CF by construction), and a later ADC reads a stale bit. That is a
 * silent wrong answer, and no test in the corpus has the shape to see it: it
 * needs a CF producer, an INC/DEC between, and an ADC/SBB consumer, in ONE
 * block, with no PUSHF and no memory access anywhere between them to force the
 * flag live for unrelated reasons.
 *
 * Every function below is a single basic block by construction — register
 * operands only, no branches, no memory, no calls between producer and
 * consumer — so the liveness pass sees the whole chain and is free to kill.
 * SETcc is used only where noted, and never between a producer and the
 * consumer under test, because SETcc is use=ALL and would mask the very thing
 * being measured.
 *
 * Results fold into a printed checksum: the JIT-vs-interpreter differential
 * (tests/run_diff_test.sh) is the enforcement, exactly as for the rest of the
 * corpus. src/interp*.c + src/flags.c are the reference.
 */
#include "gsys.h"

static g_u64 mix(g_u64 acc, g_u64 v)
{
    return acc * 1000003u + v;
}

/* ---- 1. CF must survive INC and DEC to reach an ADC --------------------------
 * THE trap entry. addq defines CF; incq/decq preserve it; adcq reads it. If
 * INC/DEC were modelled def=ALL, the addq's CF would be proven dead here and
 * never computed, and the adcq would consume a stale bit. No PUSHF, no memory,
 * no branch: the pass sees the entire chain and would kill if the table let it.
 */
static g_u64 t_cf_through_incdec(g_u64 a, g_u64 b, g_u64 c, g_u64 d)
{
    g_u64 x = a, y = c, z = d, acc = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"   /* defines all six, CF is the one that matters */
        "incq %[y]\n\t"         /* preserves CF, rewrites the other five */
        "decq %[z]\n\t"         /* preserves CF */
        "adcq $0, %[acc]\n\t"   /* READS CF, from the addq, across both */
        : [x] "+r"(x), [y] "+r"(y), [z] "+r"(z), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return x * 7 + y * 11 + z * 13 + acc * 17;
}

/* ---- 2. Same, with the borrow form ----------------------------------------- */
static g_u64 t_cf_through_incdec_sbb(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = a, y = c, acc = 0;
    __asm__ volatile(
        "subq %[b], %[x]\n\t"   /* defines CF (borrow) */
        "incq %[y]\n\t"         /* preserves CF */
        "sbbq $0, %[acc]\n\t"   /* READS CF */
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return x * 3 + y * 5 + acc * 19;
}

/* ---- 3. A logic op that KILLS CF must actually write CF=0 --------------------
 * xorq defines all six; flags.c:ocerz_flags_logic put_arith()s szp_bits only, so
 * CF/OF/AF land as 0. Here CF is live (the adcq reads it), so the emitter must
 * compute that zero rather than leave the addq's 1 behind — this is the
 * "clears exactly the bits in need" path of emit_arith for an op whose flag
 * value is a constant zero rather than something OR'd into the accumulator.
 */
static g_u64 t_logic_kills_cf(g_u64 a, g_u64 b)
{
    g_u64 x = a, t = 0, acc = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"   /* CF likely 1 */
        "xorq %[t], %[t]\n\t"   /* defines all six: CF=OF=AF=0, ZF=1 */
        "adcq $0, %[acc]\n\t"   /* must read the xor's CF=0, not the add's */
        : [x] "+r"(x), [t] "+r"(t), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return x * 3 + t * 5 + acc * 23;
}

/* ---- 4. Flag-free ops must neither kill nor clobber CF -----------------------
 * NOT is the other named trap in the table: an ALU op that writes NO flags
 * (unlike NEG, which writes all six). BSWAP/LEA/MOVZX likewise. All are
 * def=0/use=0, so CF must flow from the addq straight through to the adcq. If
 * any of them wrongly claimed a def, the addq's CF would be killed.
 */
static g_u64 t_flagfree_passthrough(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = a, y = c, acc = 0, w = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"       /* CF */
        "notq %[y]\n\t"             /* writes no flags at all */
        "bswapq %[y]\n\t"           /* writes no flags */
        "leaq 1(%[y]), %[y]\n\t"    /* writes no flags, cannot fault */
        "movzbl %b[y], %k[w]\n\t"   /* writes no flags */
        "adcq $0, %[acc]\n\t"       /* CF, from the addq, across all four */
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc), [w] "=&r"(w)
        : [b] "r"(b)
        : "cc");
    return x * 3 + y * 5 + acc * 29 + w * 31;
}

/* ---- 5. NEG does define all six ---------------------------------------------
 * The counterpart to NOT, and the reason the two must not be conflated: negq
 * kills the addq's CF and supplies its own.
 */
static g_u64 t_neg_defines_cf(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = a, y = c, acc = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"   /* CF */
        "negq %[y]\n\t"         /* defines all six, CF = (y != 0) */
        "adcq $0, %[acc]\n\t"   /* must read the neg's CF */
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return x * 3 + y * 5 + acc * 37;
}

/* ---- 6. Shift by an immediate 0 touches nothing ------------------------------
 * src/jit.c:emit_shift returns early for a masked count of 0 and emits NOTHING
 * — no result store, no flags — and src/interp.c early-outs to match. The table
 * mirrors that with def=0/use=0. If it instead claimed def=ALL for a 0 count,
 * the cmpq's ZF would be proven dead and the setz below would read a stale bit.
 * The shifts also must not disturb the value.
 */
static g_u64 t_shift_zero(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = c, z = 0;
    __asm__ volatile(
        "cmpq %[b], %[a]\n\t"   /* defines ZF */
        "shlq $0, %[x]\n\t"     /* no flags, no value change */
        "shrq $0, %[x]\n\t"
        "sarq $0, %[x]\n\t"
        "setz %%cl\n\t"         /* READS ZF, from the cmpq */
        "movzbq %%cl, %[z]\n\t"
        : [x] "+r"(x), [z] "=&r"(z)
        : [a] "r"(a), [b] "r"(b)
        : "cc", "rcx");
    return x * 3 + z * 41;
}

/* ---- 7. Shift by a nonzero immediate does define all six --------------------
 * Including the SHL OF = CF^SF(result) reconstruction, which reads CF and SF
 * back out of the emitter's flag accumulator.
 */
static g_u64 t_shift_defines(g_u64 a, g_u64 b)
{
    g_u64 x = a, y = b, acc = 0;
    __asm__ volatile(
        "addq %[y], %[x]\n\t"   /* CF */
        "shlq $3, %[y]\n\t"     /* defines all six, CF from the shifted-out bit */
        "adcq $0, %[acc]\n\t"   /* must read the shl's CF */
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc)
        :
        : "cc");
    return x * 3 + y * 5 + acc * 43;
}

/* ---- 8. Multi-precision ADC / SBB chains ------------------------------------
 * Each ADC both uses CF and defines all six, so every link is simultaneously a
 * consumer of its predecessor and a killer of everything else. This is the
 * canonical shape real code uses to move carry, and the one where an off-by-one
 * in the dataflow (killing before reading, i.e. computing fl_need from the
 * post-def live set) shows up immediately.
 */
static g_u64 t_adc_chain(g_u64 a0, g_u64 a1, g_u64 a2, g_u64 b0, g_u64 b1, g_u64 b2)
{
    g_u64 r0 = a0, r1 = a1, r2 = a2;
    __asm__ volatile(
        "addq %[b0], %[r0]\n\t"
        "adcq %[b1], %[r1]\n\t"
        "adcq %[b2], %[r2]\n\t"
        : [r0] "+r"(r0), [r1] "+r"(r1), [r2] "+r"(r2)
        : [b0] "r"(b0), [b1] "r"(b1), [b2] "r"(b2)
        : "cc");
    return r0 * 3 + r1 * 5 + r2 * 7;
}

static g_u64 t_sbb_chain(g_u64 a0, g_u64 a1, g_u64 a2, g_u64 b0, g_u64 b1, g_u64 b2)
{
    g_u64 r0 = a0, r1 = a1, r2 = a2;
    __asm__ volatile(
        "subq %[b0], %[r0]\n\t"
        "sbbq %[b1], %[r1]\n\t"
        "sbbq %[b2], %[r2]\n\t"
        : [r0] "+r"(r0), [r1] "+r"(r1), [r2] "+r"(r2)
        : [b0] "r"(b0), [b1] "r"(b1), [b2] "r"(b2)
        : "cc");
    return r0 * 3 + r1 * 5 + r2 * 7;
}

/* ---- 9. INC supplies OF/ZF/SF while the ADD still supplies CF ---------------
 * The two halves of the def=ALL&~CF split, read at once: seto observes the
 * INC's OF, and the adcq observes the ADD's CF, from the same block. A table
 * that got the split backwards (def=ALL, use=CF) would still pass #1 by keeping
 * CF live for the wrong reason; this pins that INC really does rewrite the
 * other five.
 */
static g_u64 t_inc_of_add_cf(g_u64 a, g_u64 b, g_u64 c)
{
    g_u64 x = a, y = c, acc = 0, o = 0;
    __asm__ volatile(
        "addq %[b], %[x]\n\t"   /* CF */
        "incq %[y]\n\t"         /* OF/ZF/SF/PF/AF, CF preserved */
        "adcq $0, %[acc]\n\t"   /* the addq's CF */
        "seto %%cl\n\t"         /* the adcq's OF */
        "movzbq %%cl, %[o]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc), [o] "=&r"(o)
        : [b] "r"(b)
        : "cc", "rcx");
    return x * 3 + y * 5 + acc * 47 + o * 53;
}

/* ---- 10. 32-bit forms -------------------------------------------------------
 * The emitters take a separate w-register path (a 32-bit op zero-extends), and
 * the table masks a shift count by 31 rather than 63 for them.
 */
static g_u64 t_cf_through_incdec32(g_u32 a, g_u32 b, g_u32 c)
{
    g_u32 x = a, y = c, acc = 0;
    __asm__ volatile(
        "addl %[b], %[x]\n\t"
        "incl %[y]\n\t"
        "decl %[y]\n\t"
        "adcl $0, %[acc]\n\t"
        : [x] "+r"(x), [y] "+r"(y), [acc] "+r"(acc)
        : [b] "r"(b)
        : "cc");
    return (g_u64)x * 3 + (g_u64)y * 5 + (g_u64)acc * 59;
}

/* ---- 11. THE LENGTH-CAP BLOCK: the only shape that gates live-out=ALL -------
 * src/jit.c:translate seeds the backward pass with live_out = ALL for a block's
 * last instruction. That seed is the pass's whole soundness argument, and it is
 * load-bearing in exactly ONE case, which is not the obvious one.
 *
 * A block almost always ends on a terminator, and EVERY terminator (JMP, JCC,
 * CALL, RET, SYSCALL — see is_terminator) is absent from the def/use whitelist,
 * so each is modelled use=ALL. That use immediately re-lives all six flags no
 * matter what the seed was. So a block ending on a terminator is insensitive to
 * the seed: replacing OCERZ_FL_ALL with 0 changes nothing, and a cross-block
 * producer->consumer test whose blocks are split by a JMP therefore CANNOT gate
 * this rule — it passes for the terminator's reason, not the seed's.
 *
 * The seed is observable only when a block ends WITHOUT a terminator: the
 * JIT_MAX_BLOCK_INSNS (256) length cap. Then the last instruction can be a flag
 * producer with use=0, its need is def & seed, and a seed of 0 silently drops
 * the flags the SUCCESSOR block reads.
 *
 * So: 255 flag-free NOPs then a producer as instruction #256 — the cap's last —
 * with the consumer falling into the next block. Verified to be a real gate:
 * seeding live_out = 0 in translate() makes this diverge, while every other test
 * in the corpus (including flags_live's test_crossblock) still passes.
 */
static g_u64 t_lengthcap_liveout(g_u64 a, g_u64 b)
{
    g_u64 z = 0, c = 0;
    __asm__ volatile(
        /* Terminate the current block so the next one starts exactly here and
         * the instruction count below is measured from a known origin. */
        "jmp 1f\n"
        "1:\n\t"
        ".rept 255\n\t"
        "nop\n\t"
        ".endr\n\t"
        /* Instruction #256: the last of a length-capped block. Its flags are
         * emitted only because the seed says live-out is ALL. */
        "cmpq %[b], %[a]\n\t"
        /* First instructions of the SUCCESSOR block: they read those flags. */
        "setz %%cl\n\t"
        "movzbq %%cl, %[z]\n\t"
        "adcq $0, %[c]\n\t"
        : [z] "=&r"(z), [c] "+r"(c)
        : [a] "r"(a), [b] "r"(b)
        : "cc", "rcx");
    return z * 61 + c * 67;
}

int main(void)
{
    /* Operands chosen so CF, OF, ZF, SF each take BOTH values across the sweep:
     * a stale flag is only observable when the correct value differs from the
     * one left behind, so a sweep that never flips a bit cannot fail. */
    static const g_u64 vals[] = {
        0, 1, 2, 0xf, 0x10, 0x7f, 0x80,
        0x7fffffffULL, 0x80000000ULL, 0xffffffffULL,
        0x100000000ULL,
        0x7fffffffffffffffULL, 0x8000000000000000ULL,
        0xfffffffffffffffeULL, 0xffffffffffffffffULL,
    };
    const g_u64 nv = sizeof(vals) / sizeof(vals[0]);

    g_u64 acc = 0;

    for (g_u64 i = 0; i < nv; i++) {
        for (g_u64 j = 0; j < nv; j++) {
            g_u64 a = vals[i], b = vals[j];
            g_u64 c = vals[(i + j) % nv];
            g_u64 d = vals[(i + 2 * j) % nv];

            acc = mix(acc, t_cf_through_incdec(a, b, c, d));
            acc = mix(acc, t_cf_through_incdec_sbb(a, b, c));
            acc = mix(acc, t_logic_kills_cf(a, b));
            acc = mix(acc, t_flagfree_passthrough(a, b, c));
            acc = mix(acc, t_neg_defines_cf(a, b, c));
            acc = mix(acc, t_shift_zero(a, b, c));
            acc = mix(acc, t_shift_defines(a, b));
            acc = mix(acc, t_inc_of_add_cf(a, b, c));
            acc = mix(acc, t_adc_chain(a, b, c, d, a, b));
            acc = mix(acc, t_sbb_chain(a, b, c, d, a, b));
            acc = mix(acc, t_cf_through_incdec32((g_u32)a, (g_u32)b, (g_u32)c));
            acc = mix(acc, t_lengthcap_liveout(a, b));
        }
    }

    g_putu64(acc);
    return 0;
}
