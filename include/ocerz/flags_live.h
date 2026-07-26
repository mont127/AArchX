/* Per-instruction flag liveness for the JIT: what each instruction defs and uses. */
#ifndef OCERZ_FLAGS_LIVE_H
#define OCERZ_FLAGS_LIVE_H

#include "ocerz/decode.h"
#include "ocerz/flags.h"

#define OCERZ_FL_ALL (OCERZ_CF | OCERZ_PF | OCERZ_AF | OCERZ_ZF | OCERZ_SF | OCERZ_OF)

void ocerz_flags_defuse(const X86Insn *insn, uint64_t *def, uint64_t *use);

void ocerz_flags_defuse_nofault(const X86Insn *insn, uint64_t *def, uint64_t *use);

#endif
