/*
 * Synthesized x86_64 system libraries for native mode.
 *
 * A guest that links /usr/lib/libSystem.B.dylib or CoreFoundation expects to
 * find an x86_64 Mach-O there.  On a machine with no Intel system libraries
 * left there is none, so ocerz builds one: a real Mach-O, header and load
 * commands and segments and an export trie, assembled in memory rather than
 * read off disk.  The loader cannot tell the difference, because it already
 * works out of a host buffer - map_segments memcpys segment contents out of
 * DynImage.slice, and the trie walker reads that same buffer - so a synthesized
 * image flows through mapping, import resolution, dlopen and dladdr unchanged.
 * That is the point of doing it this way rather than special-casing the
 * resolvers.  What a library exports is read from its file in the API database
 * (apidb.h) rather than compiled in, so ocerz_vdylib_have answers yes exactly
 * for the install names that have a file, and the builder treats every library
 * alike, however many exports its file declares.
 *
 * Every function export is twelve bytes of real x86 in the image's own __TEXT:
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
 * An export id is the library's position among the database's files in its top
 * twelve bits and the export's index in that file in the low twenty, so the
 * same database gives the same export the same id and the same stub bytes in
 * every process, whichever library a guest happens to load first.  The
 * dispatcher hands the call to src/bridge.c, which performs it for real when
 * the export has a descriptor and names the export and stops when it does not.
 *
 * Not every export is a stub.  A var record, such as the ___stack_chk_guard
 * canary a stack-protected program reads in every function prologue, is a slot
 * in __DATA holding its value, and the export trie points straight at it.
 *
 * And not every data export is a slot.  A CFSTR literal the compiler lays down
 * in the guest's own image carries the address of
 * ___CFConstantStringClassReference as its isa, and a guest hands
 * &kCFTypeArrayCallBacks to a CoreFoundation that may compare it with the
 * address of its own; a copy in __DATA would be a second object at a second
 * address that the native framework does not recognize.  So a data record's
 * export is the host's variable itself: an absolute export trie entry whose
 * value is the address ocerz_bridge_host_symbol finds for the record's host
 * symbol in the library the image stands for.  Native mode runs in an identity address map, so that host
 * address is already a valid guest address, and the loader binds the guest's
 * GOT entry or isa word straight to it.  A name the host does not have is left
 * out of the trie, so a guest importing it fails to bind and says which name it
 * wanted instead of running with a reference to address zero.
 *
 * ocerz_vdylib_image resolves native data through ocerz_bridge_host_symbol;
 * ocerz_vdylib_image_with takes the lookup as an argument, which is how an
 * image is built and checked against addresses the host does not have.
 * ocerz_vdylib_export_name turns an export id back into the library and symbol
 * it was minted for, the same pair the dispatcher hands the bridge.
 */
#ifndef OCERZ_VDYLIB_H
#define OCERZ_VDYLIB_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

#define OCERZ_BRIDGE_UNIMPL_EXIT 72

struct OcerzVM;

typedef void *(*OcerzVdylibHostSym)(const char *install_name, const char *host_sym);

int ocerz_vdylib_have(const char *install_name);
uint8_t *ocerz_vdylib_image(const char *install_name, size_t *len_out);
uint8_t *ocerz_vdylib_image_with(const char *install_name, OcerzVdylibHostSym host_sym,
                                 size_t *len_out);
int ocerz_vdylib_export_name(uint64_t id, const char **lib_out, const char **sym_out);
int ocerz_vdylib_dispatch(struct OcerzVM *vm, OcerzCPU *cpu);

#endif
