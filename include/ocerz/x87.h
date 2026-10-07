/*
 * The x87 unit's entry points for the rest of ocerz: the interpreter's
 * dispatch, FXSAVE/XSAVE and their restores, and the bridge, which pushes a
 * long double a native call returned.  src/x87.c explains the register model.
 */
#ifndef OCERZ_X87_H
#define OCERZ_X87_H

#include "ocerz/cpu.h"
#include "ocerz/decode.h"

int ocerz_x87_exec(OcerzCPU *cpu, const X86Insn *insn);

void ocerz_x87_push(OcerzCPU *cpu, double v);
uint16_t ocerz_x87_fsw(const OcerzCPU *cpu);
void ocerz_x87_reset(OcerzCPU *cpu);

/* The x87 part of an FXSAVE image: bytes 0-23 and the eight ST slots at 32. */
void ocerz_x87_fxsave(const OcerzCPU *cpu, uint64_t ea);
void ocerz_x87_fxrstor(OcerzCPU *cpu, uint64_t ea);

/* An 80-bit register image, from physical register p or into it. */
void ocerz_x87_to_f80(const OcerzCPU *cpu, int p, uint8_t out[10]);
void ocerz_x87_from_f80(OcerzCPU *cpu, int p, const uint8_t in[10]);

/* The double an image rounds to, as the register file keeps it; the JIT's constant loads use it. */
uint64_t ocerz_x87_f80_dbits(uint64_t mant, unsigned se);

#endif
