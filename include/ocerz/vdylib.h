/*
 * Synthesized x86_64 system libraries for native mode.
 *
 * A guest that links /usr/lib/libSystem.B.dylib expects to find an x86_64
 * Mach-O there.  On a machine with no Intel system libraries left there is
 * none, so ocerz builds one: a real Mach-O, header and load commands and
 * segments and an export trie, assembled in memory rather than read off disk.
 * The loader cannot tell the difference, because it already works out of a
 * host buffer - map_segments memcpys segment contents out of DynImage.slice,
 * and the trie walker reads that same buffer - so a synthesized image flows
 * through mapping, import resolution, dlopen and dladdr unchanged.  That is
 * the point of doing it this way rather than special-casing the resolvers.
 *
 * Every export is twelve bytes of real x86 in the image's own __TEXT:
 *
 *     41 bb <id32>        mov r11d, <export id>
 *     ff 25 <rel32>       jmp qword [rip + rel32]
 *
 * where the slot the jump reads lives in __DATA and holds one address for the
 * whole process, OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF, inside the trap window
 * the decoder, the JIT and the interpreter already watch for.  So a call into
 * a virtual framework needs no new instruction, no new range check and no
 * widened window: the guest jumps to an address that traps, and r11 says which
 * export it was.  The slot is written at build time and never relocated, since
 * the trap address is a constant and the jump is rip-relative, so a synthesized
 * image needs no fixups at all.
 *
 * Until the bridges exist, dispatching one names the export and stops.
 */
#ifndef OCERZ_VDYLIB_H
#define OCERZ_VDYLIB_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

#define OCERZ_BRIDGE_UNIMPL_EXIT 72

struct OcerzVM;

int ocerz_vdylib_have(const char *install_name);
uint8_t *ocerz_vdylib_image(const char *install_name, size_t *len_out);
int ocerz_vdylib_dispatch(struct OcerzVM *vm, OcerzCPU *cpu);

#endif
