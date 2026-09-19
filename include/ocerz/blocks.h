/*
 * Blocks crossing between x86 guest code and the host's native arm64 code.
 *
 * A block is an object whose third word is the function that runs it, called
 * with the block itself as its first argument, and whose fifth word is a
 * descriptor holding its size, the helpers that copy and dispose of what it
 * captured, and its signature as an Objective-C type encoding.  The layout, the
 * flags and the signature strings are the same on x86_64 and arm64, so a block
 * made on one side is a well-formed block on the other in every word but the
 * functions: a guest block's invoke and helpers are x86 code, a native block's
 * are arm64 code.  In native mode the guest's __NSConcreteStackBlock,
 * __NSConcreteMallocBlock and __NSConcreteGlobalBlock bind to the native
 * classes, so a guest block is also a native Objective-C object.
 *
 * ---- a guest block handed to native code ----
 * ocerz_block_to_native gives native code a block of its own for a guest block
 * gblock: a wrapper whose isa, flags and descriptor are native, whose invoke is
 * a slot of the callback bank bound to the guest block's invoke under the
 * block's signature, and whose one captured word is the guest block.  The
 * slot's first argument is class k, so when native code calls the wrapper the
 * dispatcher hands the guest invoke the guest block, not the wrapper, and the
 * guest function reads its captures where it expects them.  The signature is
 * the block's own; declared, a notation in the k{...} form without its first
 * argument, is used only when the block has none, and a block with neither is
 * refused.  A global guest block gets a global wrapper, made once and kept for
 * the life of the process.  A heap guest block gets a heap wrapper that holds a
 * reference to it, and the same heap block gets the same wrapper for as long as
 * the wrapper lives: the wrapper is looked up by the guest block's address and
 * retained only if its count has not already reached zero.  A stack guest block
 * is first copied to the heap guest-side, as _Block_copy would, its x86 copy
 * helper running as guest code, and the copy is wrapped.  A null block is null,
 * a native block passes through, and a wrapper made the other way is unwrapped
 * to the native block inside it.  *owned is a reference to the wrapper that
 * belongs to the caller, or zero: a crossing drops it with ocerz_block_release
 * once the call is over, so a wrapper native code did not copy dies then and
 * one it copied lives until native code releases it, when its dispose helper
 * releases the guest block.
 *
 * ---- a native block handed to guest code ----
 * ocerz_block_to_guest is the mirror image.  The guest's view of a native
 * block is a heap block whose invoke is an x86 trampoline in a page ocerz
 * writes into guest memory (ocerz_vdylib_trampoline), whose descriptor is
 * native, and whose one captured word is a reference to the native block,
 * taken with the native _Block_copy so that a native stack block is copied
 * before the guest can keep it.  The guest calls it the way compiled code calls
 * any block, through the word at offset 16 with the block in rdi, or in rsi when
 * a structure result's pointer comes first; the trampoline traps, and
 * ocerz_block_invoke_trap calls the native block's invoke under the native
 * block's own signature, whose first argument, class k, turns the guest view
 * back into the native block.  A native global block gets a global view kept for
 * the life of the process, and a native heap block the same view while the view
 * lives.  A wrapper of a guest block is unwrapped, and a block whose invoke is
 * already guest code passes through.
 *
 * Both kinds of wrapper are heap or global blocks with native copy and dispose
 * helpers, so the native _Block_copy, _Block_release, objc_retain and
 * objc_release are correct on them from either side, and they hold only a
 * reference to what they wrap.  A wrapper is recognized by its descriptor's
 * dispose helper for a native wrapper and by its invoke for a guest view.
 *
 * ---- results ----
 * ocerz_block_result_to_guest converts a native call's block result.  borrowed
 * zero says the result carries a reference that is now the guest's, as a
 * function declared DISPATCH_RETURNS_RETAINED_BLOCK does: the view takes the
 * reference's place.  borrowed non-zero says the result is borrowed, as a
 * getter's is: the view holds its own reference and is autoreleased, so it
 * lives until the pool around the call drains unless the guest retains it.
 * ocerz_block_result_to_native converts a guest callback's block result, which
 * is borrowed by the same convention, into an autoreleased wrapper.
 *
 * ---- the guest's block runtime ----
 * Native mode has no x86 libclosure, so the runtime functions a guest imports
 * are the native ones, and the native ones would call a guest stack block's x86
 * helpers as arm64 code.  So ocerz_block_copy_guest is _Block_copy with that
 * one case done guest-side: a stack block whose invoke is guest code is copied
 * into a malloc'd block, its descriptor replaced by a shadow whose copy and
 * dispose words are callback slots bound to the guest's helpers, and the copy
 * helper is run as guest code.  Every other block, heap or global or native,
 * goes to the native _Block_copy.  The shadow is what makes a guest heap block
 * safe in native hands: the native _Block_release that frees it calls its
 * dispose helper through the slot, as guest code.  _Block_object_assign is the
 * same for the two cases that copy: a block field goes through
 * ocerz_block_copy_guest, and a __block variable still on the stack whose
 * keep and destroy helpers are guest code is moved to the heap guest-side, with
 * the heap copy's helpers slots in the same way.  _Block_release,
 * _Block_object_dispose and every other case cross to the native functions
 * unchanged.  ocerz_block_special_copy answers _Block_copy and objc_retainBlock
 * and ocerz_block_special_object_assign answers _Block_object_assign.
 *
 * ocerz_block_is_guest answers whether a block's invoke is guest code, a guest
 * view included.  ocerz_block_is_guest_object answers, for any object pointer,
 * whether it is a guest block and not a guest view: its isa is one of the three
 * native block classes, which a block's isa always is exactly, and its invoke
 * is guest code.  A send asks it of an argument whose encoding says only @,
 * because some classes add methods at run time whose encoding says @ where the
 * receiver's method signature says @?, as an NSXPCConnection proxy does.  ocerz_block_native_wrapper and ocerz_block_guest_view answer
 * whether a block is one of the two kinds of wrapper and, if so, what it wraps.
 * ocerz_block_invoke_notation turns a block signature, or when there is none a
 * declared notation, into the notation its invoke function is called under,
 * with the block itself as a first argument of class k.
 */
#ifndef OCERZ_BLOCKS_H
#define OCERZ_BLOCKS_H

#include "ocerz/types.h"
#include "ocerz/cpu.h"

struct OcerzVM;

#define OCERZ_BLOCK_NOTATION_MAX 256

int ocerz_block_to_native(uint64_t gblock, const char *declared, uint64_t *out, uint64_t *owned);
int ocerz_block_to_guest(uint64_t nblock, const char *declared, uint64_t *out, uint64_t *owned);
int ocerz_block_result_to_guest(uint64_t nblock, const char *declared, int borrowed, uint64_t *out);
int ocerz_block_result_to_native(uint64_t gblock, const char *declared, uint64_t *out);
void ocerz_block_release(uint64_t block);

int ocerz_block_is_guest(uint64_t block);
int ocerz_block_is_guest_object(uint64_t obj);
int ocerz_block_native_wrapper(uint64_t block, uint64_t *inner);
int ocerz_block_guest_view(uint64_t block, uint64_t *inner);
int ocerz_block_invoke_notation(const char *encoding, const char *declared, char *out, size_t outlen);

uint64_t ocerz_block_copy_guest(uint64_t gblock);
int ocerz_block_invoke_trap(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_block_special_copy(struct OcerzVM *vm, OcerzCPU *cpu);
int ocerz_block_special_object_assign(struct OcerzVM *vm, OcerzCPU *cpu);

#endif
