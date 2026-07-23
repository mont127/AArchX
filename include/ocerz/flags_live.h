/*
 * include/ocerz/flags_live.h
 *
 * Per-instruction FLAG LIVENESS for the JIT: which of the six x86 arithmetic
 * flags an instruction writes (def) and which it may read (use).
 *
 * Why this exists. The JIT used to compute all six flags natively into
 * cpu->rflags for EVERY inlined ALU op. Measured on the alu benchmark's hot
 * block that is ~83% of the emitted words, and the overwhelming majority of
 * those writes are dead — overwritten by the next arithmetic op before anything
 * reads them. A standard backward liveness pass over the block lets the emitter
 * skip the dead ones. This header is the table that pass runs on.
 *
 * THE SAFETY CONTRACT, and it is the whole design:
 *
 *   def must be a SUBSET of what the instruction really writes.
 *   use must be a SUPERSET of what the instruction really reads.
 *
 * Both errors are one-directional. Under-claiming def or over-claiming use only
 * keeps a flag live that did not need to be — slower, never wrong. Over-claiming
 * def kills a producer whose flag is still read, and over-claiming... i.e.
 * under-claiming use lets a consumer read a flag that was never computed. Both
 * of those are silent wrong-answer bugs. So every entry here defaults to the
 * safe side and the table is a WHITELIST: anything not explicitly listed gets
 * def=0 (kills nothing) and use=ALL (reads everything), which reduces exactly to
 * today's eager behaviour.
 *
 * def is a property of the x86 op, NOT of whether the JIT inlines it. The
 * interpreter's put_arith writes all six for ADD at every size, so "ADD defs
 * ALL" is true whether that ADD was inlined or fell to the slow path. This is
 * what lets the table stay tier-independent.
 *
 * THE FAULT BARRIER. An instruction that can FAULT is a use=ALL barrier even if
 * it reads no flag architecturally. A fault delivers a guest signal, and the
 * handler is unpredicted control flow that may PUSHF and observe rflags. "Dead"
 * only ever meant "no guest instruction reads it before it is rewritten" — a
 * signal handler is not on that path and would see a stale value. So anything
 * touching memory, the stack, or trapping (DIV, SYSCALL, INT3) reads ALL. This
 * is why the table keys off operand shape and not just the opcode.
 *
 * Note this costs the alu hot block NOTHING: it contains no memory operand at
 * all. It is what makes mem/call gain much less, and that is the honest tradeoff
 * — a barrier is correct by construction, where reconstructing flags from a
 * fault's mcontext would be an unverifiable claim about signal state.
 *
 * src/flags.c remains THE semantic reference; this table only describes it.
 */
#ifndef OCERZ_FLAGS_LIVE_H
#define OCERZ_FLAGS_LIVE_H

#include "ocerz/decode.h"
#include "ocerz/flags.h"

/* The six arithmetic flags, the only ones this pass reasons about. Control bits
 * (DF/IF/TF and the fixed bit 1) are never touched by the emitters and are
 * outside the model entirely. */
#define OCERZ_FL_ALL (OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF)

/* Fill *def with the flags insn is guaranteed to write and *use with the flags
 * it may read (including the fault barrier described above). Total: every
 * X86Insn gets an answer, and the answer for anything unrecognized is the
 * conservative def=0 / use=OCERZ_FL_ALL. */
void ocerz_flags_defuse(const X86Insn *insn, uint64_t *def, uint64_t *use);

/* Same architectural def/use answer without the synthetic synchronous-fault
 * use=ALL barrier. The JIT may use this only when it has installed an exact
 * cold fault-reconstruction recipe for that specific inlined memory access. */
void ocerz_flags_defuse_nofault(const X86Insn *insn, uint64_t *def, uint64_t *use);

#endif /* OCERZ_FLAGS_LIVE_H */
