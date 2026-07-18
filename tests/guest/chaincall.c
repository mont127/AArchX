/*
 * tests/guest/chaincall.c
 *
 * ATTACK F gate: register-map correctness across a CHAINED CALL(imm) edge whose
 * SOURCE and TARGET blocks pin DIFFERENT guest GPRs.
 *
 * Block chaining (src/jit.c) chains a direct CALL(imm) terminator straight into
 * its callee's host entry via emit_chain_tail. The correctness claim is that the
 * seam is safe for ANY source/target pin-map pair because emit_chain_tail flushes
 * every SOURCE pinned reg to cpu->gpr[] (emit_spill_pinned), restores the
 * callee-saved host regs x21..x28 the source held (emit_pin_epilogue_restore),
 * tears the source frame down, and only THEN branches to the callee entry -- whose
 * own prologue rebuilds the frame and reloads ITS pins from cpu->gpr[]. State
 * therefore crosses the edge entirely through memory; no register is assumed
 * shared between the two pin maps.
 *
 * The existing chained-CALL coverage (fib / bench 'call') does NOT exercise this:
 * its blocks are far under OCERZ_PIN_MIN_INSNS (24), so both maps are EMPTY and
 * emit_chain_tail's spill/restore are no-ops. This program forces BOTH the caller
 * block (loop top .. the `call`) and the callee block to exceed the pin gate with
 * heavy, DISJOINT register pressure, so the source pins one set of x21..x28 slots
 * and the callee pins a different set. A bug in the flush/restore/frame-balance of
 * emit_chain_tail (e.g. dropping the source spill, or an unbalanced ldp) would
 * corrupt a live value and diverge the JIT result from the interpreter -- and from
 * native x86 hardware (the checked-in golden was produced by the native run).
 *
 * Deterministic single decimal line; exit 0. Verified native == interp == JIT and
 * identical on both OCERZ_NO_CHAIN arms.
 */
#include "gsys.h"

/* The CALLEE. noinline + a real return value force clang to emit a `call rel32`
 * (the direct/imm CALL the chainer handles) rather than inlining. The body is a
 * long dependent ALU chain over its six parameters so the block clears the 24-insn
 * pin gate and pins a register set driven by a,b,c,d,e,f -- deliberately different
 * from the caller's accumulators below. volatile-free but data-dependent on all
 * args so it cannot be constant-folded to a stub. */
__attribute__((noinline))
static g_u64 mix(g_u64 a, g_u64 b, g_u64 c, g_u64 d, g_u64 e, g_u64 f)
{
    a += b * 0x9e3779b97f4a7c15ULL;
    b ^= a >> 17;
    c += a + b;
    d ^= c << 3;
    e += d ^ (b + 0x1234567ULL);
    f ^= e + (a << 5);
    a -= f + (c >> 7);
    b += a ^ (d << 2);
    c ^= b + (e >> 11);
    d += c ^ (f << 1);
    e -= d + (a >> 13);
    f += e ^ (b << 4);
    a ^= f + (c << 6);
    b -= a + (d >> 9);
    c += b ^ (e << 7);
    d ^= c + (f >> 3);
    e += d ^ (a << 8);
    f -= e + (b >> 5);
    return a ^ b ^ c ^ d ^ e ^ f;
}

int main(void)
{
    /* The CALLER loop. Its accumulators (acc0..acc3, i) are a different working
     * set than mix()'s a..f, so the loop-top..call block pins a different subset
     * of x21..x28. A long ALU preamble each iteration lifts this block over the
     * pin gate too, so the chained edge really is pinned-source -> pinned-target
     * with disjoint maps. The `call mix(...)` in the middle ends the block at a
     * direct CALL(imm): exactly the chained terminator under test. */
    g_u64 acc0 = 0x0123456789abcdefULL;
    g_u64 acc1 = 0xfedcba9876543210ULL;
    g_u64 acc2 = 0xa5a5a5a55a5a5a5aULL;
    g_u64 acc3 = 0x0f1e2d3c4b5a6978ULL;
    g_u64 out = 0;

    for (g_u64 i = 0; i < 4000; i++) {
        acc0 += i * 3 + 1;
        acc1 ^= acc0 + (i << 2);
        acc2 -= acc1 ^ (acc0 >> 5);
        acc3 += acc2 + (acc1 << 1);
        acc0 ^= acc3 + (i >> 1);
        acc1 += acc2 ^ (acc3 << 3);
        acc2 ^= acc0 + (acc3 >> 7);
        acc3 -= acc1 + (acc2 << 2);
        acc0 += acc2 ^ (acc1 >> 9);
        acc1 ^= acc3 + (acc0 << 4);
        acc2 += acc0 ^ (i << 6);
        acc3 ^= acc1 + (acc2 >> 11);
        /* the chained direct CALL: source pins {acc*}, target pins {a..f} */
        out += mix(acc0, acc1, acc2, acc3, i, out);
        acc0 ^= out;
        acc1 += out >> 3;
        acc2 -= out << 1;
        acc3 ^= out + i;
    }

    g_putu64(out ^ acc0 ^ acc1 ^ acc2 ^ acc3);
    return 0;
}
